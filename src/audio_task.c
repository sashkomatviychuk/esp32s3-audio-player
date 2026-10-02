#include "audio_task.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"
#include "player_types.h"
#include "vs1053.h"

static const char* TAG = "audio_task";

#define CHUNK_SIZE 32  // VS1053: 32-byte chunks over SPI
#define AUDIO_TASK_STACK_SIZE 4096
#define AUDIO_TASK_PRIORITY 5
#define MAX_CONSECUTIVE_CODEC_ERRORS 10  // stop playback if VS1053 stays unresponsive

// -----------------------------------------------------------------
// Switch between "single-file debug" and "real project logic".
//
// AUDIO_DEBUG_MODE 1 — audio_task always plays AUDIO_DEBUG_FILENAME,
//   ignoring any track list. This is the mode used for testing right
//   now: set AUDIO_DEBUG_FILENAME to "demo_audio.mp3" or
//   "demo_audio_2.mp3", rebuild — and the chosen file plays.
//
// AUDIO_DEBUG_MODE 0 — switches to get_current_track_path(), which
//   is meant to take the path from the real track-selection logic
//   (vSDTask + selection in SCREEN_LIST, architecture.md). This
//   branch is currently a STUB — don't switch to 0 until the track
//   list is implemented.
//
// One macro — one switch point, the rest of audio_task's code stays
// the same regardless of the mode.
// -----------------------------------------------------------------
#define AUDIO_DEBUG_MODE 1
#define AUDIO_DEBUG_FILENAME "demo_audio.mp3"

typedef enum { PLAYBACK_STOPPED = 0, PLAYBACK_PLAYING, PLAYBACK_PAUSED } playback_state_t;

// -----------------------------------------------------------------
// Module-private state (NOT extern, accessible only within this file)
// -----------------------------------------------------------------
static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_spi_mutex = NULL;
static char s_mount_point[32] = {0};
static volatile playback_state_t s_state = PLAYBACK_STOPPED;
static uint8_t s_volume = 70;  // 0..100, starting value

// -----------------------------------------------------------------
// Thin wrapper over the VS1053 driver. Called EXCLUSIVELY while
// s_spi_mutex is already held (see the audio_task loop below) —
// vs1053_write_sdi() does not take the mutex itself (to avoid
// double-locking a non-recursive mutex).
// -----------------------------------------------------------------
static esp_err_t send_to_codec(const uint8_t* data, size_t len) {
  return vs1053_write_sdi(data, (uint8_t)len);
}

// -----------------------------------------------------------------
// Returns the path of the track to play next. The only place that
// knows about AUDIO_DEBUG_MODE — audio_task itself doesn't see this
// detail.
// -----------------------------------------------------------------
static void get_current_track_path(char* buf, size_t buf_size) {
#if AUDIO_DEBUG_MODE
  snprintf(buf, buf_size, "%s/%s", s_mount_point, AUDIO_DEBUG_FILENAME);
#else
  // TODO: real track selection from the list built by vSDTask
  // (selection happens on SCREEN_LIST -> SCREEN_PLAYER,
  // architecture.md). The stub below doesn't read any list —
  // don't switch to AUDIO_DEBUG_MODE=0 until this branch is
  // implemented.
  snprintf(buf, buf_size, "%s/%s", s_mount_point, "song.mp3");
#endif
}

static void handle_cmd(const player_cmd_t* cmd) {
  switch (cmd->type) {
    case CMD_PLAY_PAUSE:
      if (s_state == PLAYBACK_PLAYING) {
        s_state = PLAYBACK_PAUSED;
        ESP_LOGI(TAG, "Playback paused");
      } else if (s_state == PLAYBACK_PAUSED) {
        s_state = PLAYBACK_PLAYING;
        ESP_LOGI(TAG, "Playback resumed");
      }
      break;

    case CMD_NEXT:
    case CMD_PREV:
      // TODO: track switching (out of scope for this example)
      ESP_LOGW(TAG, "Track switching not implemented yet, command ignored");
      break;

    case CMD_VOLUME_UP:
      if (s_volume >= 100) {
        ESP_LOGW(TAG, "Volume already at max (%d)", s_volume);
        break;
      }
      s_volume = (s_volume <= 90) ? (uint8_t)(s_volume + 10) : 100;
      vs1053_set_volume(s_volume);  // takes the mutex INTERNALLY, we do NOT hold it here
      ESP_LOGI(TAG, "Volume up -> %d", s_volume);
      break;

    case CMD_VOLUME_DOWN:
      if (s_volume == 0) {
        ESP_LOGW(TAG, "Volume already at min (%d)", s_volume);
        break;
      }
      s_volume = (s_volume >= 10) ? (uint8_t)(s_volume - 10) : 0;
      vs1053_set_volume(s_volume);
      ESP_LOGI(TAG, "Volume down -> %d", s_volume);
      break;

    default:
      ESP_LOGW(TAG, "Unknown command type %d, ignored", cmd->type);
      break;
  }
}

static void audio_task(void* arg) {
  char path[64];
  get_current_track_path(path, sizeof(path));

  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file %s", path);
    s_state = PLAYBACK_STOPPED;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Streaming %s", path);
  s_state = PLAYBACK_PLAYING;

  // Reference VS1053 libraries (e.g. Adafruit, esp-idf-vs1053) send a
  // handful of zero "filler" bytes over SDI before the first real MP3
  // data, to prime the decoder. Our stream never sends these — try it,
  // since HDAT0/HDAT1 never showing decoded-frame info otherwise matches
  // the symptom this is meant to fix.
  {
    uint8_t filler[10] = {0};
    if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
      send_to_codec(filler, sizeof(filler));
      xSemaphoreGive(s_spi_mutex);
    }
  }

