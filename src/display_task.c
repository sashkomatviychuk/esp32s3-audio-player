#include "display_task.h"

#include "audio_task.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player_state.h"

static const char* TAG = "display_task";

#define DISPLAY_TASK_STACK_SIZE 4096
#define DISPLAY_TASK_PRIORITY 3  // below audio_task (5)
#define DISPLAY_PERIOD_MS 250

static bool state_changed(const player_state_t* a, const player_state_t* b) {
  return a->playback != b->playback || a->volume != b->volume ||
         a->track_index != b->track_index || a->track_count != b->track_count ||
         a->elapsed_sec != b->elapsed_sec || a->duration_sec != b->duration_sec;
}

static void display_task(void* arg) {
  player_state_t last = {0};
  bool firstFrame = true;
  TickType_t lastWake = xTaskGetTickCount();

  while (1) {
    player_state_t now;
    player_state_get(&now);

    if (firstFrame || state_changed(&now, &last)) {
      display_clear();
      display_render_header(&now);
      display_render_list(&now);
      esp_err_t ret = display_flush();
      if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Display flush failed: %s", esp_err_to_name(ret));
      } else {
        last = now;
        firstFrame = false;
      }
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DISPLAY_PERIOD_MS));
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
