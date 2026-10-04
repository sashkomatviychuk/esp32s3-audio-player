#include "player_state.h"

#include "audio_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "player_state";

#define INITIAL_VOLUME 70
#define VOLUME_MIN 0
#define VOLUME_MAX 100

// Guards s_state only. Held just long enough to copy a few fields — never
// held across SPI / VS1053 / SD calls, so it cannot deadlock with spi_mutex.
static SemaphoreHandle_t s_state_mutex = NULL;
static player_state_t s_state = {
    .playback = PLAYBACK_STOPPED, .volume = INITIAL_VOLUME, .track_index = 0, .track_count = 0,
    .elapsed_sec = 0, .duration_sec = 0};

esp_err_t player_state_init(void) {
  if (s_state_mutex != NULL) {
    ESP_LOGW(TAG, "player_state_init called twice, ignored");
    return ESP_OK;
  }

  s_state_mutex = xSemaphoreCreateMutex();
  if (s_state_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create state mutex");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Player state initialized (volume %d)", INITIAL_VOLUME);
  return ESP_OK;
}

void player_state_get(player_state_t* out) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  *out = s_state;
  xSemaphoreGive(s_state_mutex);
}

playback_state_t player_state_get_playback(void) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  playback_state_t playback = s_state.playback;
  xSemaphoreGive(s_state_mutex);
  return playback;
}

uint8_t player_state_get_volume(void) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  uint8_t volume = s_state.volume;
  xSemaphoreGive(s_state_mutex);
  return volume;
}

void player_state_set_playback(playback_state_t playback) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  s_state.playback = playback;
  xSemaphoreGive(s_state_mutex);
  ESP_LOGI(TAG, "Playback state -> %d", playback);
}

uint8_t player_state_change_volume(int delta) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  int volume = s_state.volume + delta;
  if (volume > VOLUME_MAX) {
    volume = VOLUME_MAX;
  } else if (volume < VOLUME_MIN) {
    volume = VOLUME_MIN;
  }
  uint8_t old_volume = s_state.volume;
  s_state.volume = (uint8_t)volume;
  s_state.muted = false;
  xSemaphoreGive(s_state_mutex);

  if (volume == old_volume) {
    ESP_LOGW(TAG, "Volume already at limit (%d)", volume);
  }
  return (uint8_t)volume;
}

bool player_state_toggle_mute(void) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  s_state.muted = !s_state.muted;
  bool muted = s_state.muted;
  xSemaphoreGive(s_state_mutex);

  ESP_LOGI(TAG, "Muted -> %d", muted);
  return muted;
}

esp_err_t player_state_select_track(int index) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  int count = s_state.track_count;
  bool valid = index >= 0 && index < count;
  if (valid) {
    s_state.track_index = index;
    s_state.elapsed_sec = 0;
    s_state.duration_sec = 0;
  }
  xSemaphoreGive(s_state_mutex);

  if (!valid) {
    ESP_LOGW(TAG, "Track index %d out of range (count %d), ignored", index, count);
    return ESP_ERR_INVALID_ARG;
  }
  ESP_LOGI(TAG, "Track index -> %d", index);
  return ESP_OK;
}

bool player_state_next_track(void) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  bool moved = s_state.track_index + 1 < s_state.track_count;
  if (moved) {
    s_state.track_index++;
    s_state.elapsed_sec = 0;
    s_state.duration_sec = 0;
  }
  int index = s_state.track_index;
  xSemaphoreGive(s_state_mutex);

  if (moved) {
    ESP_LOGI(TAG, "Next track -> %d", index);
  } else {
    ESP_LOGW(TAG, "Already at last track (%d), next ignored", index);
  }
  return moved;
}

bool player_state_prev_track(void) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  bool moved = s_state.track_index > 0;
  if (moved) {
    s_state.track_index--;
    s_state.elapsed_sec = 0;
    s_state.duration_sec = 0;
  }
  int index = s_state.track_index;
  xSemaphoreGive(s_state_mutex);

  if (moved) {
    ESP_LOGI(TAG, "Prev track -> %d", index);
  } else {
    ESP_LOGW(TAG, "Already at first track (%d), prev ignored", index);
  }
  return moved;
}

void player_state_set_track_count(int count) {
#if CONFIG_AUDIO_DEBUG_MODE
  // Only CONFIG_AUDIO_DEBUG_FILENAME is ever played, so expose a single track:
  // Next/Prev/Select then have nothing to switch to.
  if (count > 1) {
    count = 1;
  }
#endif
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  s_state.track_count = count;
  if (s_state.track_index >= count) {
    // Card was swapped / removed: the old index no longer points at a track
    s_state.track_index = 0;
    s_state.elapsed_sec = 0;
    s_state.duration_sec = 0;
  }
  xSemaphoreGive(s_state_mutex);
  ESP_LOGI(TAG, "Track count -> %d", count);
}

void player_state_set_sd_present(bool present) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  s_state.sd_present = present;
  xSemaphoreGive(s_state_mutex);
  ESP_LOGI(TAG, "SD present -> %d", present);
}

void player_state_set_progress(uint32_t elapsed_sec, uint32_t duration_sec) {
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  s_state.elapsed_sec = elapsed_sec;
  s_state.duration_sec = duration_sec;
  xSemaphoreGive(s_state_mutex);
}
