#include "mp3_info.h"

#include "esp_log.h"

static const char* TAG = "mp3_info";

#define ID3_HEADER_SIZE 10
#define SYNC_SEARCH_LIMIT 1024  // bytes scanned after the ID3 tag for a frame sync

// Bitrates in kbps, indexed by the 4-bit bitrate field. 0 = free, -1 = invalid.
static const int16_t BITRATE_MPEG1_L3[16] = {0,  32,  40,  48,  56,  64,  80,  96,
                                             112, 128, 160, 192, 224, 256, 320, -1};
static const int16_t BITRATE_MPEG2_L3[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                             64, 80, 96, 112, 128, 144, 160, -1};

uint32_t mp3_get_duration_sec(FILE* f, size_t fileSize) {
  if (f == NULL || fileSize == 0) {
    return 0;
  }

  uint32_t audioStart = 0;
  uint8_t header[ID3_HEADER_SIZE];
  rewind(f);
  if (fread(header, 1, ID3_HEADER_SIZE, f) == ID3_HEADER_SIZE && header[0] == 'I' &&
      header[1] == 'D' && header[2] == '3') {
    // Syncsafe size: 4 x 7 bits
    audioStart = ID3_HEADER_SIZE + (((uint32_t)(header[6] & 0x7F) << 21) |
                                    ((uint32_t)(header[7] & 0x7F) << 14) |
                                    ((uint32_t)(header[8] & 0x7F) << 7) | (header[9] & 0x7F));
  }

  uint32_t bitrate = 0;
  static uint8_t buf[SYNC_SEARCH_LIMIT];  // static: keeps the caller's task stack small
  if (fseek(f, (long)audioStart, SEEK_SET) == 0) {
    size_t n = fread(buf, 1, sizeof(buf), f);
    for (size_t i = 0; i + 4 <= n; i++) {
      if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) {
        continue;
      }
      int version = (buf[i + 1] >> 3) & 0x03;  // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
      int layer = (buf[i + 1] >> 1) & 0x03;    // 1 = Layer III
      int idx = (buf[i + 2] >> 4) & 0x0F;
      if (version == 1 || layer != 1 || idx == 0 || idx == 15) {
        continue;  // reserved version, not Layer III, free/invalid bitrate
      }
      int kbps = version == 3 ? BITRATE_MPEG1_L3[idx] : BITRATE_MPEG2_L3[idx];
      bitrate = (uint32_t)kbps * 1000;
      break;
    }
  }
  rewind(f);

  if (bitrate == 0 || fileSize <= audioStart) {
    ESP_LOGW(TAG, "Could not determine MP3 bitrate, duration unknown");
    return 0;
  }

  uint32_t durationSec = (uint32_t)(((uint64_t)(fileSize - audioStart) * 8) / bitrate);
  ESP_LOGI(TAG, "Bitrate %u kbps, duration ~%us", (unsigned)(bitrate / 1000), (unsigned)durationSec);
  return durationSec;
}
