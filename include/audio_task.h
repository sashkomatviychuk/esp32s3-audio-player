#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// -----------------------------------------------------------------
// Switch between "single-file debug" and "real project logic".
//
// AUDIO_DEBUG_MODE 1 — audio_task always plays AUDIO_DEBUG_FILENAME,
//   ignoring the track list. Set AUDIO_DEBUG_FILENAME to "demo_audio.mp3"
//   or "demo_audio_2.mp3", rebuild — and the chosen file plays.
//
//   NOTE: Next/Prev/Select still update player_state's track_index in
//   this mode (and restart the stream), but the file played stays
//   AUDIO_DEBUG_FILENAME. The display shows the played file name on the
//   highlighted row and a 'D' marker in the header.
//
// AUDIO_DEBUG_MODE 0 — the track at player_state's track_index is played
//   from the SD module's track list.
//
// Lives in the header so audio_task and the display share one switch point.
// -----------------------------------------------------------------
#define AUDIO_DEBUG_MODE 1
#define AUDIO_DEBUG_FILENAME "demo_audio.mp3"

/**
 * @brief Initializes the audio module: stores the passed-in handles
 *        and creates vAudioTask.
 *
 * @param cmd_queue   Shared command Queue (created in main.c,
 *                    written to by input_task and ble_task)
 * @param spi_mutex   Mutex protecting the shared SPI bus (VS1053 + SD card)
 *
 * Init-order dependencies: the SD card must be mounted (sd_card_init), the
 * track list scanned (sd_card_scan_tracks) and player_state_init() called
 * before this function. Playback state and volume live in player_state.
 *
 * @return ESP_OK on success, an error if the handles are NULL or the
 *         task could not be created
 */
esp_err_t audio_task_init(QueueHandle_t cmd_queue, SemaphoreHandle_t spi_mutex);
