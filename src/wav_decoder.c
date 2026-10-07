#include "wav_decoder.h"

#include <errno.h>
#include <stdlib.h>

#include "esp_log.h"
#include "wav_info.h"

static const char* TAG = "wav_decoder";

// One SD read is at most this many bytes: 512 stereo frames or 1024 mono frames (which expand to
// 1024 stereo frames, within AUDIO_DECODER_MAX_FRAMES). Large reads keep the per-read overhead
// (mutex, FATFS) small.
#define WAV_READ_BYTES 2048
#define BYTES_PER_SAMPLE 2  // wav_get_info() only accepts 16-bit PCM

typedef struct {
  FILE* file;
  SemaphoreHandle_t spi_mutex;
  uint32_t bytes_left;  // audio bytes not yet read from the "data" chunk
  uint16_t channels;
} wav_ctx_t;

static void fill_track_info(const wav_info_t* wav, audio_track_info_t* info) {
  info->sample_rate = wav->sample_rate;
  info->channels = wav->channels;
  info->duration_sec = wav->duration_sec;
}

static esp_err_t wav_read(audio_decoder_t* dec, int16_t* out, size_t max_frames, size_t* frames) {
  wav_ctx_t* ctx = dec->ctx;
  *frames = 0;
  if (max_frames < AUDIO_DECODER_MAX_FRAMES) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (ctx->bytes_left == 0) {
    return ESP_OK;  // the end of the "data" chunk
  }

  size_t want = WAV_READ_BYTES;
  if (want > ctx->bytes_left) {
    want = ctx->bytes_left;
  }

  size_t got = 0;
  if (xSemaphoreTake(ctx->spi_mutex, portMAX_DELAY) == pdTRUE) {
    got = fread(out, 1, want, ctx->file);
    xSemaphoreGive(ctx->spi_mutex);
  }
  if (got == 0) {
    // fread() returns 0 both at a real EOF (a file shorter than its header claims) and on a
    // read error (e.g. an SD CRC failure) — ferror() tells them apart.
    if (ferror(ctx->file)) {
      dec->read_errno = errno;
      return ESP_FAIL;
    }
    ctx->bytes_left = 0;
    return ESP_OK;
  }
  ctx->bytes_left -= (uint32_t)got;

  size_t frame_count = got / ((size_t)ctx->channels * BYTES_PER_SAMPLE);  // partial frame dropped
  if (ctx->channels == 1) {
    audio_decoder_mono_to_stereo(out, frame_count);
  }
  *frames = frame_count;
  return ESP_OK;
}

static void wav_close(audio_decoder_t* dec) {
  wav_ctx_t* ctx = dec->ctx;
  if (ctx != NULL) {
    fclose(ctx->file);
    free(ctx);
  }
  dec->ctx = NULL;
}

esp_err_t wav_decoder_probe(FILE* f, size_t file_size, audio_track_info_t* info) {
  wav_info_t wav;
  esp_err_t err = wav_get_info(f, file_size, &wav);
  if (err == ESP_OK) {
    fill_track_info(&wav, info);
  }
  return err;
}

esp_err_t wav_decoder_open(FILE* f, size_t file_size, SemaphoreHandle_t spi_mutex,
                           audio_decoder_t* dec, audio_track_info_t* info) {
  wav_info_t wav;
  esp_err_t err = wav_get_info(f, file_size, &wav);  // also seeks to the first audio byte
  if (err != ESP_OK) {
    return err;
  }

  wav_ctx_t* ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    ESP_LOGE(TAG, "Out of memory");
    return ESP_ERR_NO_MEM;
  }
  ctx->file = f;
  ctx->spi_mutex = spi_mutex;
  ctx->bytes_left = wav.data_size;
  ctx->channels = wav.channels;

  dec->ctx = ctx;
  dec->read = wav_read;
  dec->close = wav_close;
  dec->sample_rate = wav.sample_rate;
  fill_track_info(&wav, info);
  return ESP_OK;
}
