#include "mp3_info.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "mp3_info";

#define ID3_HEADER_SIZE 10
#define SYNC_SEARCH_LIMIT 1024  // bytes scanned after the ID3 tag for a frame sync

// Bitrates in kbps, indexed by the 4-bit bitrate field. 0 = free, -1 = invalid.
static const int16_t BITRATE_MPEG1_L3[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                             112, 128, 160, 192, 224, 256, 320, -1};
static const int16_t BITRATE_MPEG2_L3[16] = {0,  8,  16, 24,  32,  40,  48,  56,
                                             64, 80, 96, 112, 128, 144, 160, -1};

// Sample rates in Hz for MPEG1; MPEG2 is half of it, MPEG2.5 a quarter.
static const uint32_t SAMPLE_RATE_MPEG1[3] = {44100, 48000, 32000};

void mp3_get_info(FILE* f, size_t fileSize, mp3_info_t* out) {
  out->duration_sec = 0;
  out->bitrate_bps = 0;
  out->audio_start = 0;
  if (f == NULL || fileSize == 0) {
    return;
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
  uint32_t xingDurationSec = 0;  // exact length from a Xing/Info header, 0 if absent
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

      // The first frame of a VBR/LAME file is often a Xing/Info frame holding
      // the total frame count — the exact length, unlike a bitrate-based
      // estimate. Its offset depends on version and channel mode (side info size).
      int srIdx = (buf[i + 2] >> 2) & 0x03;
      bool mono = ((buf[i + 3] >> 6) & 0x03) == 3;
      if (srIdx < 3) {
        int shift = version == 3 ? 0 : (version == 2 ? 1 : 2);
        uint32_t sampleRate = SAMPLE_RATE_MPEG1[srIdx] >> shift;
        uint32_t samplesPerFrame = version == 3 ? 1152 : 576;
        size_t tag = i + 4 + (version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        bool isXing = tag + 12 <= n && (memcmp(buf + tag, "Xing", 4) == 0 ||
                                        memcmp(buf + tag, "Info", 4) == 0);
        if (isXing && (buf[tag + 7] & 0x01)) {  // flags: frame count present
          uint32_t frames = ((uint32_t)buf[tag + 8] << 24) | ((uint32_t)buf[tag + 9] << 16) |
                            ((uint32_t)buf[tag + 10] << 8) | buf[tag + 11];
          xingDurationSec = (uint32_t)(((uint64_t)frames * samplesPerFrame) / sampleRate);
        }
      }
      break;
    }
  }
  rewind(f);

  if (bitrate == 0 || fileSize <= audioStart) {
    ESP_LOGW(TAG, "Could not determine MP3 bitrate, duration unknown");
    return;
  }

  out->audio_start = audioStart;
  if (xingDurationSec > 0) {
    // Average bitrate, so elapsed (bytes -> seconds) also reaches the exact
    // length at the end of the file even for VBR.
    out->duration_sec = xingDurationSec;
    out->bitrate_bps = (uint32_t)(((uint64_t)(fileSize - audioStart) * 8) / xingDurationSec);
    ESP_LOGI(TAG, "Xing/Info header: first-frame bitrate %u kbps, average %u kbps",
             (unsigned)(bitrate / 1000), (unsigned)(out->bitrate_bps / 1000));
  } else {
    out->bitrate_bps = bitrate;
    out->duration_sec = (uint32_t)(((uint64_t)(fileSize - audioStart) * 8) / bitrate);
  }
  ESP_LOGI(TAG, "Bitrate %u kbps, duration %us%s", (unsigned)(out->bitrate_bps / 1000),
           (unsigned)out->duration_sec, xingDurationSec > 0 ? " (Xing)" : " (estimate)");
}
