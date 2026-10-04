#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Initializes the button input module.
 *
 * Event-driven, no polling: a GPIO interrupt on every button edge wakes
 * input_task, which starts a one-shot esp_timer for debounce. When the timer
 * fires, the settled level is read and — on a real press — a player_cmd_t is
 * sent to cmd_queue. Buttons flagged `repeat` in the table auto-repeat while
 * held (none at the moment; volume is the encoder's job, see encoder.h).
 *
 * Buttons are active-low with the internal pull-up (see the table in
 * input_task.c). The module never touches player_state or the SPI mutex —
 * it only writes commands to the queue, with a zero timeout (a full queue
 * drops the command and logs a warning).
 *
 * @param cmd_queue  Shared command Queue (created in main.c, read by
 *                   audio_task)
 *
 * @return ESP_OK on success, an error if cmd_queue is NULL or a GPIO,
 *         timer or the task could not be set up
 */
esp_err_t input_task_init(QueueHandle_t cmd_queue);
