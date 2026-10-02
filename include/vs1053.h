#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/spi_types.h"

/**
 * @brief Initializes the VS1053: configures GPIOs (XCS/XDCS/DREQ/RESET),
 *        initializes its OWN SPI bus (SPI3_HOST, separate from the SD
 *        card's SPI2 bus), adds two SPI devices on it (low-speed SCI,
 *        high-speed SDI), runs the reset sequence, and applies base
 *        settings (SM_LINE1|SM_SDINEW, 44.1kHz stereo, clock multiplier).
 *
 * @param spi_mutex Mutex serializing SD card and VS1053 access from
 *                  audio_task. Taken INTERNALLY during init and in
 *                  vs1053_set_volume(). NOT taken inside
 *                  vs1053_write_sdi() — that function must be called while
 *                  the caller (audio_task) already holds spi_mutex itself
 *                  (to avoid double-locking a non-recursive mutex).
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if spi_mutex == NULL,
 *         another esp_err_t error if the SPI bus init or adding the SPI
 *         device failed
 */
esp_err_t vs1053_init(SemaphoreHandle_t spi_mutex);

/**
 * @brief Configures the VS1053 XCS/XDCS pins as outputs and drives them
 *        HIGH (deselected) immediately, WITHOUT touching the SPI bus.
 *
 * The VS1053 now has its own SPI bus (no longer shared with the SD card),
 * so a floating XCS/XDCS can no longer corrupt SD traffic. Still worth
 * calling early: it keeps the chip deselected (no spurious SCI/SDI
 * activity) until vs1053_init() runs.
 *
 * vs1053_init() re-configures and re-asserts these same pins as part of
 * its own setup, so calling this first is safe and does not need to be
 * undone.
 */
void vs1053_deselect_early(void);

/**
 * @brief Sends MP3 data to the VS1053 over the SDI (data) interface.
 *        Maximum 32 bytes per call (VS1053 limitation).
 *
 * IMPORTANT: this function does NOT take spi_mutex itself — the caller
 * (send_to_codec() in audio_task.c) must hold spi_mutex at the time of
 * the call, so SD and VS1053 accesses from different tasks stay serialized.
 *
 * @param data  Buffer with MP3 data
 * @param bytes Number of bytes, must be <= 32
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if bytes > 32,
 *         ESP_ERR_TIMEOUT if DREQ didn't go high in time (VS1053
 *         unresponsive), another esp_err_t on SPI failure
 */
esp_err_t vs1053_write_sdi(const uint8_t* data, uint8_t bytes);

/**
 * @brief Sets the volume (0..100, 100 — loudest, same for both
 *        channels). Takes spi_mutex INTERNALLY — call it WITHOUT
 *        holding the mutex externally beforehand.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if vs1053_init()
 *         hasn't been called yet, ESP_ERR_TIMEOUT if DREQ didn't go
 *         high in time (VS1053 unresponsive), another esp_err_t on
 *         SPI failure
 */
esp_err_t vs1053_set_volume(uint8_t vol);

/**
 * @brief Diagnostic: makes the VS1053 generate a ~430Hz sine tone on its own
 *        (SM_TESTS + datasheet sine test sequence), independent of MP3
 *        decoding and the SD card. If the tone is audible, the clock, DAC
 *        and headphone output are fine; if silent, the problem is the
 *        crystal/analog supply/output wiring. Blocks for duration_ms.
 *        Takes spi_mutex INTERNALLY — call it WITHOUT holding the mutex.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if vs1053_init() hasn't
 *         been called yet, another esp_err_t on SPI/DREQ failure
 */
esp_err_t vs1053_sine_test(uint32_t duration_ms);

/**
 * @brief Diagnostic helper: reads SCI_STATUS, SCI_HDAT0 and SCI_HDAT1 and
 *        logs them. SCI_HDAT0/1 are filled in by the decoder from the most
 *        recently parsed MPEG frame header — if they stay 0x0000 while
 *        streaming, the VS1053 is not recognizing any valid MP3 frames in
 *        the SDI data it's being sent. Takes spi_mutex INTERNALLY — call it
 *        WITHOUT holding the mutex externally beforehand.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if vs1053_init() hasn't
 *         been called yet, another esp_err_t on SPI/DREQ failure
 */
esp_err_t vs1053_log_decode_status(void);

/**
 * @brief Reads SCI_DECODE_TIME — seconds of audio decoded since the last
 *        reset (see vs1053_reset_decode_time()). Does not advance while the
 *        stream is paused. Takes spi_mutex INTERNALLY — call it WITHOUT
 *        holding the mutex externally beforehand.
 *
 * @param[out] sec Decoded seconds
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if vs1053_init() hasn't
 *         been called yet, ESP_ERR_INVALID_ARG if @p sec is NULL, another
 *         esp_err_t on SPI/DREQ failure
 */
esp_err_t vs1053_get_decode_time(uint16_t* sec);

/**
 * @brief Resets SCI_DECODE_TIME to 0 (written twice, as the datasheet
 *        requires). Call when a new track starts. Takes spi_mutex
 *        INTERNALLY — call it WITHOUT holding the mutex externally.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if vs1053_init() hasn't
 *         been called yet, another esp_err_t on SPI/DREQ failure
 */
esp_err_t vs1053_reset_decode_time(void);
