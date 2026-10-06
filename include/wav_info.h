#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

typedef struct {
  uint32_t sample_rate;   // Hz
  uint16_t channels;      // 1 (mono) or 2 (stereo)
  uint16_t bits;          // bits per sample, always 16 for a supported file
  uint32_t byte_rate;     // bytes of audio data per second
  uint32_t data_start;    // file offset of the first audio byte
  uint32_t data_size;     // bytes of audio data (clamped to what the file really holds)
  uint32_t duration_sec;  // length of the audio data, rounded down
} wav_info_t;

/**
 * @brief Parses the RIFF/WAVE header of a file: walks the chunks, reads the
 *        "fmt " chunk, skips unknown ones (LIST, etc.) and locates the "data"
 *        chunk.
 *
 * Supported: uncompressed PCM (format tag 1, or 0xFFFE WAVE_FORMAT_EXTENSIBLE
 * carrying PCM), 16 bits per sample, 1 or 2 channels, 8000..48000 Hz. A "data"
 * size of 0 or larger than the rest of the file (streamed/unfinished headers)
 * is clamped to the end of the file. On success the file position is left at
 * the first audio byte (@c data_start).
 *
 * Locking: reads from the SD card but takes no mutex — the caller must hold
 * spi_mutex (or be the only SD user at that moment).
 *
 * @param f         Open file, positioned anywhere
 * @param file_size Total file size in bytes
 * @param[out] out  Result; zeroed on failure
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on NULL arguments,
 *         ESP_ERR_INVALID_RESPONSE if the file is not a valid WAV or is
 *         truncated, ESP_ERR_NOT_SUPPORTED if the format is valid but not
 *         playable (compressed, not 16-bit, more than 2 channels, odd rate)
 */
esp_err_t wav_get_info(FILE* f, size_t file_size, wav_info_t* out);
