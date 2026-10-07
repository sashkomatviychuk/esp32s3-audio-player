#include "display_views.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "audio_decoder.h"
#include "audio_task.h"
#include "display.h"
#include "esp_log.h"
#include "sd_card.h"

static const char* TAG = "display_views";

// ---- Layout (128x64) ----
#define ICON_SIZE 8
#define MAX_TIME_SEC ((99 * 60) + 59)  // "99:59" is the widest time that fits the header
#define HEADER_TIME_CHARS 11           // "MM:SS/MM:SS"
#define TIME_BUF_SIZE 16
#define HEADER_Y 0
#define HEADER_TIME_X 0
#define HEADER_ICON_X 92
#define HEADER_DEBUG_X 100
#define HEADER_BAR_X 110
#define HEADER_BAR_W 18
#define HEADER_BAR_H 7
#define SEPARATOR_Y 9
#define LIST_Y 12
#define LIST_ROW_H 10
#define LIST_ROWS 5
#define LIST_TEXT_X 4
#define LIST_MAX_CHARS 15                           // (128 - 2 * LIST_TEXT_X) / GLYPH_SIZE
#define LIST_TEXT_PX (LIST_MAX_CHARS * GLYPH_SIZE)  // width of the text window

// ---- Scrolling of a selected name that does not fit (marquee) ----
// hold at the start -> scroll left until the last character is visible -> hold -> start over.
#define SCROLL_HOLD_MS 1200
#define SCROLL_SPEED_PX_PER_S 30
#define MS_PER_SEC 1000U

// 8x8 icons, row-major, MSB = leftmost pixel (format of display_draw_bitmap)
static const uint8_t ICON_PLAY[8] = {0x80, 0xC0, 0xE0, 0xF0, 0xF0, 0xE0, 0xC0, 0x80};
static const uint8_t ICON_PAUSE[8] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
static const uint8_t ICON_STOP[8] = {0x00, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00};
static const uint8_t ICON_MUTE[8] = {0x10, 0x30, 0xF5, 0xF2,
                                     0xF5, 0x30, 0x10, 0x00};  // speaker + x

// 16x16 warning triangle with "!", same format (2 bytes per row)
static const uint8_t ICON_WARNING[32] = {
    0x01, 0x80, 0x02, 0x40, 0x02, 0x40, 0x05, 0xA0, 0x09, 0x90, 0x09, 0x90, 0x11, 0x88, 0x11, 0x88,
    0x21, 0x84, 0x21, 0x84, 0x40, 0x02, 0x41, 0x82, 0x81, 0x81, 0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00};

// ---- NO_SD view layout ----
#define NO_SD_ICON_SIZE 16
#define NO_SD_ICON_X ((DISPLAY_WIDTH - NO_SD_ICON_SIZE) / 2)
#define NO_SD_ICON_Y 10
#define NO_SD_LINE1 "Please insert"
#define NO_SD_LINE2 "SD card"
#define NO_SD_TEXT_Y 34
#define NO_SD_LINE_GAP 2

// -----------------------------------------------------------------
// Formatting helpers
// -----------------------------------------------------------------
// Copies the file name without its ".wav"/".mp3" extension into out
// (NUL-terminated, cut to out_size - 1 characters).
static void format_track_name(const char* name, char* out, size_t out_size) {
  size_t len = strlen(name);
  if (audio_format_from_name(name) != AUDIO_FORMAT_UNKNOWN) {
    len -= 4;  // both extensions are 4 characters (".wav", ".mp3")
  }
  if (len > out_size - 1) {
    len = out_size - 1;
  }
  memcpy(out, name, len);
  out[len] = '\0';
}

// The full name (no extension) of the selected track, as it is shown in its row. In debug mode
// that is the file that is really played.
static void selected_label(const player_state_t* state, char* out, size_t out_size) {
  const char* name = sd_card_get_track_name(state->track_index);
#if CONFIG_AUDIO_DEBUG_MODE
  name = CONFIG_AUDIO_DEBUG_FILENAME;
#endif
  format_track_name(name != NULL ? name : "?", out, out_size);
}

// How many pixels the text is shifted to the left @p anim_ms after the scrolling started.
// @p overflow_px is how much wider the text is than its window (> 0).
static int scroll_offset_px(int overflow_px, uint32_t anim_ms) {
  uint32_t travel_ms = ((uint32_t)overflow_px * MS_PER_SEC) / SCROLL_SPEED_PX_PER_S;
  uint32_t cycle_ms = SCROLL_HOLD_MS + travel_ms + SCROLL_HOLD_MS;
  uint32_t t = anim_ms % cycle_ms;
  if (t < SCROLL_HOLD_MS) {
    return 0;
  }
  if (t >= SCROLL_HOLD_MS + travel_ms) {
    return overflow_px;
  }
  return (int)(((t - SCROLL_HOLD_MS) * SCROLL_SPEED_PX_PER_S) / MS_PER_SEC);
}

static void format_time(char* out, size_t out_size, const player_state_t* state) {
  unsigned elapsed = state->elapsed_sec > MAX_TIME_SEC ? MAX_TIME_SEC : state->elapsed_sec;
  if (state->duration_sec == 0) {
    snprintf(out, out_size, "%02u:%02u/--:--", elapsed / 60, elapsed % 60);
    return;
  }
  unsigned total = state->duration_sec > MAX_TIME_SEC ? MAX_TIME_SEC : state->duration_sec;
  snprintf(out, out_size, "%02u:%02u/%02u:%02u", elapsed / 60, elapsed % 60, total / 60,
           total % 60);
}

