#include "audio_decoder.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "mp3_decoder.h"
#include "wav_decoder.h"

static const char* TAG = "audio_decoder";

#define EXTENSION_LEN 4  // ".wav" / ".mp3"

static bool has_extension(const char* name, size_t len, const char* ext) {
  const char* tail = name + len - EXTENSION_LEN;
  for (size_t i = 0; i < EXTENSION_LEN; i++) {
    if (tolower((unsigned char)tail[i]) != ext[i]) {
      return false;
    }
  }
  return true;
}

audio_format_t audio_format_from_name(const char* name) {
  if (name == NULL) {
    return AUDIO_FORMAT_UNKNOWN;
  }
  size_t len = strlen(name);
  if (len <= EXTENSION_LEN) {  // nothing but the extension is not a track name either
    return AUDIO_FORMAT_UNKNOWN;
  }
  if (has_extension(name, len, ".wav")) {
    return AUDIO_FORMAT_WAV;
  }
  if (has_extension(name, len, ".mp3")) {
    return AUDIO_FORMAT_MP3;
  }
  return AUDIO_FORMAT_UNKNOWN;
}

// Opens the file and runs the format's probe or open. @p dec == NULL means probe only.
// Takes spi_mutex for the whole header work: fopen/stat/fread all touch the SD card.
static esp_err_t open_track(const char* path, SemaphoreHandle_t spi_mutex, audio_decoder_t* dec,
                            audio_track_info_t* info) {
  if (path == NULL || spi_mutex == NULL || info == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(info, 0, sizeof(*info));

  audio_format_t format = audio_format_from_name(path);
  if (format == AUDIO_FORMAT_UNKNOWN) {
    ESP_LOGW(TAG, "Unsupported file type: %s", path);
    return ESP_ERR_NOT_SUPPORTED;
  }
  info->format = format;

  if (xSemaphoreTake(spi_mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_FAIL;
  }

  esp_err_t err = ESP_FAIL;
  struct stat st;
  FILE* f = NULL;
  if (stat(path, &st) != 0 || (f = fopen(path, "rb")) == NULL) {
    ESP_LOGE(TAG, "Failed to open %s", path);
  } else if (format == AUDIO_FORMAT_WAV) {
    err = dec != NULL ? wav_decoder_open(f, (size_t)st.st_size, spi_mutex, dec, info)
                      : wav_decoder_probe(f, (size_t)st.st_size, info);
  } else {
    err = dec != NULL ? mp3_decoder_open(f, (size_t)st.st_size, spi_mutex, dec, info)
                      : mp3_decoder_probe(f, (size_t)st.st_size, info);
  }

  // On success an opened decoder owns the file; in every other case it is closed here.
  if (f != NULL && (err != ESP_OK || dec == NULL)) {
    fclose(f);
  }
  xSemaphoreGive(spi_mutex);
  return err;
}

esp_err_t audio_decoder_open(const char* path, SemaphoreHandle_t spi_mutex, audio_decoder_t* dec,
                             audio_track_info_t* info) {
  if (dec == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(dec, 0, sizeof(*dec));
  esp_err_t err = open_track(path, spi_mutex, dec, info);
  if (err != ESP_OK) {
    memset(dec, 0, sizeof(*dec));
  }
  return err;
}

esp_err_t audio_decoder_probe(const char* path, SemaphoreHandle_t spi_mutex,
                              audio_track_info_t* info) {
  return open_track(path, spi_mutex, NULL, info);
}

void audio_decoder_close(audio_decoder_t* dec) {
  if (dec == NULL) {
    return;
  }
  if (dec->close != NULL) {
    dec->close(dec);
  }
  memset(dec, 0, sizeof(*dec));
}

void audio_decoder_mono_to_stereo(int16_t* buf, size_t frames) {
  // Walk backwards: the stereo frame i lands at 2i and 2i + 1, which never overwrites a mono
  // sample that is still to be read (those sit at indexes below i).
  for (size_t i = frames; i-- > 0;) {
    int16_t sample = buf[i];
    buf[(2 * i) + 1] = sample;
    buf[2 * i] = sample;
  }
}
