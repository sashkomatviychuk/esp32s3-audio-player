#pragma once

#include "esp_err.h"

/**
 * @brief Initializes the display and creates vDisplayTask, which redraws the
 *        screen from player_state every 250ms (only when something changed).
 *
 * Init-order dependencies: player_state_init() and sd_card_scan_tracks() must
 * have run (the task reads the state snapshot and the track names). Takes no
 * mutex of its own; the task never touches spi_mutex.
 *
 * @return ESP_OK on success, an error if the panel could not be initialized
 *         or the task could not be created. The player keeps working without
 *         a display, so callers should log and continue.
 */
esp_err_t display_task_init(void);
