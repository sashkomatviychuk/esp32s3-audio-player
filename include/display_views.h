#pragma once

#include "player_state.h"

/**
 * Full-screen layouts the display can show. To add a view: add a value here
 * (before DISPLAY_VIEW_COUNT), a render function + table entry in
 * display_views.c, and a routing rule in select_view() in display_task.c.
 */
typedef enum {
  DISPLAY_VIEW_PLAYER = 0,  // header + track list
  DISPLAY_VIEW_NO_SD,       // "Please insert SD card" warning
  DISPLAY_VIEW_COUNT
} display_view_t;

/**
 * @brief Draws a whole screen for @p view into the framebuffer (the caller
 *        clears before and flushes after). Unknown views are ignored with a
 *        warning. Reads track names from the SD module's in-memory list (no SD
 *        access, no mutex). Framebuffer only.
 */
void display_render_view(display_view_t view, const player_state_t* state);
