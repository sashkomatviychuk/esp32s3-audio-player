#include "sd_card.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "player_state.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char* TAG = "sd_card";

// SD card / SPI2 bus (VS1053 has its own SPI3 bus) — matches hardware.md
#define PIN_NUM_MISO 11
#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK 12
#define PIN_NUM_CS 14
#define MOUNT_POINT "/sdcard"

#define SD_MAX_FREQ_KHZ 100        // see the speed history in try_mount()
#define SD_WAIT_FOR_MISO_MS 100    // "card ready" polling window (driver default is 40)
#define SD_MAX_OPEN_FILES 5
#define SD_ALLOCATION_UNIT_SIZE 512  // FAT/SD sector size
#define SPI_MAX_TRANSFER_SIZE 4000

// -----------------------------------------------------------------
// Module-private state (NOT extern, accessible only within this file)
// -----------------------------------------------------------------
static char s_tracks[SD_MAX_TRACKS][SD_MAX_NAME];
static int s_track_count = 0;
static sdmmc_card_t* s_card = NULL;  // non-NULL while the card is mounted
static bool s_bus_ready = false;     // SPI2 bus is initialized once, never freed
static bool s_published = false;     // tracks scanned and sd_present published for this mount

#define MONITOR_TASK_STACK_SIZE 4096
#define MONITOR_TASK_PRIORITY 1  // lowest: only retries mounts / probes the card
#define MONITOR_PERIOD_MS 2000

static esp_err_t init_bus(void) {
  if (s_bus_ready) {
    return ESP_OK;
  }

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = PIN_NUM_MISO,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = SPI_MAX_TRANSFER_SIZE,
  };

  // SPI2 bus for the SD card only. VS1053 initializes its own SPI3 bus in
  // vs1053_init(). The bus stays up when the card is removed, so re-mounting
  // does not have to initialize it again.
  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init SPI bus: %s", esp_err_to_name(ret));
    return ret;
  }
  s_bus_ready = true;
  return ESP_OK;
}

esp_err_t sd_card_try_mount(void) {
  if (s_card != NULL) {
    return ESP_OK;
  }

  esp_err_t ret = init_bus();
  if (ret != ESP_OK) {
    return ret;
  }

  // format_if_mount_failed is OFF: f_mkfs's multi-block write (CMD25 +
  // ACMD22) reliably fails on this card/reader over SPI (ESP_ERR_TIMEOUT on
  // sdmmc_send_cmd_num_of_written_blocks), even though single-block writes
  // (CMD24) work fine. Pre-format the card as FAT32 on a PC (use the SD
  // Association's SD Card Formatter for cards >32GB / if Windows defaults to
  // exFAT) instead of relying on on-device formatting.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = SD_MAX_OPEN_FILES,
      .allocation_unit_size = SD_ALLOCATION_UNIT_SIZE};

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  // The SD/VS1053 SPI bus runs on ~15-20cm breadboard wiring, which was
  // unreliable at the default 20MHz (see sd-card-issues.md for the full
  // history). 1MHz was tried after fixing the VS1053 supply voltage, but
  // it reintroduced the same failure: mount succeeds, then a data CRC
  // error shows up mid-stream (sdspi_host: data CRC failed) and aborts
  // the read. Back to 100kHz, the speed this wiring actually holds up at.
  host.max_freq_khz = SD_MAX_FREQ_KHZ;
  // IMPORTANT: do NOT set SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC. It was tried
  // earlier to work around what looked like a bad CID/CSD checksum, but it
  // was masking a real failure: with CRC ignored, every data read/write
  // (including sector 0 / the MBR) silently returned ESP_OK full of garbage
  // instead of a CRC error. With CRC enabled, the card reads/writes
  // correctly and mounts fine — the data was never actually corrupted.

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = host.slot;
  // More headroom before giving up while polling MISO for "card ready"
  // (default is 40ms) — every command currently hits this timeout at least
  // once before the card responds.
  slot_config.wait_for_miso = SD_WAIT_FOR_MISO_MS;

  // The CSD (capacity) is read correctly now that data CRC is enforced — the
  // earlier "512KB" CSD was an artifact of SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC
  // (see sd-card-issues.md), so the public mount helper is enough.
  sdmmc_card_t* card = NULL;
  ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "esp_vfs_fat_sdspi_mount failed: %s", esp_err_to_name(ret));
    return ret;
  }

  s_card = card;
  ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
  sdmmc_card_print_info(stdout, card);
  return ESP_OK;
}

bool sd_card_is_mounted(void) {
  return s_card != NULL;
}

