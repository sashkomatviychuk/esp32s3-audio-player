#pragma once

#include <stdbool.h>

#include "player_types.h"

/**
 * @file player_fsm.h
 * @brief Playback state machine: a pure transition table, no I/O and no RTOS calls.
 *
 * States are playback_state_t (STOPPED, PLAYING, PAUSED). audio_task feeds events into
 * player_fsm_step(), publishes the returned state through player_state and executes the returned
 * action. The module owns no state, so it can be unit-tested on the host (tools/host_tests).
 *
 * Transitions (event -> next state [action]):
 *
 *   STOPPED  PLAY_PAUSE      -> STOPPED  [START_TRACK]
 *            TRACK_SELECTED  -> STOPPED  [START_TRACK]
 *            SD_RETURNED     -> STOPPED  [START_TRACK]
 *            TRACK_STARTED   -> PLAYING
 *            TRACK_FAILED, SD_LOST -> STOPPED
 *   PLAYING  PLAY_PAUSE      -> PAUSED
 *            TRACK_SELECTED  -> PLAYING  [START_TRACK]
 *            TRACK_STARTED   -> PLAYING
 *            TRACK_EOF_NEXT  -> PLAYING  [START_TRACK]
 *            TRACK_EOF_LAST, TRACK_FAILED, SD_LOST -> STOPPED
 *   PAUSED   PLAY_PAUSE      -> PLAYING
 *            TRACK_SELECTED  -> PAUSED   [START_TRACK]
 *            TRACK_STARTED   -> PLAYING
 *            TRACK_FAILED, SD_LOST -> STOPPED
 *
 * START_TRACK is a request: the executor opens the track at the current index and reports
 * TRACK_STARTED (or TRACK_FAILED). Any (state, event) pair not listed is invalid.
 */

typedef enum {
  PLAYER_EVT_PLAY_PAUSE = 0,  // user pressed Play/Pause
  PLAYER_EVT_TRACK_SELECTED,  // Next / Prev / Select moved to another track
  PLAYER_EVT_TRACK_STARTED,   // the track file was opened and streaming begins
  PLAYER_EVT_TRACK_EOF_NEXT,  // the track ended and a next track exists
  PLAYER_EVT_TRACK_EOF_LAST,  // the last track ended
  PLAYER_EVT_TRACK_FAILED,    // open / decode / I2S error
  PLAYER_EVT_SD_LOST,         // the SD card dropped out or a read failed
  PLAYER_EVT_SD_RETURNED,     // the card is back and playback should restart
  PLAYER_EVT_COUNT
} player_event_t;

typedef enum {
  PLAYER_ACT_NONE = 0,
  PLAYER_ACT_START_TRACK,  // (re)start streaming the track at the current index
} player_action_t;

typedef struct {
  bool valid;  // false: the event is not allowed in this state, nothing must change
  playback_state_t next;
  player_action_t action;
} player_transition_t;

/**
 * @brief Looks up the transition for @p event in @p state.
 *
 * Pure function: no locking, no side effects. For an invalid pair (or out-of-range arguments)
 * the result has valid == false, next == @p state and action == PLAYER_ACT_NONE.
 */
player_transition_t player_fsm_step(playback_state_t state, player_event_t event);

/** @brief Human-readable names for logging. Never NULL ("?" for out-of-range values). */
const char* player_fsm_state_name(playback_state_t state);
const char* player_fsm_event_name(player_event_t event);
