#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Initializes the PCM5102 I2S DAC output: creates the I2S0 TX channel
 *        (standard Philips mode, 16-bit, always stereo, 44.1 kHz until
 *        pcm5102_set_sample_rate() changes it) on WS (LRCK) = GPIO4,
 *        DOUT (DIN) = GPIO5, BCK = GPIO6, without MCLK, and enables it.
 *
 * Hardware notes: the module's SCK pin must be tied to GND (the DAC then
 * derives its system clock from BCK with its internal PLL) and the XSMT
 * solder jumper must be set High, otherwise the output stays soft-muted.
 *
 * The DMA buffers are cleared automatically when the application stops
 * writing, so a paused player outputs silence instead of repeating samples.
 *
 * Locking: none. The module is meant for a single writer (audio_task).
 *
 * @return ESP_OK on success, otherwise the i2s driver error
 */
esp_err_t pcm5102_init(void);

/**
 * @brief Changes the I2S sample rate. The channel is disabled and re-enabled
 *        around the change, which also drops any samples left in the DMA
 *        buffers — call it at the start of every track.
 *
 * @param sample_rate_hz Sample rate in Hz (e.g. 44100)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if pcm5102_init() hasn't
 *         been called yet, another esp_err_t on driver failure
 */
esp_err_t pcm5102_set_sample_rate(uint32_t sample_rate_hz);

/**
 * @brief Applies the current volume/mute gain to @p samples IN PLACE and sends
 *        them to the DAC. Blocks until all frames are queued in the DMA
 *        buffers, so the caller is paced by the sample rate.
 *
 * @param samples Interleaved stereo 16-bit samples (L, R, L, R, ...); modified
 * @param frames  Number of stereo frames (one frame = one L and one R sample)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if pcm5102_init() hasn't
 *         been called yet, ESP_ERR_INVALID_ARG if @p samples is NULL,
 *         ESP_ERR_TIMEOUT if the DMA did not accept the data in time
 */
esp_err_t pcm5102_write(int16_t* samples, size_t frames);

/**
 * @brief Sets the software volume (0..100, 100 — unchanged samples). The
 *        PCM5102 has no volume register, so the samples are scaled in
 *        pcm5102_write(). The curve is quadratic, which sounds closer to
 *        linear than a linear gain would.
 *
 * Locking: none — the value is a single word read by the writer task.
 */
void pcm5102_set_volume(uint8_t volume);

/**
 * @brief Mutes or unmutes the output. The volume set by
 *        pcm5102_set_volume() is kept, so unmuting restores it.
 *
 * Locking: none — the value is a single word read by the writer task.
 */
void pcm5102_set_mute(bool muted);
