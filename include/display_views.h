#pragma once

#include <stdbool.h>
#include <stdint.h>

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
 *
 * @param anim_ms Milliseconds since the current animation started (the name of
 *                the selected track scrolls when it does not fit its row). The
 *                caller restarts it at 0 whenever the selected track changes.
 *                Views without animation ignore it.
 */
void display_render_view(display_view_t view, const player_state_t* state, uint32_t anim_ms);

/**
 * @brief Tells whether @p view currently changes with time alone, i.e. has to be
 *        redrawn even if @p state did not change — true for the player view while
 *        the selected track's name is too long for its row and therefore scrolls.
 *        Uses the same name and width rules as display_render_view().
 */
bool display_view_is_animated(display_view_t view, const player_state_t* state);
