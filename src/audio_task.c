#include "audio_task.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "player_state.h"
#include "mp3_info.h"
#include "player_types.h"
#include "sd_card.h"
#include "vs1053.h"

static const char* TAG = "audio_task";

#define CHUNK_SIZE 32  // VS1053: 32-byte chunks over SPI
#define AUDIO_TASK_STACK_SIZE 4096
#define AUDIO_TASK_PRIORITY 5
#define PROGRESS_UPDATE_PERIOD_US 500000  // how often elapsed time is read from the codec
#define MAX_CONSECUTIVE_CODEC_ERRORS 10  // stop playback if VS1053 stays unresponsive

// AUDIO_DEBUG_MODE / AUDIO_DEBUG_FILENAME are defined in audio_task.h

// -----------------------------------------------------------------
// Module-private state (NOT extern, accessible only within this file).
// Playback state / volume / track index live in player_state.
// -----------------------------------------------------------------
static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_spi_mutex = NULL;

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
static esp_err_t get_current_track_path(char* buf, size_t buf_size) {
#if AUDIO_DEBUG_MODE
  snprintf(buf, buf_size, "%s/%s", sd_card_get_mount_point(), AUDIO_DEBUG_FILENAME);
  return ESP_OK;
#else
  // Track chosen via player_state (set from SCREEN_LIST -> SCREEN_PLAYER,
  // architecture.md), resolved to a path by the SD module's track list.
  player_state_t state;
  player_state_get(&state);
  return sd_card_get_track_path(state.track_index, buf, buf_size);
#endif
}

// Returns true if the command changed the current track (the caller must
// stop the current stream and start the track at player_state's index).
static bool handle_cmd(const player_cmd_t* cmd) {
  switch (cmd->type) {
    case CMD_PLAY_PAUSE: {
      playback_state_t playback = player_state_get_playback();
      if (playback == PLAYBACK_PLAYING) {
        player_state_set_playback(PLAYBACK_PAUSED);
        ESP_LOGI(TAG, "Playback paused");
      } else if (playback == PLAYBACK_PAUSED) {
        player_state_set_playback(PLAYBACK_PLAYING);
        ESP_LOGI(TAG, "Playback resumed");
      }
      break;
    }

    // No wrap-around: at the list boundaries player_state ignores the
    // command (logs a warning) and the current track keeps playing.
    case CMD_NEXT:
      return player_state_next_track();

    case CMD_PREV:
      return player_state_prev_track();

    case CMD_SELECT_TRACK:
      // An explicit selection restarts the track even if it is the current one.
      return player_state_select_track(cmd->index) == ESP_OK;

    case CMD_VOLUME_UP:
    case CMD_VOLUME_DOWN: {
      uint8_t old_volume = player_state_get_volume();
      uint8_t volume = player_state_change_volume(cmd->type == CMD_VOLUME_UP ? 10 : -10);
      if (volume == old_volume) {
        break;  // already at a limit (player_state logged the warning)
      }
      // takes spi_mutex INTERNALLY, we do NOT hold it here (nor the state lock)
      vs1053_set_volume(volume);
      ESP_LOGI(TAG, "Volume %s -> %d", cmd->type == CMD_VOLUME_UP ? "up" : "down", volume);
      break;
    }

    default:
      ESP_LOGW(TAG, "Unknown command type %d, ignored", cmd->type);
      break;
  }
  return false;
}

typedef enum {
  TRACK_FINISHED,  // end of file, codec error, or the track could not be opened
  TRACK_CHANGED,   // a command selected another track — start it right away
} track_result_t;

// Streams the track at player_state's current index until it ends or a
// command switches the track. Sets the state to PLAYING on entry (so a
// track change also clears a pause); the caller sets STOPPED on
// TRACK_FINISHED.
static track_result_t stream_current_track(void) {
  char path[SD_MAX_NAME + 16];
  if (get_current_track_path(path, sizeof(path)) != ESP_OK) {
    ESP_LOGE(TAG, "No track to play");
    return TRACK_FINISHED;
  }

  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file %s", path);
    return TRACK_FINISHED;
  }

  ESP_LOGI(TAG, "Streaming %s", path);

  // Track length for the display. Reads the file head, so it needs spi_mutex
  // (the SD card shares the bus with the VS1053).
  uint32_t durationSec = 0;
  struct stat st;
  if (stat(path, &st) == 0 && xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
    durationSec = mp3_get_duration_sec(f, (size_t)st.st_size);
    xSemaphoreGive(s_spi_mutex);
  }
  vs1053_reset_decode_time();  // takes spi_mutex internally
  player_state_set_progress(0, durationSec);
  int64_t lastProgressUs = esp_timer_get_time();

  player_state_set_playback(PLAYBACK_PLAYING);

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
  track_result_t result = TRACK_FINISHED;
#define DECODE_STATUS_LOG_EVERY_N_CHUNKS 200  // ~every 1.5s of audio at 128kbps

  while (1) {
    // --- While paused: block on the Queue with no timeout (0% CPU) ---
    if (player_state_get_playback() == PLAYBACK_PAUSED) {
      if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        if (handle_cmd(&cmd)) {
          result = TRACK_CHANGED;
          break;
        }
      }
      continue;  // re-check the state before reading the file
    }

    // --- In PLAYING mode: non-blocking queue check before each
    //     chunk, so a command (e.g. pause) doesn't wait for fread ---
    if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
      if (handle_cmd(&cmd)) {
        result = TRACK_CHANGED;
        break;
      }
      if (player_state_get_playback() != PLAYBACK_PLAYING) {
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

    // --- Publish the playback position for the display ---
    if (esp_timer_get_time() - lastProgressUs >= PROGRESS_UPDATE_PERIOD_US) {
      lastProgressUs = esp_timer_get_time();
      uint16_t decodedSec = 0;
      if (vs1053_get_decode_time(&decodedSec) == ESP_OK) {  // takes spi_mutex internally
        player_state_set_progress(decodedSec, durationSec);
      }
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

  ESP_LOGI(TAG, "%s", result == TRACK_CHANGED ? "Track changed, closing file" : "Done streaming file");
  fclose(f);
  return result;
}

static void audio_task(void* arg) {
  player_cmd_t cmd;
  bool start_track = true;  // autoplay the current track on startup

  // The task never exits: after a track ends it waits for commands so
  // Next/Prev/Select/Play can start another track.
  while (1) {
    if (start_track) {
      if (stream_current_track() == TRACK_CHANGED) {
        continue;  // start_track stays true: open the newly selected track
      }
      player_state_set_playback(PLAYBACK_STOPPED);
      ESP_LOGI(TAG, "Playback stopped, waiting for commands");
      start_track = false;
      continue;
    }

    // --- STOPPED: block on the Queue with no timeout (0% CPU) ---
    if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
      if (handle_cmd(&cmd) || cmd.type == CMD_PLAY_PAUSE) {
        start_track = true;  // track changed, or Play pressed — (re)start current track
      }
    }
  }
}

esp_err_t audio_task_init(QueueHandle_t cmd_queue, SemaphoreHandle_t spi_mutex) {
  if (cmd_queue == NULL || spi_mutex == NULL) {
    ESP_LOGE(TAG, "audio_task_init: invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  s_cmd_queue = cmd_queue;
  s_spi_mutex = spi_mutex;

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
