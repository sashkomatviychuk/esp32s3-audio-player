#include "vs1053.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

static const char* TAG = "VS1053";

// ---- VS1053 pins (matches hardware.md, updated) ----
#define VS1053_PIN_XCS 7   // control CS (SCI)
#define VS1053_PIN_XDCS 6  // data CS (SDI)
#define VS1053_PIN_DREQ 5  // input, data request
#define VS1053_PIN_RESET 4

// ---- VS1053 SPI bus (own bus, separate from the SD card's SPI2) ----
#define VS1053_SPI_HOST SPI3_HOST
#define VS1053_PIN_SCLK 38
#define VS1053_PIN_MOSI 39  // VS1053 SI
#define VS1053_PIN_MISO 40  // VS1053 SO

// ---- SCI registers (standard VS1053 addresses) ----
#define SCI_MODE 0x00
#define SCI_STATUS 0x01
#define SCI_CLOCKF 0x03
#define SCI_AUDATA 0x05
#define SCI_VOL 0x0B
#define SCI_HDAT0 0x08
#define SCI_HDAT1 0x09

#define SM_SDINEW 0x0800
#define SM_LINE1 0x4000

#define VS1053_MAX_SDI_CHUNK 32

// ---- Module-private state ----
static spi_device_handle_t s_spi_low_speed = NULL;   // SCI, ~800kHz
static spi_device_handle_t s_spi_high_speed = NULL;  // SDI, ~6MHz
static SemaphoreHandle_t s_spi_mutex = NULL;         // passed in externally (main.c)

// -----------------------------------------------------------------
// Internal helpers for SCI (commands/registers). Called only from
// places where the mutex is NOT YET held by the caller (vs1053_init,
// vs1053_set_volume) — they take/give s_spi_mutex externally relative
// to these functions (see the call sites below).
// -----------------------------------------------------------------
#define DREQ_WAIT_TIMEOUT_MS 50

// Waits for DREQ to go high, with a bounded timeout instead of spinning
// forever — an unresponsive VS1053 (bad wiring, not powered, reset stuck)
// must not be allowed to hang the calling task and trip the watchdog.
static esp_err_t wait_dreq_high(const char* context) {
  int64_t deadline_us = esp_timer_get_time() + ((int64_t)DREQ_WAIT_TIMEOUT_MS * 1000);
  while (!gpio_get_level(VS1053_PIN_DREQ)) {
    if (esp_timer_get_time() > deadline_us) {
      ESP_LOGE(TAG, "%s: DREQ still low after %dms, VS1053 may be unresponsive", context,
               DREQ_WAIT_TIMEOUT_MS);
      return ESP_ERR_TIMEOUT;
    }
  }
  return ESP_OK;
}

static esp_err_t vs1053_write_sci_locked(uint8_t addr, uint16_t data) {
  esp_err_t dreq_ret = wait_dreq_high("vs1053_write_sci_locked");
  if (dreq_ret != ESP_OK) {
    return dreq_ret;
  }

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));

  gpio_set_level(VS1053_PIN_XCS, 0);

  t.flags |= SPI_TRANS_USE_TXDATA;
  t.cmd = 0x02;  // write
  t.addr = addr;
  t.tx_data[0] = (data >> 8) & 0xFF;
  t.tx_data[1] = data & 0xFF;
  t.length = 16;

  esp_err_t ret = spi_device_transmit(s_spi_low_speed, &t);

  gpio_set_level(VS1053_PIN_XCS, 1);

  return ret;
}

static esp_err_t vs1053_read_sci_locked(uint8_t addr, uint16_t* out_value) {
  esp_err_t dreq_ret = wait_dreq_high("vs1053_read_sci_locked");
  if (dreq_ret != ESP_OK) {
    return dreq_ret;
  }

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));

  gpio_set_level(VS1053_PIN_XCS, 0);

  t.flags |= SPI_TRANS_USE_RXDATA;
  t.cmd = 0x03;  // read
  t.addr = addr;
  t.length = 16;

  esp_err_t ret = spi_device_transmit(s_spi_low_speed, &t);

  gpio_set_level(VS1053_PIN_XCS, 1);

  if (ret != ESP_OK) {
    return ret;
  }

  *out_value = (uint16_t)(((t.rx_data[0] & 0xFF) << 8) | (t.rx_data[1] & 0xFF));
  return ESP_OK;
}

static long map_range(long x, long in_min, long in_max, long out_min, long out_max) {
  return ((x - in_min) * (out_max - out_min) / (in_max - in_min)) + out_min;
}

