#include "wav_info.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "wav_info";

#define RIFF_HEADER_SIZE 12  // "RIFF" + size + "WAVE"
#define CHUNK_HEADER_SIZE 8  // 4-byte id + 32-bit little-endian size
#define FMT_MIN_SIZE 16      // PCMWAVEFORMAT
#define FMT_EXT_MIN_SIZE 40  // WAVEFORMATEXTENSIBLE
#define FMT_EXT_SUBFORMAT_OFFSET 24
#define FORMAT_TAG_PCM 1
#define FORMAT_TAG_EXTENSIBLE 0xFFFE
#define SUPPORTED_BITS 16
#define MAX_CHANNELS 2
#define MIN_SAMPLE_RATE 8000
#define MAX_SAMPLE_RATE 48000
#define MAX_CHUNKS 32  // guards against a corrupt chunk list looping over the whole file

static uint16_t read_u16le(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Parses the body of a "fmt " chunk into @p out. Returns ESP_OK or why the
// format cannot be played.
static esp_err_t parse_fmt(const uint8_t* fmt, uint32_t size, wav_info_t* out) {
  if (size < FMT_MIN_SIZE) {
    ESP_LOGW(TAG, "fmt chunk too small (%u bytes)", (unsigned)size);
    return ESP_ERR_INVALID_RESPONSE;
  }

  uint16_t format_tag = read_u16le(fmt);
  if (format_tag == FORMAT_TAG_EXTENSIBLE && size >= FMT_EXT_MIN_SIZE) {
    // The real format tag is the first two bytes of the sub-format GUID.
    format_tag = read_u16le(fmt + FMT_EXT_SUBFORMAT_OFFSET);
  }
  if (format_tag != FORMAT_TAG_PCM) {
    ESP_LOGW(TAG, "Unsupported WAV format tag 0x%04X (only PCM)", format_tag);
    return ESP_ERR_NOT_SUPPORTED;
  }

  out->channels = read_u16le(fmt + 2);
  out->sample_rate = read_u32le(fmt + 4);
  out->byte_rate = read_u32le(fmt + 8);
  out->bits = read_u16le(fmt + 14);

  if (out->bits != SUPPORTED_BITS) {
    ESP_LOGW(TAG, "Unsupported sample size: %u bit (only %d)", out->bits, SUPPORTED_BITS);
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (out->channels < 1 || out->channels > MAX_CHANNELS) {
    ESP_LOGW(TAG, "Unsupported channel count: %u (only mono/stereo)", out->channels);
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (out->sample_rate < MIN_SAMPLE_RATE || out->sample_rate > MAX_SAMPLE_RATE) {
    ESP_LOGW(TAG, "Unsupported sample rate: %u Hz", (unsigned)out->sample_rate);
    return ESP_ERR_NOT_SUPPORTED;
  }

  // Do not trust the header's byte rate field — derive it from the format.
  out->byte_rate = out->sample_rate * out->channels * (out->bits / 8);
  return ESP_OK;
}

esp_err_t wav_get_info(FILE* f, size_t file_size, wav_info_t* out) {
  if (f == NULL || out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));

  uint8_t header[RIFF_HEADER_SIZE];
  if (fseek(f, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), f) != sizeof(header) ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    ESP_LOGW(TAG, "Not a RIFF/WAVE file");
    return ESP_ERR_INVALID_RESPONSE;
  }

  bool have_fmt = false;
  size_t pos = RIFF_HEADER_SIZE;
  for (int chunk = 0; chunk < MAX_CHUNKS && (pos + CHUNK_HEADER_SIZE) <= file_size; chunk++) {
    uint8_t chunk_header[CHUNK_HEADER_SIZE];
    if (fseek(f, (long)pos, SEEK_SET) != 0 ||
        fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) {
      break;
    }
    uint32_t chunk_size = read_u32le(chunk_header + 4);
    size_t body = pos + CHUNK_HEADER_SIZE;

    if (memcmp(chunk_header, "fmt ", 4) == 0) {
      uint8_t fmt[FMT_EXT_MIN_SIZE] = {0};
      size_t to_read = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
      if (fread(fmt, 1, to_read, f) != to_read) {
        break;
      }
      esp_err_t err = parse_fmt(fmt, chunk_size, out);
      if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
      }
      have_fmt = true;
    } else if (memcmp(chunk_header, "data", 4) == 0) {
      if (!have_fmt) {
        ESP_LOGW(TAG, "data chunk before fmt chunk");
        break;
      }
      size_t available = file_size - body;
      // 0 / 0xFFFFFFFF sizes come from encoders that stream without seeking back.
      if (chunk_size == 0 || chunk_size > available) {
        chunk_size = (uint32_t)available;
      }
      out->data_start = (uint32_t)body;
      out->data_size = chunk_size - (chunk_size % (out->channels * (out->bits / 8)));
      out->duration_sec = out->data_size / out->byte_rate;
      if (fseek(f, (long)body, SEEK_SET) != 0) {
        break;
      }
      ESP_LOGI(TAG, "WAV: %u Hz, %u ch, %u bit, %u s", (unsigned)out->sample_rate, out->channels,
               out->bits, (unsigned)out->duration_sec);
      return ESP_OK;
    }

    // Chunks are word-aligned: an odd size is followed by one padding byte.
    pos = body + chunk_size + (chunk_size & 1U);
  }

  ESP_LOGW(TAG, "No usable fmt/data chunks found");
  memset(out, 0, sizeof(*out));
  return ESP_ERR_INVALID_RESPONSE;
}
