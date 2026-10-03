#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Initializes the BLE control module.
 *
 * The ESP32-S3 acts as a GAP peripheral and a GATT server (NimBLE). It
 * advertises as "MP3 Player" and exposes one custom service (128-bit UUID)
 * with one characteristic that accepts WRITE and WRITE_NO_RSP. No pairing,
 * bonding or encryption is used: any central may connect and write.
 *
 * A write must carry exactly one ASCII byte:
 *   'P' play/pause, 'N' next, 'B' previous, '+' volume up, '-' volume down.
 * Each valid byte is turned into a player_cmd_t and sent to cmd_queue with a
 * zero timeout (a full queue drops the command and logs a warning). Anything
 * else is rejected with an ATT error (visible only for WRITE with response).
 *
 * Besides the NimBLE host stack the module creates one FreeRTOS task,
 * "ble_task", which runs nimble_port_run() — the NimBLE host task, created
 * here instead of by nimble_port_freertos_init() so that name, stack and
 * priority are ours. GATT callbacks run in that task and never block. Advertising
 * restarts automatically after a disconnect. The module also initializes NVS
 * (needed by the Bluetooth controller/PHY) and never touches player_state or
 * the SPI mutex.
 *
 * @param cmd_queue  Shared command Queue (created in main.c, read by
 *                   audio_task)
 *
 * @return ESP_OK on success, an error if cmd_queue is NULL or NVS, the BLE
 *         stack, the GATT table or the task could not be set up
 */
esp_err_t ble_task_init(QueueHandle_t cmd_queue);
