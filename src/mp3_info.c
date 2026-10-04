#include "mp3_info.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "mp3_info";

// ---- ID3v2 ----
#define ID3_HEADER_SIZE 10
#define ID3_SIZE_OFFSET 6  // 4 syncsafe size bytes follow "ID3", version and flags
#define ID3_SIZE_BYTES 4
#define ID3_SYNCSAFE_BITS 7  // the top bit of every size byte is always 0
#define ID3_SYNCSAFE_MASK 0x7F

// ---- MPEG audio frame header ----
#define SYNC_SEARCH_LIMIT 1024  // bytes scanned after the ID3 tag for a frame sync
#define FRAME_HEADER_SIZE 4
#define FRAME_SYNC_BYTE 0xFF
#define FRAME_SYNC_MASK 0xE0  // top 3 bits of the 2nd byte complete the 11-bit sync word
#define MPEG_VERSION_2_5 0
#define MPEG_VERSION_RESERVED 1
#define MPEG_VERSION_2 2
#define MPEG_VERSION_1 3
#define LAYER_III 1
#define BITRATE_IDX_FREE 0
#define BITRATE_IDX_INVALID 15
#define SAMPLE_RATE_IDX_RESERVED 3
#define CHANNEL_MODE_MONO 3
#define SAMPLES_PER_FRAME_MPEG1 1152
#define SAMPLES_PER_FRAME_MPEG2 576

// ---- Xing/Info tag (first frame of VBR/LAME files) ----
// Offset after the frame header = Layer III side info size.
#define SIDE_INFO_MPEG1_STEREO 32
#define SIDE_INFO_MPEG1_MONO 17
#define SIDE_INFO_MPEG2_STEREO 17
#define SIDE_INFO_MPEG2_MONO 9
#define XING_TAG_LEN 4
#define XING_MIN_SIZE 12  // tag + flags + frame count
#define XING_FLAGS_OFFSET 7
#define XING_FLAG_FRAMES 0x01  // frame count field present
#define XING_FRAMES_OFFSET 8

#define BITS_PER_BYTE 8
#define BITS_PER_KBIT 1000

// Bitrates in kbps, indexed by the 4-bit bitrate field. 0 = free, -1 = invalid.
static const int16_t BITRATE_MPEG1_L3[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                             112, 128, 160, 192, 224, 256, 320, -1};
static const int16_t BITRATE_MPEG2_L3[16] = {0,  8,  16, 24,  32,  40,  48,  56,
                                             64, 80, 96, 112, 128, 144, 160, -1};

// Sample rates in Hz for MPEG1; MPEG2 is half of it, MPEG2.5 a quarter.
static const uint32_t SAMPLE_RATE_MPEG1[3] = {44100, 48000, 32000};

typedef struct {
  int version;     // MPEG_VERSION_*
  int layer;       // LAYER_III
  int bitrateIdx;  // index into the bitrate tables
  int sampleRateIdx;
  int channelMode;  // CHANNEL_MODE_MONO = mono
} frame_header_t;

// Splits the 4 header bytes into fields (bit positions per the MPEG audio frame spec).
static frame_header_t parse_frame_header(const uint8_t* b) {
  frame_header_t h = {
      .version = (b[1] >> 3) & 0x03,
      .layer = (b[1] >> 1) & 0x03,
      .bitrateIdx = (b[2] >> 4) & 0x0F,
      .sampleRateIdx = (b[2] >> 2) & 0x03,
      .channelMode = (b[3] >> 6) & 0x03,
  };
  return h;
}

// Reserved version, not Layer III, free/invalid bitrate — not usable for an estimate.
static bool is_usable_frame(const frame_header_t* h) {
  return h->version != MPEG_VERSION_RESERVED && h->layer == LAYER_III &&
         h->bitrateIdx != BITRATE_IDX_FREE && h->bitrateIdx != BITRATE_IDX_INVALID;
}

// MPEG2 rates are half of MPEG1, MPEG2.5 a quarter.
static int sample_rate_shift(int version) {
  if (version == MPEG_VERSION_1) {
    return 0;
  }
  return version == MPEG_VERSION_2 ? 1 : 2;
}

static size_t side_info_size(const frame_header_t* h) {
  bool mono = h->channelMode == CHANNEL_MODE_MONO;
  if (h->version == MPEG_VERSION_1) {
    return mono ? SIDE_INFO_MPEG1_MONO : SIDE_INFO_MPEG1_STEREO;
  }
  return mono ? SIDE_INFO_MPEG2_MONO : SIDE_INFO_MPEG2_STEREO;
}

// Seeks to the start and returns the offset of the first audio byte (after an
// ID3v2 tag, 0 if there is none). Returns false only if the seek failed.
static bool read_audio_start(FILE* f, uint32_t* audio_start) {
  *audio_start = 0;
  if (fseek(f, 0, SEEK_SET) != 0) {
    return false;
  }
  uint8_t header[ID3_HEADER_SIZE];
  if (fread(header, 1, ID3_HEADER_SIZE, f) == ID3_HEADER_SIZE && memcmp(header, "ID3", 3) == 0) {
    uint32_t tag_size = 0;
    for (int i = 0; i < ID3_SIZE_BYTES; i++) {
      tag_size = (tag_size << ID3_SYNCSAFE_BITS) | (header[ID3_SIZE_OFFSET + i] & ID3_SYNCSAFE_MASK);
    }
    *audio_start = ID3_HEADER_SIZE + tag_size;
  }
  return true;
}

