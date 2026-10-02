#include "audio_task.h"
#include "display_task.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "player_state.h"
#include "player_types.h"
#include "sd_card.h"
#include "vs1053.h"

static const char* TAG = "main";

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_log_level_set("ssd1306", ESP_LOG_WARN);

  ESP_LOGI(TAG, "MP3 player starting up");

  // Keep the VS1053 deselected until vs1053_init() runs — its XCS/XDCS pins
  // would otherwise float (default GPIO input state). It has its own SPI
  // bus (SPI3), so this no longer affects the SD card (SPI2).
  vs1053_deselect_early();

  if (player_state_init() != ESP_OK) {
    ESP_LOGE(TAG, "player_state_init failed");
    return;
  }

  // Shared resources for all tasks touching the SD card / VS1053:
  // created here, in main.c, as the single owner, and passed in as
  // parameters to sd_card_scan_tracks() / audio_task_init() / vs1053_init().
  QueueHandle_t cmd_queue = xQueueCreate(10, sizeof(player_cmd_t));
  SemaphoreHandle_t spi_mutex = xSemaphoreCreateMutex();

  if (cmd_queue == NULL || spi_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create cmd_queue/spi_mutex");
    return;
  }

  // Initializes the SD card's SPI bus (SPI2) and mounts the card.
  spi_host_device_t spi_host;
  // A missing card is not fatal: sd_card_monitor_init() keeps retrying and
  // the display shows a "please insert SD card" view meanwhile.
  if (sd_card_init(&spi_host) != ESP_OK) {
    ESP_LOGW(TAG, "sd_card_init failed (no card?), will keep retrying");
  }

  // VS1053 initializes its own, separate SPI bus (SPI3) inside vs1053_init.
  if (vs1053_init(spi_mutex) != ESP_OK) {
    ESP_LOGE(TAG, "vs1053_init failed");
    return;
  }

  // Scans the card now (if present) and then watches for insert / removal.
  if (sd_card_monitor_init(spi_mutex) != ESP_OK) {
    ESP_LOGE(TAG, "sd_card_monitor_init failed");
    return;
  }

  if (audio_task_init(cmd_queue, spi_mutex) != ESP_OK) {
    ESP_LOGE(TAG, "audio_task_init failed");
    return;
  }

  // The player works without a display, so a failure is only logged.
  if (display_task_init() != ESP_OK) {
    ESP_LOGE(TAG, "display_task_init failed, continuing without display");
  }

  // --- example: sending a command from outside
  //     (in the real project — from vInputTask / vBLETask,
  //     which will also receive cmd_queue as a parameter at init) ---
  // player_cmd_t cmd = { .type = CMD_PLAY_PAUSE };
  // xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
}
