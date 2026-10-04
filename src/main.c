#include "audio_task.h"
#include "ble_task.h"
#include "display_task.h"
#include "esp_log.h"
#include "esp_log_level.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_task.h"
#include "player_state.h"
#include "player_types.h"
#include "sd_card.h"
#include "vs1053.h"

static const char* TAG = "main";

#define STARTUP_DELAY_MS 2000  // lets the serial monitor attach before the first logs
#define CMD_QUEUE_LENGTH 10

#if CONFIG_AUDIO_DEBUG_VS1053_LOG_DEBUG
#define VS1053_LOG_LEVEL ESP_LOG_DEBUG
#elif CONFIG_AUDIO_DEBUG_VS1053_LOG_INFO
#define VS1053_LOG_LEVEL ESP_LOG_INFO
#else
#define VS1053_LOG_LEVEL ESP_LOG_WARN
#endif

#if CONFIG_AUDIO_DEBUG_SPI_MASTER_LOG_DEBUG
#define SPI_MASTER_LOG_LEVEL ESP_LOG_DEBUG
#elif CONFIG_AUDIO_DEBUG_SPI_MASTER_LOG_WARN
#define SPI_MASTER_LOG_LEVEL ESP_LOG_WARN
#else
#define SPI_MASTER_LOG_LEVEL ESP_LOG_INFO  // drops "deviceN release bus" debug spam
#endif

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY_MS));
  esp_log_level_set("ssd1306", ESP_LOG_WARN);
  esp_log_level_set("spi_master", SPI_MASTER_LOG_LEVEL);
  esp_log_level_set("VS1053", VS1053_LOG_LEVEL);

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
  QueueHandle_t cmd_queue = xQueueCreate(CMD_QUEUE_LENGTH, sizeof(player_cmd_t));
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

  // Buttons only send commands to cmd_queue; the player works without them.
  if (input_task_init(cmd_queue) != ESP_OK) {
    ESP_LOGE(TAG, "input_task_init failed, continuing without buttons");
  }

  // BLE only sends commands to cmd_queue; the player works without it.
  if (ble_task_init(cmd_queue) != ESP_OK) {
    ESP_LOGE(TAG, "ble_task_init failed, continuing without BLE");
  }
}
