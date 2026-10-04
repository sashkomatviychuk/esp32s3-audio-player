#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

// Audio debug settings (single-file debug mode, diagnostic logging) live in
// Kconfig: see src/Kconfig.projbuild, menu "MP3 player: audio debug"
// (CONFIG_AUDIO_DEBUG_*). The header only pulls in sdkconfig.h so audio_task,
// player_state and the display share one switch point.

/**
 * @brief Initializes the audio module: stores the passed-in handles
 *        and creates audio_task.
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