// 2048 reintroduced the SD CRC failures sd-card-issues.md already fixed
// once (sdspi_host: data CRC failed, mid-stream): a longer continuous SPI
// transaction on this breadboard wiring/100kHz is more likely to glitch.
// 512 matches the FAT/SD sector size — still batches the mutex/queue-check
// overhead that came with per-32-byte reads, without the long transaction.
#define READ_BUF_SIZE 512  // read in large blocks, feed VS1053 in 32-byte SDI chunks from it
  static uint8_t read_buf[READ_BUF_SIZE];
  size_t read_buf_len = 0;
  size_t read_buf_pos = 0;
  player_cmd_t cmd;
  int consecutive_codec_errors = 0;
  int chunks_sent = 0;
  int total_chunks_logged = 0;
#define DECODE_STATUS_LOG_EVERY_N_CHUNKS 200  // ~every 1.5s of audio at 128kbps

  while (1) {
    // --- While paused: block on the Queue with no timeout (0% CPU) ---
    if (s_state == PLAYBACK_PAUSED) {
      if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        handle_cmd(&cmd);
      }
      continue;  // re-check the state before reading the file
    }

    // --- In PLAYING mode: non-blocking queue check before each
    //     chunk, so a command (e.g. pause) doesn't wait for fread ---
    if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
      handle_cmd(&cmd);
      if (s_state != PLAYBACK_PLAYING) {
        continue;  // state changed (e.g. to PAUSED) — don't read a chunk
      }
    }

    // --- Refill the read buffer from the SD card once it's drained.
    //     Reading READ_BUF_SIZE at once instead of CHUNK_SIZE (32 bytes)
    //     means the mutex take/give and queue-check overhead around each
    //     fread only happens once per 512 bytes instead of once per 32. ---
    if (read_buf_pos >= read_buf_len) {
      if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
        read_buf_len = fread(read_buf, 1, READ_BUF_SIZE, f);
        xSemaphoreGive(s_spi_mutex);
      } else {
        read_buf_len = 0;
      }
      read_buf_pos = 0;

      if (read_buf_len == 0) {
        break;  // end of file
      }
    }

    size_t bytes_to_send = read_buf_len - read_buf_pos;
    if (bytes_to_send > CHUNK_SIZE) {
      bytes_to_send = CHUNK_SIZE;
    }
    uint8_t* chunk = read_buf + read_buf_pos;

    // --- Diagnostic: dump the first few chunks as read from the SD card,
    //     to confirm fread is actually returning real file bytes (MP3
    //     sync word 0xFF 0xFB expected at the very start) before they
    //     get sent to the codec ---
    if (total_chunks_logged < 5) {
      char hex[(CHUNK_SIZE * 3) + 1];
      for (size_t i = 0; i < bytes_to_send; i++) {
        snprintf(hex + (i * 3), 4, "%02X ", chunk[i]);
      }
      ESP_LOGI(TAG, "chunk %d (%d bytes): %s", total_chunks_logged, (int)bytes_to_send, hex);
      total_chunks_logged++;
    }

    // --- Send to VS1053, under the mutex for the same SPI bus ---
    esp_err_t codec_ret = ESP_FAIL;
    if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
      codec_ret = send_to_codec(chunk, bytes_to_send);
      xSemaphoreGive(s_spi_mutex);
    }
    read_buf_pos += bytes_to_send;

    if (codec_ret != ESP_OK) {
      if (++consecutive_codec_errors >= MAX_CONSECUTIVE_CODEC_ERRORS) {
        ESP_LOGE(TAG, "VS1053 unresponsive after %d consecutive errors, stopping playback",
                 consecutive_codec_errors);
        break;
      }
    } else {
      consecutive_codec_errors = 0;
    }

    // --- Diagnostic: confirm the decoder is actually locking onto MP3
    //     frames in the data we're sending, not just accepting bytes ---
    if (++chunks_sent >= DECODE_STATUS_LOG_EVERY_N_CHUNKS) {
      chunks_sent = 0;
      vs1053_log_decode_status();
    }

    // CONFIG_FREERTOS_HZ=100 means vTaskDelay(1) blocks for a whole 10ms
    // tick — calling it after every 32-byte chunk capped throughput at
    // ~3.2KB/s, far below the ~16-20KB/s a 128kbps MP3 needs. DREQ
    // (checked inside vs1053_write_sdi) is what actually paces us against
    // the codec; this is only here often enough to keep the watchdog fed
    // and let other tasks run.
    if ((chunks_sent % 32) == 0) {
      taskYIELD();
    }
  }

  ESP_LOGI(TAG, "Done streaming file");
  fclose(f);
  s_state = PLAYBACK_STOPPED;
  vTaskDelete(NULL);
}

esp_err_t audio_task_init(QueueHandle_t cmd_queue, SemaphoreHandle_t spi_mutex,
                          const char* mount_point) {
  if (cmd_queue == NULL || spi_mutex == NULL || mount_point == NULL) {
    ESP_LOGE(TAG, "audio_task_init: invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  s_cmd_queue = cmd_queue;
  s_spi_mutex = spi_mutex;
  strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);

#if AUDIO_DEBUG_MODE
  ESP_LOGW(TAG, "AUDIO_DEBUG_MODE enabled: always playing %s, track list ignored",
           AUDIO_DEBUG_FILENAME);
#endif

  BaseType_t ret =
      xTaskCreate(audio_task, "audio_task", AUDIO_TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIORITY, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio_task");
    return ESP_FAIL;
  }

  return ESP_OK;
}
