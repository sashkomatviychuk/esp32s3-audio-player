#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** Largest number of stereo frames one audio_decoder_t::read() call can return (one MPEG-1 frame).
 */
#define AUDIO_DECODER_MAX_FRAMES 1152

/** Size in int16_t samples of the output buffer every read() call needs (stereo). */
#define AUDIO_DECODER_OUT_SAMPLES (AUDIO_DECODER_MAX_FRAMES * 2)

typedef enum {
  AUDIO_FORMAT_UNKNOWN = 0,
  AUDIO_FORMAT_WAV,
  AUDIO_FORMAT_MP3,
} audio_format_t;

typedef struct {
  audio_format_t format;
  uint32_t sample_rate;   // Hz
  uint16_t channels;      // 1 (mono) or 2 (stereo) in the FILE; the decoder output is always stereo
  uint32_t duration_sec;  // 0 if unknown
} audio_track_info_t;

typedef struct audio_decoder audio_decoder_t;

/**
 * A decoder that turns one open audio file into interleaved stereo 16-bit PCM. Created by
 * audio_decoder_open(), released by audio_decoder_close(). Not thread-safe: one task at a time.
 */
struct audio_decoder {
  /**
   * @brief Decodes the next block of audio.
   *
   * Locking: takes spi_mutex itself around every SD read — call it WITHOUT holding the mutex.
   * Blocks only for the time of the SD read and the decoding (a few ms).
   *
   * @param dec        This decoder
   * @param[out] out   Buffer of at least AUDIO_DECODER_OUT_SAMPLES int16_t; receives interleaved
   *                   stereo samples (L, R, L, R, ...). Mono sources are duplicated into both
   *                   channels, so the output format never depends on the file.
   * @param max_frames Capacity of @p out in stereo frames, at least AUDIO_DECODER_MAX_FRAMES
   * @param[out] frames Number of stereo frames written; 0 together with ESP_OK means the end
   *                   of the audio data
   *
   * @return ESP_OK, ESP_FAIL if the SD read failed (the errno is in read_errno),
   *         ESP_ERR_INVALID_RESPONSE if the data could not be decoded (corrupt file),
   *         ESP_ERR_INVALID_SIZE if @p max_frames is too small
   */
  esp_err_t (*read)(audio_decoder_t* dec, int16_t* out, size_t max_frames, size_t* frames);

  /** @brief Releases the decoder state and closes the file. Use audio_decoder_close(). */
  void (*close)(audio_decoder_t* dec);

  void* ctx;             // decoder-private state
  uint32_t sample_rate;  // rate of the stream NOW; read() updates it if the stream changes it
  int read_errno;        // errno of the failed SD read, valid after read() returned ESP_FAIL
};

/**
 * @brief Maps a file name to a supported audio format by its extension (".wav", ".mp3"),
 *        case-insensitively.
 *
 * Locking: none. Only looks at the name, so it is safe to call from any task.
 *
 * @return The format, or AUDIO_FORMAT_UNKNOWN for anything else (including NULL and names
 *         without an extension). Hidden files and directories are not detected here.
 */
audio_format_t audio_format_from_name(const char* name);

/**
 * @brief Opens @p path, parses its header and prepares a decoder for it.
 *
 * Locking: takes @p spi_mutex internally for the file open and the header parsing — call it
 * WITHOUT holding it.
 *
 * @param path       Full file path (the format is chosen by its extension)
 * @param spi_mutex  Mutex serializing SD access; kept by the decoder for its read() calls
 * @param[out] dec   The decoder; valid only if ESP_OK is returned
 * @param[out] info  Format, sample rate, channels and length of the track
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG on NULL arguments, ESP_ERR_NOT_SUPPORTED for an unknown
 *         extension or a format that cannot be played (24-bit WAV, MPEG Layer I/II, ...),
 *         ESP_ERR_INVALID_RESPONSE for a corrupt header, ESP_ERR_NO_MEM, ESP_FAIL if the file
 *         could not be opened
 */
esp_err_t audio_decoder_open(const char* path, SemaphoreHandle_t spi_mutex, audio_decoder_t* dec,
                             audio_track_info_t* info);

/**
 * @brief Like audio_decoder_open(), but only parses the header (length, format) — no decoder
 *        state is allocated and the file is closed again. Used to show the length of a track
 *        that is not playing.
 *
 * Locking: takes @p spi_mutex internally — call it WITHOUT holding it.
 *
 * @return Same codes as audio_decoder_open()
 */
esp_err_t audio_decoder_probe(const char* path, SemaphoreHandle_t spi_mutex,
                              audio_track_info_t* info);

/** @brief Closes @p dec if it is open and resets it. Safe to call on a zeroed decoder. */
void audio_decoder_close(audio_decoder_t* dec);

/**
 * @brief Converts @p frames mono samples at the start of @p buf into @p frames stereo frames,
 *        IN PLACE (the buffer must hold 2 * @p frames samples). Helper for the decoders.
 */
void audio_decoder_mono_to_stereo(int16_t* buf, size_t frames);
