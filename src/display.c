#include "display.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "audio_task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "font_latin_8x8.h"
#include "sd_card.h"
#include "ssd1306.h"

static const char* TAG = "display";

// ---- I2C wiring (matches the pin map in README.md) ----
#define DISPLAY_I2C_PORT I2C_NUM_0
#define DISPLAY_PIN_SDA 17
#define DISPLAY_PIN_SCL 18
#define DISPLAY_I2C_SPEED_HZ 400000  // a 1KB framebuffer flush is too slow at 100kHz

// ---- Layout (128x64) ----
#define GLYPH_SIZE 8
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
#define LIST_MAX_CHARS 15  // (128 - 2 * LIST_TEXT_X) / GLYPH_SIZE
#define FRAME_RADIUS_CUT 1

static ssd1306_handle_t s_display = NULL;

// 8x8 icons, row-major, MSB = leftmost pixel (format of ssd1306_set_bitmap)
static const uint8_t ICON_PLAY[8] = {0x80, 0xC0, 0xE0, 0xF0, 0xF0, 0xE0, 0xC0, 0x80};
static const uint8_t ICON_PAUSE[8] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
static const uint8_t ICON_STOP[8] = {0x00, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00};

// -----------------------------------------------------------------
// Drawing primitives (framebuffer only)
// -----------------------------------------------------------------
static void draw_text(int x, int y, const char* text, int maxChars) {
  for (int i = 0; i < maxChars && text[i] != '\0'; i++) {
    unsigned char ch = (unsigned char)text[i];
    if (ch > 0x7E) {
      ch = '?';  // the font is Latin-only: show a placeholder for UTF-8 bytes
    }
    // font_latin_8x8_tr is column-major: byte = column, bit 0 = top row
    for (int col = 0; col < GLYPH_SIZE; col++) {
      uint8_t bits = font_latin_8x8_tr[ch][col];
      for (int row = 0; row < GLYPH_SIZE; row++) {
        if (bits & (1 << row)) {
          ssd1306_set_pixel(s_display, (uint8_t)(x + i * GLYPH_SIZE + col), (uint8_t)(y + row),
                            false);
        }
      }
    }
  }
}

static void draw_rounded_frame(int x, int y, int w, int h) {
  int r = FRAME_RADIUS_CUT;
  int x1 = x + w - 1;
  int y1 = y + h - 1;
  ssd1306_set_line(s_display, (uint8_t)(x + r + 1), (uint8_t)y, (uint8_t)(x1 - r - 1), (uint8_t)y,
                   false);
  ssd1306_set_line(s_display, (uint8_t)(x + r + 1), (uint8_t)y1, (uint8_t)(x1 - r - 1), (uint8_t)y1,
                   false);
  ssd1306_set_line(s_display, (uint8_t)x, (uint8_t)(y + r + 1), (uint8_t)x, (uint8_t)(y1 - r - 1),
                   false);
  ssd1306_set_line(s_display, (uint8_t)x1, (uint8_t)(y + r + 1), (uint8_t)x1, (uint8_t)(y1 - r - 1),
                   false);
  // Rounded corners: one diagonal pixel each
  ssd1306_set_pixel(s_display, (uint8_t)(x + 1), (uint8_t)(y + 1), false);
  ssd1306_set_pixel(s_display, (uint8_t)(x1 - 1), (uint8_t)(y + 1), false);
  ssd1306_set_pixel(s_display, (uint8_t)(x + 1), (uint8_t)(y1 - 1), false);
  ssd1306_set_pixel(s_display, (uint8_t)(x1 - 1), (uint8_t)(y1 - 1), false);
}

static void draw_bar(int x, int y, int w, int h, int percent) {
  int x1 = x + w - 1;
  int y1 = y + h - 1;
  ssd1306_set_line(s_display, (uint8_t)x, (uint8_t)y, (uint8_t)x1, (uint8_t)y, false);
  ssd1306_set_line(s_display, (uint8_t)x, (uint8_t)y1, (uint8_t)x1, (uint8_t)y1, false);
  ssd1306_set_line(s_display, (uint8_t)x, (uint8_t)y, (uint8_t)x, (uint8_t)y1, false);
  ssd1306_set_line(s_display, (uint8_t)x1, (uint8_t)y, (uint8_t)x1, (uint8_t)y1, false);

  int innerW = w - 4;
  int fill = innerW * percent / 100;
  for (int i = 0; i < fill; i++) {
    ssd1306_set_line(s_display, (uint8_t)(x + 2 + i), (uint8_t)(y + 2), (uint8_t)(x + 2 + i),
                     (uint8_t)(y1 - 2), false);
  }
}

// Copies the file name without its ".mp3" extension into out (NUL-terminated,
// cut to LIST_MAX_CHARS).
static void format_track_name(const char* name, char* out, size_t outSize) {
  size_t len = strlen(name);
  if (len > 4 && strcasecmp(name + len - 4, ".mp3") == 0) {
    len -= 4;
  }
  if (len > outSize - 1) {
    len = outSize - 1;
  }
  memcpy(out, name, len);
  out[len] = '\0';
}