// -----------------------------------------------------------------
// Player view parts
// -----------------------------------------------------------------
// First row: time, play/pause/stop icon, debug marker, volume bar (mute icon
// while muted), separator.
static void render_header(const player_state_t* state) {
  char time[TIME_BUF_SIZE];
  format_time(time, sizeof(time), state);
  display_draw_text(HEADER_TIME_X, HEADER_Y, time, HEADER_TIME_CHARS);

  const uint8_t* icon = ICON_STOP;
  if (state->playback == PLAYBACK_PLAYING) {
    icon = ICON_PLAY;
  } else if (state->playback == PLAYBACK_PAUSED) {
    icon = ICON_PAUSE;
  }
  display_draw_bitmap(HEADER_ICON_X, HEADER_Y, icon, ICON_SIZE, ICON_SIZE);

#if CONFIG_AUDIO_DEBUG_MODE
  display_draw_text(HEADER_DEBUG_X, HEADER_Y, "D", 1);
#endif

  if (state->muted) {
    // An icon instead of the bar: an empty bar would look like volume 0.
    display_draw_bitmap(HEADER_BAR_X, HEADER_Y, ICON_MUTE, ICON_SIZE, ICON_SIZE);
  } else {
    display_draw_bar(HEADER_BAR_X, HEADER_Y, HEADER_BAR_W, HEADER_BAR_H, state->volume);
  }
  display_draw_hline(SEPARATOR_Y);
}

// Track list below the header: up to LIST_ROWS rows, the window follows
// state->track_index, the current track is in a rounded frame.
static void render_list(const player_state_t* state, uint32_t anim_ms) {
  if (state->track_count <= 0) {
    display_draw_text_centered(LIST_Y + ((LIST_ROWS / 2) * LIST_ROW_H), "No tracks");
    return;
  }

  int top = state->track_index - (LIST_ROWS / 2);
  int max_top = state->track_count - LIST_ROWS;
  if (top > max_top) {
    top = max_top;
  }
  if (top < 0) {
    top = 0;
  }

  for (int row = 0; row < LIST_ROWS; row++) {
    int index = top + row;
    if (index >= state->track_count) {
      break;
    }
    int y = LIST_Y + (row * LIST_ROW_H);
    char label[SD_MAX_NAME];
    if (index == state->track_index) {
      // The selected row shows the whole name, scrolling it if it does not fit the window.
      display_draw_rounded_frame(0, y, DISPLAY_WIDTH, LIST_ROW_H);
      selected_label(state, label, sizeof(label));
      int text_px = (int)strlen(label) * GLYPH_SIZE;
      int offset = text_px > LIST_TEXT_PX ? scroll_offset_px(text_px - LIST_TEXT_PX, anim_ms) : 0;
      display_draw_text_clipped(LIST_TEXT_X - offset, y + 1, label, LIST_TEXT_X, LIST_TEXT_PX);
      continue;
    }
    // The other rows are cut to the window.
    const char* name = sd_card_get_track_name(index);
    format_track_name(name != NULL ? name : "?", label, LIST_MAX_CHARS + 1);
    display_draw_text(LIST_TEXT_X, y + 1, label, LIST_MAX_CHARS);
  }
}

// -----------------------------------------------------------------
// Views (full-screen layouts)
// -----------------------------------------------------------------
static void view_player(const player_state_t* state, uint32_t anim_ms) {
  render_header(state);
  render_list(state, anim_ms);
}

static void view_no_sd(const player_state_t* state, uint32_t anim_ms) {
  (void)state;
  (void)anim_ms;
  display_draw_bitmap(NO_SD_ICON_X, NO_SD_ICON_Y, ICON_WARNING, NO_SD_ICON_SIZE, NO_SD_ICON_SIZE);
  display_draw_text_centered(NO_SD_TEXT_Y, NO_SD_LINE1);
  display_draw_text_centered(NO_SD_TEXT_Y + GLYPH_SIZE + NO_SD_LINE_GAP, NO_SD_LINE2);
}

// Indexed by display_view_t.
static void (*const s_views[DISPLAY_VIEW_COUNT])(const player_state_t*, uint32_t) = {
    [DISPLAY_VIEW_PLAYER] = view_player,
    [DISPLAY_VIEW_NO_SD] = view_no_sd,
};

void display_render_view(display_view_t view, const player_state_t* state, uint32_t anim_ms) {
  if ((int)view < 0 || view >= DISPLAY_VIEW_COUNT || s_views[view] == NULL) {
    ESP_LOGW(TAG, "Unknown view %d", (int)view);
    return;
  }
  s_views[view](state, anim_ms);
}

bool display_view_is_animated(display_view_t view, const player_state_t* state) {
  if (view != DISPLAY_VIEW_PLAYER || state->track_count <= 0) {
    return false;
  }
  char label[SD_MAX_NAME];
  selected_label(state, label, sizeof(label));
  return strlen(label) > LIST_MAX_CHARS;
}
