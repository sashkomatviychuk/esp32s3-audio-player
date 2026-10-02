#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum { PLAYBACK_STOPPED = 0, PLAYBACK_PLAYING, PLAYBACK_PAUSED } playback_state_t;

/** Atomic snapshot of the whole player state (for display / BLE tasks). */
typedef struct {
  playback_state_t playback;
  uint8_t volume;   // 0..100
  int track_index;  // index into the SD track list
  int track_count;  // number of tracks found on the SD card
  uint32_t elapsed_sec;   // playback position of the current track
  uint32_t duration_sec;  // estimated track length, 0 if unknown
} player_state_t;

/**
 * @brief Creates the internal state mutex and sets the initial values
 *        (STOPPED, volume 70, track 0 of 0).
 *
 * Must be called once, before any other player_state_* function and before
 * any task that uses the state is created.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the mutex could not be created
 */
esp_err_t player_state_init(void);

/**
 * @brief Copies a consistent snapshot of the whole state into @p out.
 *
 * Locking: takes the INTERNAL state mutex (not spi_mutex) — safe to call
 * with or without spi_mutex held.
 */
void player_state_get(player_state_t* out);

/** @brief Returns the current playback state. Locking: internal state mutex only. */
playback_state_t player_state_get_playback(void);

/** @brief Returns the current volume (0..100). Locking: internal state mutex only. */
uint8_t player_state_get_volume(void);

/** @brief Sets the playback state. Locking: internal state mutex only. */
void player_state_set_playback(playback_state_t playback);

/**
 * @brief Changes the volume by @p delta, clamped to 0..100.
 *
 * Only updates the stored value — it does NOT touch the codec. The caller
 * is responsible for applying the returned value with vs1053_set_volume()
 * (which takes spi_mutex itself), outside of any state lock.
 *
 * @return The new volume; equal to the old one if already at a limit
 */
uint8_t player_state_change_volume(int delta);

/**
 * @brief Selects the track at @p index.
 *
 * Locking: internal state mutex only.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if @p index is not in
 *         0..track_count-1 (the stored index is left unchanged)
 */
esp_err_t player_state_select_track(int index);

/**
 * @brief Moves to the next track. Does NOT wrap around: at the last track
 *        the call is ignored and the index is left unchanged.
 *
 * Locking: internal state mutex only.
 *
 * @return true if the index changed, false if already at the last track
 */
bool player_state_next_track(void);

/**
 * @brief Moves to the previous track. Does NOT wrap around: at the first
 *        track the call is ignored and the index is left unchanged.
 *
 * Locking: internal state mutex only.
 *
 * @return true if the index changed, false if already at the first track
 */
bool player_state_prev_track(void);

/** @brief Sets the number of available tracks. Locking: internal state mutex only. */
void player_state_set_track_count(int count);

/**
 * @brief Publishes the playback position and the (estimated) track length.
 *
 * Called by audio_task. Locking: internal state mutex only.
 *
 * @param elapsedSec  Seconds played so far
 * @param durationSec Track length in seconds, 0 if unknown
 */
void player_state_set_progress(uint32_t elapsedSec, uint32_t durationSec);
