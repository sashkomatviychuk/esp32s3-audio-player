#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/spi_types.h"

/**
 * @brief Initializes the VS1053: configures GPIOs (XCS/XDCS/DREQ/RESET),
 *        adds two SPI devices (low-speed SCI, high-speed SDI) on an
 *        ALREADY EXISTING SPI bus (does not call spi_bus_initialize
 *        itself), runs the reset sequence, and applies base settings
 *        (SM_LINE1|SM_SDINEW, 44.1kHz stereo, clock multiplier).
 *
 * @param host      SPI host on which spi_bus_initialize() has already
 *                  been called (the same host/bus as the SD card —
 *                  with separate CS pins)
 * @param spi_mutex Mutex protecting the shared SPI bus (VS1053 + SD card).
 *                  Taken INTERNALLY during init and in vs1053_set_volume().
 *                  NOT taken inside vs1053_write_sdi() — that function must
 *                  be called while the caller (audio_task) already holds
 *                  spi_mutex itself (to avoid double-locking a
 *                  non-recursive mutex).
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if spi_mutex == NULL,
 *         another esp_err_t error if adding the SPI device failed
 */
esp_err_t vs1053_init(spi_host_device_t host, SemaphoreHandle_t spi_mutex);

/**
 * @brief Configures the VS1053 XCS/XDCS pins as outputs and drives them
 *        HIGH (deselected) immediately, WITHOUT touching the SPI bus.
 *
 * Call this BEFORE spi_bus_initialize()/mounting the SD card, which
 * shares the same MISO/MOSI/SCLK lines. Without it, XCS/XDCS stay
 * floating (default GPIO input state) until vs1053_init() runs, so the
 * VS1053 can spuriously think it is selected and corrupt SD card SPI
 * traffic (e.g. garbled CSD reads) before vs1053_init() is ever called.
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
 * the call. Calling it without holding the mutex is a race on the
 * shared SPI bus (the same bus used by the SD card).
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
