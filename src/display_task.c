#include "display_task.h"

#include "audio_task.h"
#include "display.h"
#include "display_views.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player_state.h"

static const char* TAG = "display_task";

#define DISPLAY_TASK_STACK_SIZE 4096
#define DISPLAY_TASK_PRIORITY 3  // below audio_task (5)
#define DISPLAY_PERIOD_MS 250

static bool state_changed(const player_state_t* curr_state, const player_state_t* last_state) {
  return curr_state->playback != last_state->playback || curr_state->volume != last_state->volume ||
         curr_state->track_index != last_state->track_index ||
         curr_state->track_count != last_state->track_count ||
         curr_state->elapsed_sec != last_state->elapsed_sec ||
         curr_state->duration_sec != last_state->duration_sec ||
         curr_state->sd_present != last_state->sd_present;
}

// The single place with routing rules: which view the current state calls for.
static display_view_t select_view(const player_state_t* state) {
  if (!state->sd_present) {
    return DISPLAY_VIEW_NO_SD;
  }
  return DISPLAY_VIEW_PLAYER;
}

static void display_task(void* arg) {
  player_state_t last = {0};
  display_view_t last_view = DISPLAY_VIEW_PLAYER;
  bool first_frame = true;
  TickType_t last_wake = xTaskGetTickCount();

  while (1) {
    player_state_t curr_player_state;
    player_state_get(&curr_player_state);
    display_view_t view = select_view(&curr_player_state);

    if (first_frame || view != last_view || state_changed(&curr_player_state, &last)) {
      display_clear();
      display_render_view(view, &curr_player_state);
      esp_err_t ret = display_flush();
      if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Display flush failed: %s", esp_err_to_name(ret));
      } else {
        last = curr_player_state;
        last_view = view;
        first_frame = false;
      }
    }

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DISPLAY_PERIOD_MS));
  }
}

esp_err_t display_task_init(void) {
  esp_err_t ret = display_init();
  if (ret != ESP_OK) {
    return ret;
  }

#if AUDIO_DEBUG_MODE
  ESP_LOGW(TAG, "AUDIO_DEBUG_MODE enabled: highlighted row shows %s, 'D' marker in header",
           AUDIO_DEBUG_FILENAME);
#endif

  BaseType_t created = xTaskCreate(display_task, "display_task", DISPLAY_TASK_STACK_SIZE, NULL,
                                   DISPLAY_TASK_PRIORITY, NULL);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create display_task");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "display_task started (%d ms period)", DISPLAY_PERIOD_MS);
  return ESP_OK;
}