// -----------------------------------------------------------------
// Public interface
// -----------------------------------------------------------------
void vs1053_deselect_early(void) {
  gpio_config_t out_conf = {
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
      .pin_bit_mask = (1ULL << VS1053_PIN_XCS) | (1ULL << VS1053_PIN_XDCS),
  };
  ESP_ERROR_CHECK(gpio_config(&out_conf));

  gpio_set_level(VS1053_PIN_XCS, 1);
  gpio_set_level(VS1053_PIN_XDCS, 1);

  ESP_LOGI(TAG, "XCS/XDCS deselected early (before SD card mount)");
}

esp_err_t vs1053_init(SemaphoreHandle_t spi_mutex) {
  if (spi_mutex == NULL) {
    ESP_LOGE(TAG, "vs1053_init: spi_mutex is NULL");
    return ESP_ERR_INVALID_ARG;
  }
  s_spi_mutex = spi_mutex;

  // --- GPIO: XCS, XDCS, RESET as output; DREQ as input (no pull, see below) ---
  gpio_config_t out_conf = {
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
      .pin_bit_mask =
          (1ULL << VS1053_PIN_XCS) | (1ULL << VS1053_PIN_XDCS) | (1ULL << VS1053_PIN_RESET),
  };
  ESP_ERROR_CHECK(gpio_config(&out_conf));

  // DREQ is actively driven by the VS1053 (not open-drain), so it needs no
  // pull. A pull-down here would fight the chip's own high-Z state during
  // its internal boot (before firmware brings DREQ high), making it look
  // stuck low even once the chip is actually ready.
  gpio_config_t dreq_conf = {
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
      .pin_bit_mask = (1ULL << VS1053_PIN_DREQ),
  };
  ESP_ERROR_CHECK(gpio_config(&dreq_conf));

  // --- VS1053's own SPI bus (SPI3), independent from the SD card's SPI2 ---
  spi_bus_config_t bus_cfg = {
      .mosi_io_num = VS1053_PIN_MOSI,
      .miso_io_num = VS1053_PIN_MISO,
      .sclk_io_num = VS1053_PIN_SCLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 64,
  };
  esp_err_t ret = spi_bus_initialize(VS1053_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize (SPI3) failed: %s", esp_err_to_name(ret));
    return ret;
  }

  spi_device_interface_config_t devcfg_low = {
      .clock_speed_hz = 800000,  // SCI, slow, safe before clock configuration
      .command_bits = 8,
      .address_bits = 8,
      .dummy_bits = 0,
      .mode = 0,
      .spics_io_num = -1,  // XCS is driven manually via gpio_set_level
      .queue_size = 1,
      .flags = SPI_DEVICE_NO_DUMMY,
  };

  if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_FAIL;
  }

  ret = spi_bus_add_device(VS1053_SPI_HOST, &devcfg_low, &s_spi_low_speed);
  if (ret != ESP_OK) {
    xSemaphoreGive(s_spi_mutex);
    ESP_LOGE(TAG, "spi_bus_add_device (low speed) failed: %s", esp_err_to_name(ret));
    return ret;
  }

  spi_device_interface_config_t devcfg_high = devcfg_low;
  // Tried dropping this to 1MHz while HDAT0/HDAT1 stayed 0x0000 (decoder
  // never locking onto an MP3 frame) — made no difference, so the SDI
  // clock itself isn't the cause. Back to 6MHz.
  devcfg_high.clock_speed_hz = 6000000;  // SDI, after clock configuration in VS1053
  devcfg_high.command_bits = 0;
  devcfg_high.address_bits = 0;

  ret = spi_bus_add_device(VS1053_SPI_HOST, &devcfg_high, &s_spi_high_speed);
  if (ret != ESP_OK) {
    xSemaphoreGive(s_spi_mutex);
    ESP_LOGE(TAG, "spi_bus_add_device (high speed) failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // --- Reset sequence ---
  gpio_set_level(VS1053_PIN_XCS, 1);
  gpio_set_level(VS1053_PIN_XDCS, 1);
  gpio_set_level(VS1053_PIN_RESET, 0);
  vTaskDelay(pdMS_TO_TICKS(5));
  gpio_set_level(VS1053_PIN_RESET, 1);
  vTaskDelay(pdMS_TO_TICKS(20));

  // --- Base mode/clock/audata configuration ---
  uint16_t mode = 0;
  ret = vs1053_read_sci_locked(SCI_MODE, &mode);
  if (ret != ESP_OK) {
    xSemaphoreGive(s_spi_mutex);
    ESP_LOGE(TAG, "Failed to read SCI_MODE: %s", esp_err_to_name(ret));
    return ret;
  }
  if (mode != (SM_LINE1 | SM_SDINEW)) {
    ESP_LOGW(TAG, "SCI_MODE was 0x%04X, expected 0x%04X — reconfiguring", mode,
             (SM_LINE1 | SM_SDINEW));
    ret = vs1053_write_sci_locked(SCI_MODE, (SM_LINE1 | SM_SDINEW));
    if (ret != ESP_OK) {
      xSemaphoreGive(s_spi_mutex);
      ESP_LOGE(TAG, "Failed to write SCI_MODE: %s", esp_err_to_name(ret));
      return ret;
    }
  }

  ret = vs1053_write_sci_locked(SCI_AUDATA, 44101);  // 44.1kHz stereo
  if (ret != ESP_OK) {
    xSemaphoreGive(s_spi_mutex);
    ESP_LOGE(TAG, "Failed to write SCI_AUDATA: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = vs1053_write_sci_locked(SCI_CLOCKF, (6 << 12));  // multiplier 3.0 (~12.2MHz)
  if (ret != ESP_OK) {
    xSemaphoreGive(s_spi_mutex);
    ESP_LOGE(TAG, "Failed to write SCI_CLOCKF: %s", esp_err_to_name(ret));
    return ret;
  }

  xSemaphoreGive(s_spi_mutex);

  ESP_LOGI(TAG, "VS1053 initialized (XCS=%d XDCS=%d DREQ=%d RESET=%d)", VS1053_PIN_XCS,
           VS1053_PIN_XDCS, VS1053_PIN_DREQ, VS1053_PIN_RESET);

  return ESP_OK;
}

esp_err_t vs1053_write_sdi(const uint8_t* data, uint8_t bytes) {
  // ⚠️ The mutex is NOT taken here — the caller (send_to_codec() in
  // audio_task.c) already holds s_spi_mutex at the time of the call.
  if (bytes > VS1053_MAX_SDI_CHUNK) {
    ESP_LOGE(TAG, "vs1053_write_sdi: bytes > %d, ignoring", VS1053_MAX_SDI_CHUNK);
    return ESP_ERR_INVALID_ARG;
  }

  if (bytes == 0) {
    ESP_LOGW(TAG, "vs1053_write_sdi: called with 0 bytes, nothing to send");
    return ESP_OK;
  }

  esp_err_t dreq_ret = wait_dreq_high("vs1053_write_sdi");
  if (dreq_ret != ESP_OK) {
    return dreq_ret;
  }

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = (size_t)bytes * 8;
  t.tx_buffer = data;

  gpio_set_level(VS1053_PIN_XDCS, 0);
  esp_err_t ret = spi_device_transmit(s_spi_high_speed, &t);
  gpio_set_level(VS1053_PIN_XDCS, 1);

  return ret;
}

esp_err_t vs1053_log_decode_status(void) {
  if (s_spi_mutex == NULL) {
    ESP_LOGE(TAG, "vs1053_log_decode_status: vs1053_init() not called yet");
    return ESP_ERR_INVALID_STATE;
  }

  uint16_t status = 0;
  uint16_t hdat0 = 0;
  uint16_t hdat1 = 0;
  esp_err_t ret = ESP_FAIL;

  if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_FAIL;
  }

  ret = vs1053_read_sci_locked(SCI_STATUS, &status);
  if (ret == ESP_OK) {
    ret = vs1053_read_sci_locked(SCI_HDAT0, &hdat0);
  }
  if (ret == ESP_OK) {
    ret = vs1053_read_sci_locked(SCI_HDAT1, &hdat1);
  }

  xSemaphoreGive(s_spi_mutex);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "vs1053_log_decode_status: read failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // HDAT0/HDAT1 are refilled by the decoder from the last MPEG frame header
  // it parsed. Both staying 0x0000 while streaming means the decoder has
  // not locked onto a single valid MP3 frame in the SDI data it received.
  ESP_LOGI(TAG, "SCI_STATUS=0x%04X HDAT0=0x%04X HDAT1=0x%04X", status, hdat0, hdat1);
  return ESP_OK;
}

esp_err_t vs1053_set_volume(uint8_t vol) {
  if (s_spi_mutex == NULL) {
    ESP_LOGE(TAG, "vs1053_set_volume: vs1053_init() not called yet");
    return ESP_ERR_INVALID_STATE;
  }

  // 0..100 -> 0x00..0x20 per channel (0x00 - loudest, 0xFE - silence)
  uint16_t value = (uint16_t)map_range(vol, 0, 100, 0x20, 0x00);
  if (value == 0x20) {
    value = 0xFE;
  }
  value = (value << 8) | value;  // same for left and right channel

  // Taken INTERNALLY — unlike vs1053_write_sdi(), this function is NOT
  // called from the hot streaming loop, so it's responsible for
  // acquiring the mutex itself.
  esp_err_t ret = ESP_FAIL;
  if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
    ret = vs1053_write_sci_locked(SCI_VOL, value);
    xSemaphoreGive(s_spi_mutex);
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "Volume set to %d (SCI_VOL=0x%04X)", vol, value);
    } else {
      ESP_LOGE(TAG, "Failed to set volume: %s", esp_err_to_name(ret));
    }
  }
  return ret;
}
