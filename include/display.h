#pragma once

#include "esp_err.h"
#include "player_state.h"

/**
 * @brief Initializes the I2C bus (I2C_NUM_0, SDA 17 / SCL 18) and the 128x64
 *        SSD1306 panel, and clears it.
 *
 * All drawing goes to the library's RAM framebuffer; nothing reaches the
 * panel until display_flush(). Not thread-safe: the display is meant to be
 * driven from a single task (display_task). Takes no spi_mutex — the display
 * is on a separate I2C bus.
 *
 * @return ESP_OK on success, an error if the I2C bus or the panel failed to
 *         initialize (e.g. display not connected)
 */
esp_err_t display_init(void);

/** @brief Clears the framebuffer (RAM only, the panel is not touched). */
void display_clear(void);

/** @brief Sends the framebuffer to the panel over I2C. */
esp_err_t display_flush(void);

/**
 * @brief Draws the first row: "MM:SS/MM:SS" (or "--:--" for an unknown
 *        duration), a play/pause/stop icon, a debug marker (only when
 *        AUDIO_DEBUG_MODE is on) and the volume bar. Framebuffer only.
 */
void display_render_header(const player_state_t* state);

/**
 * @brief Draws the track list below the header: up to 5 rows, names cut to
 *        the row width, the current track in a rounded frame. The visible
 *        window follows state->track_index. Reads names from the SD module's
 *        in-memory list (no SD access, no mutex). Framebuffer only.
 */
void display_render_list(const player_state_t* state);
