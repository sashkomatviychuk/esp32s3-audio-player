#include "display.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "font_latin_8x8.h"
#include "ssd1306.h"

static const char* TAG = "display";

// ---- I2C wiring (matches the pin map in README.md) ----
#define DISPLAY_I2C_PORT I2C_NUM_0
#define DISPLAY_PIN_SDA 17
#define DISPLAY_PIN_SCL 18
#define DISPLAY_I2C_SPEED_HZ 400000  // a 1KB framebuffer flush is too slow at 100kHz

#define FONT_LAST_CHAR 0x7E  // the font covers ASCII up to '~'
#define FRAME_RADIUS_CUT 1

static ssd1306_handle_t s_display = NULL;

// -----------------------------------------------------------------
// Init / flush
// -----------------------------------------------------------------
esp_err_t display_init(void) {
  i2c_master_bus_config_t bus_config = {
      .i2c_port = DISPLAY_I2C_PORT,
      .sda_io_num = DISPLAY_PIN_SDA,
      .scl_io_num = DISPLAY_PIN_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      // Harmless if the module has its own 4.7k pull-ups, required if not
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus = NULL;
  esp_err_t ret = i2c_new_master_bus(&bus_config, &bus);
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

// -----------------------------------------------------------------
// Drawing primitives (framebuffer only)
// -----------------------------------------------------------------
// Draws one glyph at (x, y), only the columns inside [clip_lo, clip_hi). The window is also
// limited to the panel, so every x handed to ssd1306_set_pixel() is in 0..DISPLAY_WIDTH - 1.
static void draw_glyph(int x, int y, unsigned char ch, int clip_lo, int clip_hi) {
  if (ch > FONT_LAST_CHAR) {
    ch = '?';  // the font is Latin-only: show a placeholder for UTF-8 bytes
  }
  // font_latin_8x8_tr is column-major: byte = column, bit 0 = top row
  for (int col = 0; col < GLYPH_SIZE; col++) {
    int px = x + col;
    if (px < clip_lo || px >= clip_hi) {
      continue;
    }
    uint8_t bits = font_latin_8x8_tr[ch][col];
    for (int row = 0; row < GLYPH_SIZE; row++) {
      if (bits & (1 << row)) {
        ssd1306_set_pixel(s_display, (uint8_t)px, (uint8_t)(y + row), false);
      }
    }
  }
}

void display_draw_text(int x, int y, const char* text, int max_chars) {
  if (s_display == NULL) {
    return;
  }
  for (int i = 0; i < max_chars && text[i] != '\0'; i++) {
    draw_glyph(x + (i * GLYPH_SIZE), y, (unsigned char)text[i], 0, DISPLAY_WIDTH);
  }
}

void display_draw_text_clipped(int x, int y, const char* text, int clip_x, int clip_w) {
  if (s_display == NULL) {
    return;
  }
  int clip_lo = clip_x < 0 ? 0 : clip_x;
  int clip_hi = clip_x + clip_w > DISPLAY_WIDTH ? DISPLAY_WIDTH : clip_x + clip_w;
  for (int i = 0; text[i] != '\0'; i++) {
    int gx = x + (i * GLYPH_SIZE);
    if (gx + GLYPH_SIZE <= clip_lo) {
      continue;  // not in the window yet
    }
    if (gx >= clip_hi) {
      break;  // past the window: the rest is too
    }
    draw_glyph(gx, y, (unsigned char)text[i], clip_lo, clip_hi);
  }
}

// Text is centered using 8 px per glyph.
void display_draw_text_centered(int y, const char* text) {
  int len = (int)strlen(text);
  display_draw_text((DISPLAY_WIDTH - (len * GLYPH_SIZE)) / 2, y, text, len);
}

void display_draw_rounded_frame(int x, int y, int w, int h) {
  if (s_display == NULL) {
    return;
  }
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

void display_draw_bar(int x, int y, int w, int h, int percent) {
  if (s_display == NULL) {
    return;
  }
  int x1 = x + w - 1;
  int y1 = y + h - 1;
  ssd1306_set_line(s_display, (uint8_t)x, (uint8_t)y, (uint8_t)x1, (uint8_t)y, false);
  ssd1306_set_line(s_display, (uint8_t)x, (uint8_t)y1, (uint8_t)x1, (uint8_t)y1, false);
  ssd1306_set_line(s_display, (uint8_t)x, (uint8_t)y, (uint8_t)x, (uint8_t)y1, false);
  ssd1306_set_line(s_display, (uint8_t)x1, (uint8_t)y, (uint8_t)x1, (uint8_t)y1, false);

  int inner_w = w - 4;
  int fill = inner_w * percent / 100;
  for (int i = 0; i < fill; i++) {
    ssd1306_set_line(s_display, (uint8_t)(x + 2 + i), (uint8_t)(y + 2), (uint8_t)(x + 2 + i),
                     (uint8_t)(y1 - 2), false);
  }
}

void display_draw_hline(int y) {
  if (s_display == NULL) {
    return;
  }
  ssd1306_set_line(s_display, 0, (uint8_t)y, DISPLAY_WIDTH - 1, (uint8_t)y, false);
}

void display_draw_bitmap(int x, int y, const uint8_t* bitmap, int w, int h) {
  if (s_display == NULL) {
    return;
  }
  ssd1306_set_bitmap(s_display, (uint8_t)x, (uint8_t)y, bitmap, (uint8_t)w, (uint8_t)h, false);
}