// Finds the first usable Layer III frame header in buf[0..n).
static bool find_first_frame(const uint8_t* buf, size_t n, size_t* pos, frame_header_t* header) {
  for (size_t i = 0; i + FRAME_HEADER_SIZE <= n; i++) {
    if (buf[i] != FRAME_SYNC_BYTE || (buf[i + 1] & FRAME_SYNC_MASK) != FRAME_SYNC_MASK) {
      continue;
    }
    frame_header_t h = parse_frame_header(buf + i);
    if (is_usable_frame(&h)) {
      *pos = i;
      *header = h;
      return true;
    }
  }
  return false;
}

// The first frame of a VBR/LAME file is often a Xing/Info frame holding the
// total frame count — the exact length, unlike a bitrate-based estimate. Its
// offset depends on version and channel mode (side info size).
// Returns the length in seconds, 0 if there is no such tag.
static uint32_t read_xing_duration(const uint8_t* buf, size_t n, size_t frame_pos,
                                   const frame_header_t* h) {
  if (h->sampleRateIdx == SAMPLE_RATE_IDX_RESERVED) {
    return 0;
  }
  size_t tag = frame_pos + FRAME_HEADER_SIZE + side_info_size(h);
  if (tag + XING_MIN_SIZE > n ||
      (memcmp(buf + tag, "Xing", XING_TAG_LEN) != 0 &&
       memcmp(buf + tag, "Info", XING_TAG_LEN) != 0) ||
      (buf[tag + XING_FLAGS_OFFSET] & XING_FLAG_FRAMES) == 0) {
    return 0;
  }

  const uint8_t* f = buf + tag + XING_FRAMES_OFFSET;
  uint32_t frames = ((uint32_t)f[0] << 24) | ((uint32_t)f[1] << 16) | ((uint32_t)f[2] << 8) | f[3];
  uint32_t sample_rate = SAMPLE_RATE_MPEG1[h->sampleRateIdx] >> sample_rate_shift(h->version);
  uint32_t samples_per_frame =
      h->version == MPEG_VERSION_1 ? SAMPLES_PER_FRAME_MPEG1 : SAMPLES_PER_FRAME_MPEG2;
  return (uint32_t)(((uint64_t)frames * samples_per_frame) / sample_rate);
}

void mp3_get_info(FILE* f, size_t file_size, mp3_info_t* out) {
  out->duration_sec = 0;
  out->bitrate_bps = 0;
  out->audio_start = 0;
  if (f == NULL || file_size == 0) {
    return;
  }

  uint32_t audio_start = 0;
  if (!read_audio_start(f, &audio_start)) {
    ESP_LOGW(TAG, "fseek to start failed, duration unknown");
    return;
  }

  uint32_t bitrate = 0;
  uint32_t xing_duration_sec = 0;           // exact length from a Xing/Info header, 0 if absent
  static uint8_t buf[SYNC_SEARCH_LIMIT];  // static: keeps the caller's task stack small
  if (fseek(f, (long)audio_start, SEEK_SET) == 0) {
    size_t n = fread(buf, 1, sizeof(buf), f);
    size_t frame_pos = 0;
    frame_header_t header;
    if (find_first_frame(buf, n, &frame_pos, &header)) {
      const int16_t* table = header.version == MPEG_VERSION_1 ? BITRATE_MPEG1_L3 : BITRATE_MPEG2_L3;
      bitrate = (uint32_t)table[header.bitrateIdx] * BITS_PER_KBIT;
      xing_duration_sec = read_xing_duration(buf, n, frame_pos, &header);
#if CONFIG_AUDIO_DEBUG_MP3_INFO_LOG
      ESP_LOGI(TAG,
               "First frame: file %u B, audio_start %u, frame_pos %u, MPEG ver %d, bitrate idx %d "
               "(%d kbps), sample rate idx %d, channel mode %d, Xing %us",
               (unsigned)file_size, (unsigned)audio_start, (unsigned)frame_pos, header.version,
               header.bitrateIdx, (int)table[header.bitrateIdx], header.sampleRateIdx,
               header.channelMode, (unsigned)xing_duration_sec);
#endif
    } else {
      ESP_LOGW(TAG, "No usable MPEG frame in %u bytes after audio_start %u", (unsigned)n,
               (unsigned)audio_start);
    }
  }
  // The caller streams the file from the start next, so a failed seek is fatal for it too
  if (fseek(f, 0, SEEK_SET) != 0) {
    ESP_LOGW(TAG, "fseek back to start failed, duration unknown");
    return;
  }

  if (bitrate == 0 || file_size <= audio_start) {
    ESP_LOGW(TAG, "Could not determine MP3 bitrate, duration unknown");
    return;
  }

  uint64_t audio_bits = (uint64_t)(file_size - audio_start) * BITS_PER_BYTE;
  out->audio_start = audio_start;
  if (xing_duration_sec > 0) {
    // Average bitrate, so elapsed (bytes -> seconds) also reaches the exact
    // length at the end of the file even for VBR.
    out->duration_sec = xing_duration_sec;
    out->bitrate_bps = (uint32_t)(audio_bits / xing_duration_sec);
    ESP_LOGI(TAG, "Xing/Info header: first-frame bitrate %u kbps, average %u kbps",
             (unsigned)(bitrate / BITS_PER_KBIT), (unsigned)(out->bitrate_bps / BITS_PER_KBIT));
  } else {
    out->bitrate_bps = bitrate;
    out->duration_sec = (uint32_t)(audio_bits / bitrate);
  }
  ESP_LOGI(TAG, "Bitrate %u kbps, duration %us%s", (unsigned)(out->bitrate_bps / BITS_PER_KBIT),
           (unsigned)out->duration_sec, xing_duration_sec > 0 ? " (Xing)" : " (estimate)");
}
