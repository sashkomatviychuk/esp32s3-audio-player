#pragma once

#include <stddef.h>

#include "driver/spi_common.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SD_MAX_TRACKS 64
#define SD_MAX_NAME 64

/**
 * @brief Initializes the SD card's SPI bus (SPI2) and mounts the card (FAT32).
 *
 * The VS1053 uses its own separate bus (SPI3), initialized in vs1053_init().
 *
 * Locking: takes no mutex — call it before any task that uses the card
 * is created.
 *
 * @param[out] out_host SPI host the SD bus was initialized on
 *
 * @return ESP_OK on success, an error if the bus init, card init or mount failed
 */
esp_err_t sd_card_init(spi_host_device_t* out_host);

/** @brief Returns the SD mount point (e.g. "/sdcard"). */
const char* sd_card_get_mount_point(void);

/**
 * @brief Scans the mount point root for .mp3 files (case-insensitive) and
 *        stores up to SD_MAX_TRACKS names. Also publishes the track count
 *        via player_state_set_track_count().
 *
 * Locking: takes @p spi_mutex internally for the duration of the scan
 * (the SD card shares the bus with the VS1053) — call WITHOUT holding it.
 *
 * @param spi_mutex Mutex protecting the shared SPI bus
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if @p spi_mutex is NULL,
 *         ESP_FAIL if the directory could not be opened or the mutex taken
 */
esp_err_t sd_card_scan_tracks(SemaphoreHandle_t spi_mutex);

/** @brief Returns the number of tracks found by the last sd_card_scan_tracks(). */
int sd_card_get_track_count(void);

/**
 * @brief Returns the file name (without directory) of track @p index, e.g. for
 *        the track list screen. Together with sd_card_get_track_count() this
 *        gives access to the whole list.
 *
 * Locking: none — only reads the in-memory track list, no SD access.
 *
 * @return Pointer to a string owned by this module (valid until the next
 *         sd_card_scan_tracks()), or NULL if @p index is out of range
 */
const char* sd_card_get_track_name(int index);

/**
 * @brief Writes the full path ("<mount point>/<name>") of track @p index into @p buf.
 *
 * Locking: none — only reads the in-memory track list, no SD access.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if @p index is out of range
 *         or @p buf is NULL, ESP_ERR_INVALID_SIZE if the path does not fit
 */
esp_err_t sd_card_get_track_path(int index, char* buf, size_t buf_size);
