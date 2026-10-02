#pragma once

typedef enum {
  CMD_PLAY_PAUSE = 0,
  CMD_NEXT,
  CMD_PREV,
  CMD_VOLUME_UP,
  CMD_VOLUME_DOWN,
  CMD_SELECT_TRACK,  // uses player_cmd_t.index
} cmd_type_t;

typedef struct {
  cmd_type_t type;
  int index;  // track index, read only for CMD_SELECT_TRACK
} player_cmd_t;
