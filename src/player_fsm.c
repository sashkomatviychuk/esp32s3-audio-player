#include "player_fsm.h"

#define PLAYER_FSM_STATE_COUNT 3

_Static_assert(PLAYBACK_PAUSED + 1 == PLAYER_FSM_STATE_COUNT,
               "PLAYER_FSM_STATE_COUNT must match playback_state_t");

#define TRANSITION(next_state, act) {.valid = true, .next = (next_state), .action = (act)}

// Rows are states, columns are events. Entries left out stay zero-initialized (valid == false).
static const player_transition_t TRANSITIONS[PLAYER_FSM_STATE_COUNT][PLAYER_EVT_COUNT] = {
    [PLAYBACK_STOPPED] =
        {
            [PLAYER_EVT_PLAY_PAUSE] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_START_TRACK),
            [PLAYER_EVT_TRACK_SELECTED] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_START_TRACK),
            [PLAYER_EVT_TRACK_STARTED] = TRANSITION(PLAYBACK_PLAYING, PLAYER_ACT_NONE),
            [PLAYER_EVT_TRACK_FAILED] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_NONE),
            [PLAYER_EVT_SD_LOST] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_NONE),
            [PLAYER_EVT_SD_RETURNED] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_START_TRACK),
        },
    [PLAYBACK_PLAYING] =
        {
            [PLAYER_EVT_PLAY_PAUSE] = TRANSITION(PLAYBACK_PAUSED, PLAYER_ACT_NONE),
            [PLAYER_EVT_TRACK_SELECTED] = TRANSITION(PLAYBACK_PLAYING, PLAYER_ACT_START_TRACK),
            [PLAYER_EVT_TRACK_STARTED] = TRANSITION(PLAYBACK_PLAYING, PLAYER_ACT_NONE),
            [PLAYER_EVT_TRACK_EOF_NEXT] = TRANSITION(PLAYBACK_PLAYING, PLAYER_ACT_START_TRACK),
            [PLAYER_EVT_TRACK_EOF_LAST] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_NONE),
            [PLAYER_EVT_TRACK_FAILED] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_NONE),
            [PLAYER_EVT_SD_LOST] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_NONE),
        },
    [PLAYBACK_PAUSED] =
        {
            [PLAYER_EVT_PLAY_PAUSE] = TRANSITION(PLAYBACK_PLAYING, PLAYER_ACT_NONE),
            [PLAYER_EVT_TRACK_SELECTED] = TRANSITION(PLAYBACK_PAUSED, PLAYER_ACT_START_TRACK),
            [PLAYER_EVT_TRACK_STARTED] = TRANSITION(PLAYBACK_PLAYING, PLAYER_ACT_NONE),
            [PLAYER_EVT_TRACK_FAILED] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_NONE),
            [PLAYER_EVT_SD_LOST] = TRANSITION(PLAYBACK_STOPPED, PLAYER_ACT_NONE),
        },
};

player_transition_t player_fsm_step(playback_state_t state, player_event_t event) {
  if ((int)state < 0 || (int)state >= PLAYER_FSM_STATE_COUNT || (int)event < 0 ||
      (int)event >= PLAYER_EVT_COUNT) {
    return (player_transition_t){.valid = false, .next = state, .action = PLAYER_ACT_NONE};
  }
  player_transition_t transition = TRANSITIONS[state][event];
  if (!transition.valid) {
    transition.next = state;
  }
  return transition;
}

const char* player_fsm_state_name(playback_state_t state) {
  switch (state) {
    case PLAYBACK_STOPPED:
      return "STOPPED";
    case PLAYBACK_PLAYING:
      return "PLAYING";
    case PLAYBACK_PAUSED:
      return "PAUSED";
    default:
      return "?";
  }
}

const char* player_fsm_event_name(player_event_t event) {
  switch (event) {
    case PLAYER_EVT_PLAY_PAUSE:
      return "PLAY_PAUSE";
    case PLAYER_EVT_TRACK_SELECTED:
      return "TRACK_SELECTED";
    case PLAYER_EVT_TRACK_STARTED:
      return "TRACK_STARTED";
    case PLAYER_EVT_TRACK_EOF_NEXT:
      return "TRACK_EOF_NEXT";
    case PLAYER_EVT_TRACK_EOF_LAST:
      return "TRACK_EOF_LAST";
    case PLAYER_EVT_TRACK_FAILED:
      return "TRACK_FAILED";
    case PLAYER_EVT_SD_LOST:
      return "SD_LOST";
    case PLAYER_EVT_SD_RETURNED:
      return "SD_RETURNED";
    default:
      return "?";
  }
}
