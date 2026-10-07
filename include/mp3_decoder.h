#pragma once

#include <stdio.h>

#include "audio_decoder.h"

/**
 * @brief Reads the first MPEG frame header of an open MP3 file (see mp3_get_info()) into
 *        @p info: sample rate, channels and length.
 *
 * Locking: reads from the SD card but takes no mutex — the caller (audio_decoder_open/probe)
 * holds spi_mutex.
 *
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED if no MPEG Layer III frame was found near the start
 *         (Layer I/II files, or not an MP3 at all)
 */
esp_err_t mp3_decoder_probe(FILE* f, size_t file_size, audio_track_info_t* info);

/**
 * @brief Prepares @p dec to decode an open MP3 file with the Helix decoder: allocates the
 *        decoder state (~24 KB heap) and the input buffer and positions the file after the
 *        ID3v2 tag.
 *
 * On ESP_OK @p dec owns @p f (audio_decoder_close() closes it); on an error the caller keeps it.
 * Locking: as mp3_decoder_probe() — spi_mutex is held by the caller; read() takes it itself.
 *
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED (no Layer III frame), ESP_ERR_NO_MEM
 */
esp_err_t mp3_decoder_open(FILE* f, size_t file_size, SemaphoreHandle_t spi_mutex,
                           audio_decoder_t* dec, audio_track_info_t* info);
