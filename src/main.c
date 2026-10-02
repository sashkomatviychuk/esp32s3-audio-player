#include <inttypes.h>
#include <stdlib.h>

#include "audio_task.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "player_types.h"
#include "sdmmc_cmd.h"
#include "vs1053.h"

// Private ESP-IDF fatfs component header — not part of the public
// esp_vfs_fat.h API, but exposes esp_vfs_fat_sdspi_sdcard_init() and
// esp_vfs_fat_mount_initialized() as two separate steps. We need that split
// to patch card->csd between them (see the CSD workaround in mount_sdcard).
// NOT guaranteed stable across ESP-IDF versions — re-verify after upgrading.
#include "vfs_fat_internal.h"

static const char* TAG = "main";

// SD card / SPI bus (shared with VS1053) — matches hardware.md (updated)
#define PIN_NUM_MISO 11
#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK 12
#define PIN_NUM_CS 14
#define MOUNT_POINT "/sdcard"

static esp_err_t mount_sdcard(sdmmc_card_t** out_card, sdmmc_host_t* out_host) {
  // format_if_mount_failed is OFF: f_mkfs's multi-block write (CMD25 +
  // ACMD22) reliably fails on this card/reader over SPI (ESP_ERR_TIMEOUT on
  // sdmmc_send_cmd_num_of_written_blocks), even though single-block writes
  // (CMD24) work fine. Pre-format the card as FAT32 on a PC (use the SD
  // Association's SD Card Formatter for cards >32GB / if Windows defaults to
  // exFAT) instead of relying on on-device formatting.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 512};

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  // The SD/VS1053 SPI bus runs on ~15-20cm breadboard wiring, which was
  // unreliable at the default 20MHz (see sd-card-issues.md for the full
  // history). 1MHz was tried after fixing the VS1053 supply voltage, but
  // it reintroduced the same failure: mount succeeds, then a data CRC
  // error shows up mid-stream (sdspi_host: data CRC failed) and aborts
  // the read. Back to 100kHz, the speed this wiring actually holds up at.
  host.max_freq_khz = 100;
  // IMPORTANT: do NOT set SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC. It was tried
  // earlier to work around what looked like a bad CID/CSD checksum, but it
  // was masking a real failure: with CRC ignored, every data read/write
  // (including sector 0 / the MBR) silently returned ESP_OK full of garbage
  // instead of a CRC error. With CRC enabled, the card reads/writes
  // correctly and mounts fine — the data was never actually corrupted.

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = PIN_NUM_MISO,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };

  // The single SPI bus initialization for the whole project. VS1053
  // (via vs1053_init below) is added as a SEPARATE device on THIS
  // SAME bus, spi_bus_initialize is NOT called a second time anywhere.
  esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init SPI bus: %s", esp_err_to_name(ret));
    return ret;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = host.slot;
  // More headroom before giving up while polling MISO for "card ready"
  // (default is 40ms) — every command currently hits this timeout at least
  // once before the card responds.
  slot_config.wait_for_miso = 100;

  // not using ff_memalloc here, matching what esp_vfs_fat_sdspi_mount does
  // internally — we can't use that helper directly because it doesn't give
  // us a chance to patch card->csd between card_init and the FatFs mount
  // (see the CSD workaround below).
  sdmmc_card_t* card = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
  if (card == NULL) {
    ESP_LOGE(TAG, "Failed to allocate sdmmc_card_t");
    return ESP_ERR_NO_MEM;
  }

  bool host_inited = false;
  ret = esp_vfs_fat_sdspi_sdcard_init(&host, &slot_config, card, &host_inited);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_sdspi_sdcard_init failed: %s", esp_err_to_name(ret));
    free(card);
    return ret;
  }

  // WORKAROUND: this specific card/reader consistently returns a corrupted
  // CSD register over SPI (csd_ver=0, capacity=1024, sector_size=512 — i.e.
  // "512KB"), even with IGNORE_DATA_CRC and a very conservative 100kHz/100ms
  // wait_for_miso. Reads and writes both work fine against the real flash —
  // it's specifically the CSD decode that's wrong, so FatFs ends up
  // formatting/mounting only the first 512KB of a 16GB card. Until this is
  // root-caused (or the card/reader is swapped), override capacity/sector
  // size with a conservative estimate of the card's real size so the rest of
  // the flash is usable. 16GB card, marketing (decimal) GB, knocked down 5%
  // for margin: 16e9 bytes / 512 * 0.95 sectors.
  ESP_LOGW(TAG,
           "CSD capacity looked wrong (%d sectors), overriding to %d sectors for this 16GB card",
           card->csd.capacity, 29687500);
  card->csd.capacity = 29687500;
  card->csd.sector_size = 512;

  ret = esp_vfs_fat_mount_initialized(card, MOUNT_POINT, &mount_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_mount_initialized failed: %s", esp_err_to_name(ret));
    if (host_inited) {
      if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host.deinit_p(host.slot);
      } else {
        host.deinit();
      }
    }
    free(card);
    return ret;
  }

  ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
  sdmmc_card_print_info(stdout, card);

  *out_card = card;
  *out_host = host;
  return ESP_OK;
}

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  ESP_LOGI(TAG, "MP3 player starting up");

  // Deselect the VS1053 BEFORE touching the shared SPI bus at all — it sits
  // on the same MISO/MOSI/SCLK lines as the SD card, and until vs1053_init()
  // runs, its XCS/XDCS pins are floating (default GPIO input state), which
  // can make it think it's selected and corrupt SD card SPI traffic.
  vs1053_deselect_early();

  sdmmc_card_t* card;
  sdmmc_host_t host;

  if (mount_sdcard(&card, &host) != ESP_OK) {
    return;
  }

  // Shared resources for all tasks touching the SPI bus (SD + VS1053):
  // created here, in main.c, as the single owner, and passed in as
  // parameters to audio_task_init() / vs1053_init().
  QueueHandle_t cmd_queue = xQueueCreate(10, sizeof(player_cmd_t));
  SemaphoreHandle_t spi_mutex = xSemaphoreCreateMutex();

  if (cmd_queue == NULL || spi_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create cmd_queue/spi_mutex");
    return;
  }

  // VS1053 is added as a second SPI device on the ALREADY INITIALIZED
  // bus (host.slot) — spi_bus_initialize is NOT called again here.
  if (vs1053_init((spi_host_device_t)host.slot, spi_mutex) != ESP_OK) {
    ESP_LOGE(TAG, "vs1053_init failed");
    return;
  }

  if (audio_task_init(cmd_queue, spi_mutex, MOUNT_POINT) != ESP_OK) {
    ESP_LOGE(TAG, "audio_task_init failed");
    return;
  }

  // --- example: sending a command from outside
  //     (in the real project — from vInputTask / vBLETask,
  //     which will also receive cmd_queue as a parameter at init) ---
  // player_cmd_t cmd = { .type = CMD_PLAY_PAUSE };
  // xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
}
