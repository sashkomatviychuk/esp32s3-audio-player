#pragma once

#include <stdint.h>

#include "esp_err.h"

#define DISPLAY_WIDTH 128
#define GLYPH_SIZE 8

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

// ---- Drawing primitives (framebuffer only, no-ops before display_init) ----

/**
 * @brief Draws up to @p max_chars characters of @p text with the 8x8 Latin
 *        font at (x, y). Characters outside the font are drawn as '?'.
 */
void display_draw_text(int x, int y, const char* text, int max_chars);

/**
 * @brief Draws the whole @p text starting at @p x, but only the pixels inside the
 *        window [@p clip_x, @p clip_x + @p clip_w) — the rest is cut off. @p x may be
 *        negative or beyond the window, which is how text is scrolled.
 *
 * Use this instead of display_draw_text() whenever the text can leave the screen:
 * pixel coordinates are 8-bit in the panel library, so an unclipped x outside
 * 0..255 would wrap around and draw garbage on the other side.
 */
void display_draw_text_clipped(int x, int y, const char* text, int clip_x, int clip_w);

/** @brief Draws @p text horizontally centered on the row at @p y. */
void display_draw_text_centered(int y, const char* text);

/** @brief Draws a 1px frame with rounded corners. */
void display_draw_rounded_frame(int x, int y, int w, int h);

/** @brief Draws a progress bar outline filled to @p percent (0-100). */
void display_draw_bar(int x, int y, int w, int h, int percent);

/** @brief Draws a full-width horizontal line at @p y. */
void display_draw_hline(int y);

/**
 * @brief Draws a 1-bit bitmap, row-major, MSB = leftmost pixel (the format of
 *        ssd1306_set_bitmap), @p w x @p h pixels at (x, y).
 */
void display_draw_bitmap(int x, int y, const uint8_t* bitmap, int w, int h);
