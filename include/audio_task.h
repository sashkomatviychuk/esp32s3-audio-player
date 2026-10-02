#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/**
 * @brief Initializes the audio module: stores the passed-in handles
 *        and creates vAudioTask.
 *
 * @param cmd_queue   Shared command Queue (created in main.c,
 *                    written to by vInputTask and vBLETask)
 * @param spi_mutex   Mutex protecting the shared SPI bus (VS1053 + SD card)
 * @param mount_point SD card mount path (e.g. "/sdcard")
 *
 * @return ESP_OK on success, an error if the handles are NULL or the
 *         task could not be created
 */
esp_err_t audio_task_init(QueueHandle_t cmd_queue, SemaphoreHandle_t spi_mutex,
                          const char* mount_point);
