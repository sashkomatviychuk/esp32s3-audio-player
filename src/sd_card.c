#include "sd_card.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "player_state.h"
#include "sdmmc_cmd.h"

static const char* TAG = "sd_card";

// SD card / SPI2 bus (VS1053 has its own SPI3 bus) — matches hardware.md
#define PIN_NUM_MISO 11
#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK 12
#define PIN_NUM_CS 14
#define MOUNT_POINT "/sdcard"

// -----------------------------------------------------------------
// Module-private state (NOT extern, accessible only within this file)
// -----------------------------------------------------------------
static char s_tracks[SD_MAX_TRACKS][SD_MAX_NAME];
static int s_track_count = 0;

esp_err_t sd_card_init(spi_host_device_t* out_host) {
  if (out_host == NULL) {
    ESP_LOGE(TAG, "sd_card_init: invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

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

  // SPI2 bus for the SD card only. VS1053 initializes its own SPI3 bus in
  // vs1053_init().
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

  // The CSD (capacity) is read correctly now that data CRC is enforced — the
  // earlier "512KB" CSD was an artifact of SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC
  // (see sd-card-issues.md), so the public mount helper is enough.
  sdmmc_card_t* card = NULL;
  ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
  sdmmc_card_print_info(stdout, card);

  *out_host = (spi_host_device_t)host.slot;
  return ESP_OK;
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
