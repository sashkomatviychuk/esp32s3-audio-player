#include "mp3_decoder.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mp3_info.h"
#include "mp3dec.h"  // Helix (chmorgan/esp-libhelix-mp3)

static const char* TAG = "mp3_decoder";

// Holds the unread tail (up to MAINBUF_SIZE, the largest amount one frame can need) plus a
// fresh read of 2 KB or more, so a refill is one decent-sized SD read, not one per frame.
#define INPUT_BUF_SIZE 4096

// Consecutive undecodable frames before the file is given up as corrupt. Each failure skips at
// least one byte, so this is a few hundred bytes of garbage at most.
#define MAX_CONSECUTIVE_ERRORS 100

#define MAX_NCHAN_STEREO 2
#define SYNC_TAIL_BYTES 1  // a sync word is 2 bytes: keep the last byte when no sync was found

typedef struct {
  FILE* file;
  SemaphoreHandle_t spi_mutex;
  HMP3Decoder helix;
  uint8_t* buf;    // INPUT_BUF_SIZE bytes of raw file data
  size_t buf_len;  // valid bytes in buf
  size_t buf_pos;  // next byte to decode
  bool eof;        // the file has been read to its end
  int errors;      // consecutive failed decodes
  // Decode statistics, logged when the track is closed.
  uint32_t frames_decoded;
  uint32_t frames_skipped;  // MAINDATA_UNDERFLOW: silent frames at the start (bit reservoir)
  uint64_t decode_us_total;
  uint32_t decode_us_max;
  uint32_t frame_period_us;  // duration of the last decoded frame, for the log
} mp3_ctx_t;

static void fill_track_info(const mp3_info_t* mp3, audio_track_info_t* info) {
  info->sample_rate = mp3->sample_rate;
  info->channels = mp3->channels;
  info->duration_sec = mp3->duration_sec;
}

// Reads the header via mp3_get_info(); Layer I/II and non-MP3 data end up as "no frame found".
static esp_err_t read_header(FILE* f, size_t file_size, mp3_info_t* mp3) {
  mp3_get_info(f, file_size, mp3);
  if (mp3->sample_rate == 0 || mp3->channels == 0) {
    ESP_LOGW(TAG, "No MPEG Layer III frame found (only MP3 Layer III is supported)");
    return ESP_ERR_NOT_SUPPORTED;
  }
  return ESP_OK;
}

// Moves the unread bytes to the start of the buffer and tops it up from the file.
static esp_err_t fill_input(audio_decoder_t* dec, mp3_ctx_t* ctx) {
  if (ctx->buf_pos > 0) {
    size_t keep = ctx->buf_len - ctx->buf_pos;
    memmove(ctx->buf, ctx->buf + ctx->buf_pos, keep);
    ctx->buf_len = keep;
    ctx->buf_pos = 0;
  }

  size_t got = 0;
  if (xSemaphoreTake(ctx->spi_mutex, portMAX_DELAY) == pdTRUE) {
    got = fread(ctx->buf + ctx->buf_len, 1, INPUT_BUF_SIZE - ctx->buf_len, ctx->file);
    xSemaphoreGive(ctx->spi_mutex);
  }
  ctx->buf_len += got;

  if (got == 0) {
    // fread() returns 0 both at a real EOF and on a read error (e.g. an SD CRC failure).
    if (ferror(ctx->file)) {
      dec->read_errno = errno;
      return ESP_FAIL;
    }
    ctx->eof = true;
  }
  return ESP_OK;
}

// Handles a frame Helix decoded successfully: converts it to stereo and tracks the statistics.
// @p out holds fi->outputSamps samples as written by MP3Decode().
static size_t finish_frame(audio_decoder_t* dec, mp3_ctx_t* ctx, const MP3FrameInfo* fi,
                           int64_t decode_us, int16_t* out) {
  size_t frames = (size_t)fi->outputSamps / (size_t)fi->nChans;
  if (fi->nChans == 1) {
    audio_decoder_mono_to_stereo(out, frames);  // Helix leaves mono as one sample per frame
  }

  if ((uint32_t)fi->samprate != dec->sample_rate) {
    ESP_LOGW(TAG, "Sample rate changed mid-stream: %u -> %d Hz", (unsigned)dec->sample_rate,
             fi->samprate);
    dec->sample_rate = (uint32_t)fi->samprate;
  }

  ctx->frames_decoded++;
  ctx->decode_us_total += (uint64_t)decode_us;
  if ((uint32_t)decode_us > ctx->decode_us_max) {
    ctx->decode_us_max = (uint32_t)decode_us;
  }
  ctx->frame_period_us = (uint32_t)(((uint64_t)frames * 1000000U) / (uint32_t)fi->samprate);
  return frames;
}

