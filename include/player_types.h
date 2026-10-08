#pragma once

/** Playback state shared by audio_task (the only writer), the display and BLE. */
typedef enum { PLAYBACK_STOPPED = 0, PLAYBACK_PLAYING, PLAYBACK_PAUSED } playback_state_t;

typedef enum {
  CMD_PLAY_PAUSE = 0,
  CMD_NEXT,
  CMD_PREV,
  CMD_VOLUME_UP,
  CMD_VOLUME_DOWN,
  CMD_SELECT_TRACK,  // uses player_cmd_t.index
  CMD_TOGGLE_MUTE,
} cmd_type_t;

typedef struct {
  cmd_type_t type;
  int index;  // track index, read only for CMD_SELECT_TRACK
} player_cmd_t;
