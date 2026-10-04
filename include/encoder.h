#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Initializes the EC11 rotary encoder (volume knob).
 *
 * Event-driven, no task and no polling: the quadrature signal (A/B) is decoded
 * by the PCNT peripheral in hardware. Every full detent (EDGES_PER_DETENT
 * edges, see encoder.c) raises a PCNT watch-point interrupt, which sends one
 * CMD_VOLUME_UP (clockwise) or CMD_VOLUME_DOWN (counter-clockwise) to
 * cmd_queue. The step size per command is audio_task's business.
 *
 * A and B use the internal pull-ups (pins are listed in encoder.c). The push
 * switch (SW) is wired but not handled yet.
 *
 * Like input_task, the module never touches player_state or the SPI mutex —
 * it only writes commands to the queue, with a zero timeout (a full queue
 * silently drops the command: logging is not allowed in the ISR).
 *
 * @param cmd_queue  Shared command Queue (created in main.c, read by
 *                   audio_task)
 *
 * @return ESP_OK on success, an error if cmd_queue is NULL or the PCNT unit,
 *         its channels or the GPIOs could not be set up
 */
esp_err_t encoder_init(QueueHandle_t cmd_queue);
