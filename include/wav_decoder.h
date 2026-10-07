#pragma once

#include <stdio.h>

#include "audio_decoder.h"

/**
 * @brief Parses the header of an open WAV file (see wav_get_info()) into @p info.
 *
 * Locking: reads from the SD card but takes no mutex — the caller (audio_decoder_open/probe)
 * holds spi_mutex.
 *
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED (not 16-bit PCM, more than 2 channels, odd sample rate)
 *         or ESP_ERR_INVALID_RESPONSE (not a valid WAV)
 */
esp_err_t wav_decoder_probe(FILE* f, size_t file_size, audio_track_info_t* info);

/**
 * @brief Prepares @p dec to stream the audio data of an open WAV file.
 *
 * On ESP_OK @p dec owns @p f (audio_decoder_close() closes it); on an error the caller keeps it.
 * Locking: as wav_decoder_probe() — spi_mutex is held by the caller; read() takes it itself.
 */
esp_err_t wav_decoder_open(FILE* f, size_t file_size, SemaphoreHandle_t spi_mutex,
                           audio_decoder_t* dec, audio_track_info_t* info);