static void format_time(char* out, size_t outSize, const player_state_t* state) {
  unsigned elapsed = state->elapsed_sec > 99 * 60 + 59 ? 99 * 60 + 59 : state->elapsed_sec;
  if (state->duration_sec == 0) {
    snprintf(out, outSize, "%02u:%02u/--:--", elapsed / 60, elapsed % 60);
    return;
  }
  unsigned total = state->duration_sec > 99 * 60 + 59 ? 99 * 60 + 59 : state->duration_sec;
  snprintf(out, outSize, "%02u:%02u/%02u:%02u", elapsed / 60, elapsed % 60, total / 60, total % 60);
}

// -----------------------------------------------------------------
// Public API
// -----------------------------------------------------------------
esp_err_t display_init(void) {
  i2c_master_bus_config_t busConfig = {
      .i2c_port = DISPLAY_I2C_PORT,
      .sda_io_num = DISPLAY_PIN_SDA,
      .scl_io_num = DISPLAY_PIN_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      // Harmless if the module has its own 4.7k pull-ups, required if not
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus = NULL;
  esp_err_t ret = i2c_new_master_bus(&busConfig, &bus);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ssd1306_config_t config = I2C_SSD1306_128x64_CONFIG_DEFAULT;
  config.i2c_clock_speed = DISPLAY_I2C_SPEED_HZ;
  ret = ssd1306_init(bus, &config, &s_display);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "ssd1306_init failed: %s (display connected? SDA=%d SCL=%d)",
             esp_err_to_name(ret), DISPLAY_PIN_SDA, DISPLAY_PIN_SCL);
    i2c_del_master_bus(bus);
    s_display = NULL;
    return ret;
  }

  display_clear();
  display_flush();
  ESP_LOGI(TAG, "SSD1306 128x64 initialized (SDA=%d SCL=%d, %d Hz)", DISPLAY_PIN_SDA,
           DISPLAY_PIN_SCL, DISPLAY_I2C_SPEED_HZ);
  return ESP_OK;
}

void display_clear(void) {
  if (s_display == NULL) {
    return;
  }
  // ssd1306_clear_display() writes straight to the panel and does not touch the
  // framebuffer, so the buffer is zeroed directly.
  for (uint8_t page = 0; page < s_display->pages; page++) {
    memset(s_display->page[page].segment, 0, SSD1306_PAGE_SEGMENT_SIZE);
  }
}

esp_err_t display_flush(void) {
  if (s_display == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  return ssd1306_display_pages(s_display);
}

void display_render_header(const player_state_t* state) {
  if (s_display == NULL) {
    return;
  }

  char time[16];
  format_time(time, sizeof(time), state);
  draw_text(HEADER_TIME_X, HEADER_Y, time, 11);

  const uint8_t* icon = ICON_STOP;
  if (state->playback == PLAYBACK_PLAYING) {
    icon = ICON_PLAY;
  } else if (state->playback == PLAYBACK_PAUSED) {
    icon = ICON_PAUSE;
  }
  ssd1306_set_bitmap(s_display, HEADER_ICON_X, HEADER_Y, icon, 8, 8, false);

#if AUDIO_DEBUG_MODE
  draw_text(HEADER_DEBUG_X, HEADER_Y, "D", 1);
#endif

  draw_bar(HEADER_BAR_X, HEADER_Y, HEADER_BAR_W, HEADER_BAR_H, state->volume);
  ssd1306_set_line(s_display, 0, SEPARATOR_Y, 127, SEPARATOR_Y, false);
}

void display_render_list(const player_state_t* state) {
  if (s_display == NULL) {
    return;
  }

  if (state->track_count <= 0) {
    draw_text(24, LIST_Y + 2 * LIST_ROW_H, "No tracks", 9);
    return;
  }

  int top = state->track_index - LIST_ROWS / 2;
  int maxTop = state->track_count - LIST_ROWS;
  if (top > maxTop) {
    top = maxTop;
  }
  if (top < 0) {
    top = 0;
  }

  for (int row = 0; row < LIST_ROWS; row++) {
    int index = top + row;
    if (index >= state->track_count) {
      break;
    }
    int y = LIST_Y + row * LIST_ROW_H;
    const char* name = sd_card_get_track_name(index);
    if (index == state->track_index) {
#if AUDIO_DEBUG_MODE
      name = AUDIO_DEBUG_FILENAME;  // show what is really playing
#endif
      draw_rounded_frame(0, y, 128, LIST_ROW_H);
    }
    char label[LIST_MAX_CHARS + 1];
    format_track_name(name != NULL ? name : "?", label, sizeof(label));
    draw_text(LIST_TEXT_X, y + 1, label, LIST_MAX_CHARS);
  }
}