static esp_err_t mp3_read(audio_decoder_t* dec, int16_t* out, size_t max_frames, size_t* frames) {
  mp3_ctx_t* ctx = dec->ctx;
  *frames = 0;
  if (max_frames < AUDIO_DECODER_MAX_FRAMES) {
    return ESP_ERR_INVALID_SIZE;
  }

  while (ctx->errors < MAX_CONSECUTIVE_ERRORS) {
    // Helix needs a whole frame plus the bit reservoir in front of it: keep MAINBUF_SIZE bytes.
    if (!ctx->eof && (ctx->buf_len - ctx->buf_pos) < MAINBUF_SIZE) {
      esp_err_t err = fill_input(dec, ctx);
      if (err != ESP_OK) {
        return err;
      }
    }

    size_t avail = ctx->buf_len - ctx->buf_pos;
    int offset = avail >= 2 ? MP3FindSyncWord(ctx->buf + ctx->buf_pos, (int)avail) : -1;
    if (offset < 0) {
      if (ctx->eof) {
        return ESP_OK;  // trailing data without a frame (ID3v1 tag, padding): the end
      }
      // Keep the last byte, it may be the first half of a sync word; fill_input() adds more.
      ctx->buf_pos = ctx->buf_len > 0 ? ctx->buf_len - SYNC_TAIL_BYTES : 0;
      continue;
    }
    ctx->buf_pos += (size_t)offset;

    unsigned char* frame_start = ctx->buf + ctx->buf_pos;
    unsigned char* in = frame_start;
    int bytes_left = (int)(ctx->buf_len - ctx->buf_pos);
    int64_t start_us = esp_timer_get_time();
    int result = MP3Decode(ctx->helix, &in, &bytes_left, out, 0);
    int64_t decode_us = esp_timer_get_time() - start_us;
    size_t consumed = (size_t)(in - frame_start);  // how far Helix moved the input pointer

    switch (result) {
      case ERR_MP3_NONE: {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(ctx->helix, &fi);
        ctx->buf_pos += consumed;
        if (fi.nChans < 1 || fi.nChans > MAX_NCHAN_STEREO || fi.outputSamps <= 0 ||
            fi.samprate <= 0) {
          ctx->errors++;
          continue;
        }
        ctx->errors = 0;
        *frames = finish_frame(dec, ctx, &fi, decode_us, out);
        return ESP_OK;
      }

      case ERR_MP3_MAINDATA_UNDERFLOW:
        // Expected for the first frame(s) of a file: they reference bytes of the bit reservoir
        // that came from earlier frames we never saw. The frame is consumed, its output is silent
        // and skipped; this is not an error.
        ctx->buf_pos += consumed;
        ctx->frames_skipped++;
        continue;

      case ERR_MP3_INDATA_UNDERFLOW:
        // The buffer ends inside the frame; nothing was consumed. Retry after a refill (the loop
        // top does it, as less than MAINBUF_SIZE is left). A truncated last frame is the end.
        if (ctx->eof) {
          return ESP_OK;
        }
        if ((ctx->buf_len - ctx->buf_pos) >= MAINBUF_SIZE) {
          ctx->buf_pos++;  // a full buffer cannot be short of data: a false sync, skip it
          ctx->errors++;
        }
        continue;

      default:
        // Bad header / side info / Huffman data: skip what Helix consumed (at least one byte)
        // and look for the next sync word.
        ESP_LOGD(TAG, "Decode error %d at buffer offset %u", result, (unsigned)ctx->buf_pos);
        ctx->buf_pos += consumed > 0 ? consumed : 1;
        ctx->errors++;
        continue;
    }
  }

  ESP_LOGE(TAG, "Too many undecodable frames in a row, giving up (corrupt file?)");
  return ESP_ERR_INVALID_RESPONSE;
}

static void mp3_close(audio_decoder_t* dec) {
  mp3_ctx_t* ctx = dec->ctx;
  if (ctx == NULL) {
    return;
  }
  if (ctx->frames_decoded > 0) {
    ESP_LOGI(
        TAG,
        "Decoded %u frames (%u silent skipped): avg %u us, max %u us per frame, frame is %u us",
        (unsigned)ctx->frames_decoded, (unsigned)ctx->frames_skipped,
        (unsigned)(ctx->decode_us_total / ctx->frames_decoded), (unsigned)ctx->decode_us_max,
        (unsigned)ctx->frame_period_us);
  }
  MP3FreeDecoder(ctx->helix);
  fclose(ctx->file);
  free(ctx->buf);
  free(ctx);
  dec->ctx = NULL;
}

esp_err_t mp3_decoder_probe(FILE* f, size_t file_size, audio_track_info_t* info) {
  mp3_info_t mp3;
  esp_err_t err = read_header(f, file_size, &mp3);
  if (err == ESP_OK) {
    fill_track_info(&mp3, info);
  }
  return err;
}

esp_err_t mp3_decoder_open(FILE* f, size_t file_size, SemaphoreHandle_t spi_mutex,
                           audio_decoder_t* dec, audio_track_info_t* info) {
  mp3_info_t mp3;
  esp_err_t err = read_header(f, file_size, &mp3);
  if (err != ESP_OK) {
    return err;
  }
  if (fseek(f, (long)mp3.audio_start, SEEK_SET) != 0) {  // skip the ID3v2 tag
    ESP_LOGE(TAG, "fseek to the audio data failed");
    return ESP_FAIL;
  }

  mp3_ctx_t* ctx = calloc(1, sizeof(*ctx));
  if (ctx != NULL) {
    ctx->buf = malloc(INPUT_BUF_SIZE);
    ctx->helix = MP3InitDecoder();
  }
  if (ctx == NULL || ctx->buf == NULL || ctx->helix == NULL) {
    ESP_LOGE(TAG, "Out of memory for the MP3 decoder");
    if (ctx != NULL) {
      if (ctx->helix != NULL) {
        MP3FreeDecoder(ctx->helix);
      }
      free(ctx->buf);
      free(ctx);
    }
    return ESP_ERR_NO_MEM;
  }
  ctx->file = f;
  ctx->spi_mutex = spi_mutex;

  dec->ctx = ctx;
  dec->read = mp3_read;
  dec->close = mp3_close;
  dec->sample_rate = mp3.sample_rate;
  fill_track_info(&mp3, info);
  return ESP_OK;
}