esp_err_t sd_card_init(spi_host_device_t* out_host) {
  if (out_host == NULL) {
    ESP_LOGE(TAG, "sd_card_init: invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = init_bus();
  if (ret != ESP_OK) {
    return ret;
  }
  *out_host = SPI2_HOST;
  return sd_card_try_mount();
}

const char* sd_card_get_mount_point(void) {
  return MOUNT_POINT;
}

static bool has_mp3_extension(const char* name) {
  size_t len = strlen(name);
  if (len < 4) {
    return false;
  }
  const char* ext = name + len - 4;
  return ext[0] == '.' && tolower((unsigned char)ext[1]) == 'm' &&
         tolower((unsigned char)ext[2]) == 'p' && tolower((unsigned char)ext[3]) == '3';
}

esp_err_t sd_card_scan_tracks(SemaphoreHandle_t spi_mutex) {
  if (spi_mutex == NULL) {
    ESP_LOGE(TAG, "sd_card_scan_tracks: invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(spi_mutex, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to take spi_mutex for track scan");
    return ESP_FAIL;
  }

  DIR* dir = opendir(MOUNT_POINT);
  if (dir == NULL) {
    xSemaphoreGive(spi_mutex);
    ESP_LOGE(TAG, "Failed to open %s", MOUNT_POINT);
    return ESP_FAIL;
  }

  int count = 0;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    // Skip hidden files too: macOS leaves "._name.mp3" AppleDouble stubs next
    // to the real tracks on cards written from a Mac — they match ".mp3" but
    // aren't playable audio.
    if (entry->d_type == DT_DIR || entry->d_name[0] == '.' || !has_mp3_extension(entry->d_name)) {
      continue;
    }
    if (count >= SD_MAX_TRACKS) {
      ESP_LOGW(TAG, "Track list full (%d), remaining files ignored", SD_MAX_TRACKS);
      break;
    }
    if (strlen(entry->d_name) >= SD_MAX_NAME) {
      ESP_LOGW(TAG, "File name too long, skipped: %s", entry->d_name);
      continue;
    }
    strcpy(s_tracks[count], entry->d_name);
    ESP_LOGI(TAG, "Track %d: %s", count, s_tracks[count]);
    count++;
  }
  closedir(dir);
  xSemaphoreGive(spi_mutex);

  s_track_count = count;
  player_state_set_track_count(count);
  ESP_LOGI(TAG, "Found %d .mp3 track(s)", count);
  if (count == 0) {
    ESP_LOGW(TAG, "No .mp3 files found in %s", MOUNT_POINT);
  }
  return ESP_OK;
}

int sd_card_get_track_count(void) {
  return s_track_count;
}

const char* sd_card_get_track_name(int index) {
  if (index < 0 || index >= s_track_count) {
    return NULL;
  }
  return s_tracks[index];
}

esp_err_t sd_card_get_track_path(int index, char* buf, size_t buf_size) {
  if (buf == NULL || index < 0 || index >= s_track_count) {
    ESP_LOGW(TAG, "sd_card_get_track_path: invalid index %d (count %d)", index, s_track_count);
    return ESP_ERR_INVALID_ARG;
  }

  int written = snprintf(buf, buf_size, "%s/%s", MOUNT_POINT, s_tracks[index]);
  if (written < 0 || (size_t)written >= buf_size) {
    ESP_LOGE(TAG, "Path buffer too small for track %d", index);
    return ESP_ERR_INVALID_SIZE;
  }
  return ESP_OK;
}

// -----------------------------------------------------------------
// Hot-plug monitor
// -----------------------------------------------------------------
static void unmount_card(void) {
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
  s_card = NULL;
  s_published = false;
  s_track_count = 0;
  player_state_set_track_count(0);
  player_state_set_sd_present(false);
}

// One pass: mount (if needed) + scan + publish until the card is "published",
// then probe that it still answers. The card may already be mounted by
// sd_card_init() on the first pass.
static void monitor_step(SemaphoreHandle_t spi_mutex) {
  if (!s_published) {
    if (sd_card_try_mount() != ESP_OK) {
      return;
    }
    if (sd_card_scan_tracks(spi_mutex) != ESP_OK) {
      ESP_LOGW(TAG, "Track scan failed, unmounting");
      unmount_card();
      return;
    }
    s_published = true;
    player_state_set_sd_present(true);
    return;
  }

  if (xSemaphoreTake(spi_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  esp_err_t ret = sdmmc_get_status(s_card);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "SD card stopped responding (%s), unmounting", esp_err_to_name(ret));
    unmount_card();
  }
  xSemaphoreGive(spi_mutex);
}

static void monitor_task(void* arg) {
  SemaphoreHandle_t spi_mutex = (SemaphoreHandle_t)arg;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    monitor_step(spi_mutex);
  }
}

esp_err_t sd_card_monitor_init(SemaphoreHandle_t spi_mutex) {
  if (spi_mutex == NULL) {
    ESP_LOGE(TAG, "sd_card_monitor_init: invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  // First pass synchronously so the track list is ready before audio_task
  monitor_step(spi_mutex);

  BaseType_t created = xTaskCreate(monitor_task, "sd_monitor", MONITOR_TASK_STACK_SIZE,
                                   spi_mutex, MONITOR_TASK_PRIORITY, NULL);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create sd_monitor task");
    return ESP_FAIL;
  }
  return ESP_OK;
}
