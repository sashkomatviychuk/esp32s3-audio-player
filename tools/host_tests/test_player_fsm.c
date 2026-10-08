// Host-side test of the playback state machine (pure C, no ESP-IDF needed).
//
// Run from the repository root:
//   cc -std=c11 -Wall -Wextra -Iinclude tools/host_tests/test_player_fsm.c src/player_fsm.c \
//      -o /tmp/test_player_fsm && /tmp/test_player_fsm

#include <stdio.h>

#include "player_fsm.h"

static int s_failed = 0;

static void expect(playback_state_t from, player_event_t event, bool valid, playback_state_t next,
                   player_action_t action) {
  player_transition_t t = player_fsm_step(from, event);
  if (t.valid != valid || t.next != next || t.action != action) {
    printf("FAIL: %s --%s--> got valid=%d next=%s action=%d, want valid=%d next=%s action=%d\n",
           player_fsm_state_name(from), player_fsm_event_name(event), t.valid,
           player_fsm_state_name(t.next), t.action, valid, player_fsm_state_name(next), action);
    s_failed++;
  }
}

#define OK(from, ev, next, act) expect(from, ev, true, next, act)
#define INVALID(from, ev) expect(from, ev, false, from, PLAYER_ACT_NONE)

int main(void) {
  // STOPPED
  OK(PLAYBACK_STOPPED, PLAYER_EVT_PLAY_PAUSE, PLAYBACK_STOPPED, PLAYER_ACT_START_TRACK);
  OK(PLAYBACK_STOPPED, PLAYER_EVT_TRACK_SELECTED, PLAYBACK_STOPPED, PLAYER_ACT_START_TRACK);
  OK(PLAYBACK_STOPPED, PLAYER_EVT_SD_RETURNED, PLAYBACK_STOPPED, PLAYER_ACT_START_TRACK);
  OK(PLAYBACK_STOPPED, PLAYER_EVT_TRACK_STARTED, PLAYBACK_PLAYING, PLAYER_ACT_NONE);
  OK(PLAYBACK_STOPPED, PLAYER_EVT_TRACK_FAILED, PLAYBACK_STOPPED, PLAYER_ACT_NONE);
  OK(PLAYBACK_STOPPED, PLAYER_EVT_SD_LOST, PLAYBACK_STOPPED, PLAYER_ACT_NONE);
  INVALID(PLAYBACK_STOPPED, PLAYER_EVT_TRACK_EOF_NEXT);
  INVALID(PLAYBACK_STOPPED, PLAYER_EVT_TRACK_EOF_LAST);

  // PLAYING
  OK(PLAYBACK_PLAYING, PLAYER_EVT_PLAY_PAUSE, PLAYBACK_PAUSED, PLAYER_ACT_NONE);
  OK(PLAYBACK_PLAYING, PLAYER_EVT_TRACK_SELECTED, PLAYBACK_PLAYING, PLAYER_ACT_START_TRACK);
  OK(PLAYBACK_PLAYING, PLAYER_EVT_TRACK_STARTED, PLAYBACK_PLAYING, PLAYER_ACT_NONE);
  OK(PLAYBACK_PLAYING, PLAYER_EVT_TRACK_EOF_NEXT, PLAYBACK_PLAYING, PLAYER_ACT_START_TRACK);
  OK(PLAYBACK_PLAYING, PLAYER_EVT_TRACK_EOF_LAST, PLAYBACK_STOPPED, PLAYER_ACT_NONE);
  OK(PLAYBACK_PLAYING, PLAYER_EVT_TRACK_FAILED, PLAYBACK_STOPPED, PLAYER_ACT_NONE);
  OK(PLAYBACK_PLAYING, PLAYER_EVT_SD_LOST, PLAYBACK_STOPPED, PLAYER_ACT_NONE);
  INVALID(PLAYBACK_PLAYING, PLAYER_EVT_SD_RETURNED);

  // PAUSED
  OK(PLAYBACK_PAUSED, PLAYER_EVT_PLAY_PAUSE, PLAYBACK_PLAYING, PLAYER_ACT_NONE);
  OK(PLAYBACK_PAUSED, PLAYER_EVT_TRACK_SELECTED, PLAYBACK_PAUSED, PLAYER_ACT_START_TRACK);
  OK(PLAYBACK_PAUSED, PLAYER_EVT_TRACK_STARTED, PLAYBACK_PLAYING, PLAYER_ACT_NONE);
  OK(PLAYBACK_PAUSED, PLAYER_EVT_TRACK_FAILED, PLAYBACK_STOPPED, PLAYER_ACT_NONE);
  OK(PLAYBACK_PAUSED, PLAYER_EVT_SD_LOST, PLAYBACK_STOPPED, PLAYER_ACT_NONE);
  INVALID(PLAYBACK_PAUSED, PLAYER_EVT_TRACK_EOF_NEXT);
  INVALID(PLAYBACK_PAUSED, PLAYER_EVT_TRACK_EOF_LAST);
  INVALID(PLAYBACK_PAUSED, PLAYER_EVT_SD_RETURNED);

  // Out-of-range arguments never match a transition.
  INVALID((playback_state_t)99, PLAYER_EVT_PLAY_PAUSE);
  INVALID(PLAYBACK_STOPPED, PLAYER_EVT_COUNT);
  INVALID(PLAYBACK_STOPPED, (player_event_t)-1);

  // Every valid transition must stay inside the state range.
  for (int s = PLAYBACK_STOPPED; s <= PLAYBACK_PAUSED; s++) {
    for (int e = 0; e < PLAYER_EVT_COUNT; e++) {
      player_transition_t t = player_fsm_step((playback_state_t)s, (player_event_t)e);
      if (t.next < PLAYBACK_STOPPED || t.next > PLAYBACK_PAUSED) {
        printf("FAIL: %s --%s--> next state out of range\n",
               player_fsm_state_name((playback_state_t)s),
               player_fsm_event_name((player_event_t)e));
        s_failed++;
      }
    }
  }

  if (s_failed == 0) {
    printf("player_fsm: all transitions OK\n");
    return 0;
  }
  printf("player_fsm: %d check(s) failed\n", s_failed);
  return 1;
}
