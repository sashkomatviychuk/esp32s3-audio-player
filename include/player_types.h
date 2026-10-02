#pragma once

typedef enum {
  CMD_PLAY_PAUSE = 0,
  CMD_NEXT,
  CMD_PREV,
  CMD_VOLUME_UP,
  CMD_VOLUME_DOWN,
} cmd_type_t;

typedef struct {
  cmd_type_t type;
} player_cmd_t;
