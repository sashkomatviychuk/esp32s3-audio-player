#include "audio_task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "pcm5102.h"
#include "player_state.h"
#include "player_types.h"
#include "sd_card.h"
#include "wav_info.h"

static const char* TAG = "audio_task";

#define AUDIO_TASK_STACK_SIZE 4096
#define AUDIO_TASK_PRIORITY 5
#define PROGRESS_UPDATE_PERIOD_US 500000  // how often elapsed time is published
#define SD_POLL_PERIOD_MS 200             // how often idle/paused states check the SD card
#define VOLUME_STEP 5                     // percent per CMD_VOLUME_UP / CMD_VOLUME_DOWN
#define PATH_EXTRA_CHARS 16               // room for "<mount point>/" around a track name

// One SD read = one I2S write. At 44.1 kHz stereo 2048 bytes are ~12 ms of
// audio, far less than the DMA buffer in pcm5102.c (~87 ms), so a slow SD read
// does not starve the DAC, and a command waits at most one block. Must be a
// multiple of 4 (one stereo frame) so a block never ends mid-frame.
#define READ_BUF_SIZE 2048

// CONFIG_AUDIO_DEBUG_* options come from src/Kconfig.projbuild

// -----------------------------------------------------------------
// Module-private state (NOT extern, accessible only within this file).
// Playback state / volume / track index live in player_state.
// -----------------------------------------------------------------
static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_spi_mutex = NULL;

// -----------------------------------------------------------------
// Returns the path of the track to play next. The only place that
// knows about CONFIG_AUDIO_DEBUG_MODE — audio_task itself doesn't see this
// detail.
// -----------------------------------------------------------------
static esp_err_t get_current_track_path(char* buf, size_t buf_size) {
#if CONFIG_AUDIO_DEBUG_MODE
  snprintf(buf, buf_size, "%s/%s", sd_card_get_mount_point(), CONFIG_AUDIO_DEBUG_FILENAME);
  return ESP_OK;
#else
  // Track chosen via player_state (set from SCREEN_LIST -> SCREEN_PLAYER,
  // architecture.md), resolved to a path by the SD module's track list.
  player_state_t state;
  player_state_get(&state);
  return sd_card_get_track_path(state.track_index, buf, buf_size);
#endif
}

static bool sd_is_present(void) {
  player_state_t state;
  player_state_get(&state);
  return state.sd_present;
}

// Reads the WAV header (length, format) for the display and the stream. Reads
// the file head, so it needs spi_mutex (the SD card is shared with the scan
// task). Leaves the file positioned at the first audio byte.
static esp_err_t read_track_info(const char* path, FILE* f, wav_info_t* info) {
  struct stat st;
  if (stat(path, &st) != 0 || xSemaphoreTake(s_spi_mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_FAIL;
  }
  esp_err_t err = wav_get_info(f, (size_t)st.st_size, info);
  xSemaphoreGive(s_spi_mutex);
  return err;
}

// Opens the track at player_state's current index and stores its path in
// @p path. Returns NULL (after logging) if there is no such track or it
// cannot be opened.
static FILE* open_current_track(char* path, size_t path_size) {
  if (get_current_track_path(path, path_size) != ESP_OK) {
    ESP_LOGE(TAG, "No track to play");
    return NULL;
  }
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file %s", path);
  }
  return f;
}

// Publishes the duration of the current track without starting playback.
static void load_track_duration(void) {
  char path[SD_MAX_NAME + PATH_EXTRA_CHARS];
  FILE* f = open_current_track(path, sizeof(path));
  if (f == NULL) {
    return;
  }
  wav_info_t info = {0};
  read_track_info(path, f, &info);
  fclose(f);
  player_state_set_progress(0, info.duration_sec);
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
      player_state_t before;
      player_state_get(&before);
      // A volume change also unmutes (player_state clears the flag).
      uint8_t volume =
          player_state_change_volume(cmd->type == CMD_VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP);
      if (volume == before.volume && !before.muted) {
        break;  // already at a limit (player_state logged the warning)
      }
      pcm5102_set_volume(volume);
      pcm5102_set_mute(false);
      ESP_LOGI(TAG, "Volume %s -> %d", cmd->type == CMD_VOLUME_UP ? "up" : "down", volume);
      break;
    }

    case CMD_TOGGLE_MUTE: {
      bool muted = player_state_toggle_mute();
      // The stored volume is kept, so unmuting restores the previous level.
      pcm5102_set_mute(muted);
      ESP_LOGI(TAG, "%s", muted ? "Muted" : "Unmuted");
      break;
    }

    default:
      ESP_LOGW(TAG, "Unknown command type %d, ignored", cmd->type);
      break;
  }
  return false;
}

typedef enum {
  TRACK_EOF,       // the file was played to its end — the caller may advance to the next track
  TRACK_FINISHED,  // aborted: I2S/SD error, SD removed, or the track could not be played
  TRACK_CHANGED,   // a command selected another track — start it right away
} track_result_t;

// Why the streaming loop ended; END_NONE means "keep streaming".
typedef enum {
  END_NONE,
  END_EOF,         // real end of the audio data
  END_CHANGED,     // a command switched the track
  END_SD_REMOVED,  // the card was pulled
  END_READ_ERROR,  // fread() failed (e.g. an SD CRC error), not a real EOF
  END_I2S_ERROR,   // the I2S write failed
} stream_end_t;

// Per-track streaming state, owned by stream_current_track().
typedef struct {
  FILE* file;
  wav_info_t info;
  // Elapsed time is derived from the audio bytes handed to the DAC. The I2S
  // write blocks until the DMA has room, so this follows real playback within
  // the DMA depth; pause is accounted for automatically because no bytes are
  // sent while paused.
  uint64_t bytes_played;
  uint32_t bytes_left;  // audio bytes not yet read from the file
  int64_t last_progress_us;
  size_t buf_len;  // valid bytes in s_read_buf
  int read_errno;  // errno of the failed fread (END_READ_ERROR)
} stream_ctx_t;

static int16_t s_read_buf[READ_BUF_SIZE / sizeof(int16_t)];  // samples as read from the file
static int16_t s_out_buf[READ_BUF_SIZE];                     // always stereo: 2x a mono block

// Publishes the playback position for the display, at most once per
// PROGRESS_UPDATE_PERIOD_US.
static void publish_progress(stream_ctx_t* ctx) {
  if (ctx->info.byte_rate == 0) {
    return;
  }
  int64_t now_us = esp_timer_get_time();
  if (now_us - ctx->last_progress_us < PROGRESS_UPDATE_PERIOD_US) {
    return;
  }
  ctx->last_progress_us = now_us;
  player_state_set_progress((uint32_t)(ctx->bytes_played / ctx->info.byte_rate),
                            ctx->info.duration_sec);
}

// While paused: blocks on the queue (near 0% CPU). The timeout only exists to
// notice the SD card being pulled — the file is dead then.
static stream_end_t wait_while_paused(void) {
  player_cmd_t cmd;
  if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(SD_POLL_PERIOD_MS)) == pdTRUE) {
    return handle_cmd(&cmd) ? END_CHANGED : END_NONE;
  }
  return sd_is_present() ? END_NONE : END_SD_REMOVED;
}

// Refills the read buffer from the SD card with the next block of audio data,
// never reading past the end of the "data" chunk. Returns END_NONE when new
// data is available.
static stream_end_t refill_buffer(stream_ctx_t* ctx) {
  if (!sd_is_present()) {
    return END_SD_REMOVED;
  }
  if (ctx->bytes_left == 0) {
    return END_EOF;
  }

  size_t to_read = ctx->bytes_left < READ_BUF_SIZE ? ctx->bytes_left : READ_BUF_SIZE;
  size_t len = 0;
  if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
    len = fread(s_read_buf, 1, to_read, ctx->file);
    xSemaphoreGive(s_spi_mutex);
  }
  ctx->buf_len = len;
  if (len > 0) {
    ctx->bytes_left -= (uint32_t)len;
    return END_NONE;
  }

  // fread() returns 0 both at a real EOF (a file shorter than its header
  // claims) and on a read error (e.g. an SD CRC failure) — ferror() tells them
  // apart.
  if (ferror(ctx->file)) {
    ctx->read_errno = errno;
    return END_READ_ERROR;
  }
  return END_EOF;
}

// Converts the block in s_read_buf to stereo (a mono sample is duplicated into
// both channels, so the I2S format never changes) and sends it to the DAC.
// Blocks until the DMA accepted all of it, which paces the whole stream.
static stream_end_t send_buffer(stream_ctx_t* ctx) {
  size_t bytes_per_frame = ctx->info.channels * sizeof(int16_t);
  size_t frames = ctx->buf_len / bytes_per_frame;

  if (ctx->info.channels == 1) {
    for (size_t i = 0; i < frames; i++) {
      s_out_buf[2 * i] = s_read_buf[i];
      s_out_buf[(2 * i) + 1] = s_read_buf[i];
    }
  } else {
    memcpy(s_out_buf, s_read_buf, frames * bytes_per_frame);
  }

  publish_progress(ctx);

  esp_err_t err = pcm5102_write(s_out_buf, frames);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
    return END_I2S_ERROR;
  }
  ctx->bytes_played += frames * bytes_per_frame;
  return END_NONE;
}

// Streaming loop: runs until the track ends, a command switches it, or an
// error aborts it.
static stream_end_t stream_until_end(stream_ctx_t* ctx) {
  stream_end_t end = END_NONE;
  while (end == END_NONE) {
    if (player_state_get_playback() == PLAYBACK_PAUSED) {
      end = wait_while_paused();
      continue;  // re-check the state before reading the file
    }

    // In PLAYING mode: non-blocking queue check before each block, so a
    // command (e.g. pause) doesn't wait for fread.
    player_cmd_t cmd;
    if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
      if (handle_cmd(&cmd)) {
        end = END_CHANGED;
      }
      continue;  // the state may have changed (e.g. to PAUSED) — don't read a block yet
    }

    end = refill_buffer(ctx);
    if (end != END_NONE) {
      continue;
    }
    end = send_buffer(ctx);
  }
  return end;
}

// Logs why the stream ended and maps it to the result for audio_task.
static track_result_t finish_stream(stream_end_t end, const stream_ctx_t* ctx) {
  switch (end) {
    case END_EOF:
      // The periodic update can lag up to PROGRESS_UPDATE_PERIOD_US and the byte
      // count is rounded down, so snap to the full length once the file is done.
      if (ctx->info.duration_sec > 0) {
        player_state_set_progress(ctx->info.duration_sec, ctx->info.duration_sec);
      }
      ESP_LOGI(TAG, "End of file");
      return TRACK_EOF;

    case END_CHANGED:
      ESP_LOGI(TAG, "Track changed, closing file");
      return TRACK_CHANGED;

    case END_SD_REMOVED:
      ESP_LOGW(TAG, "SD card removed, stopping playback");
      return TRACK_FINISHED;

    case END_READ_ERROR:
      ESP_LOGE(TAG, "SD read error (errno %d), stopping playback", ctx->read_errno);
      return TRACK_FINISHED;

    case END_I2S_ERROR:
      ESP_LOGE(TAG, "I2S output failed, stopping playback");
      return TRACK_FINISHED;

    default:
      return TRACK_FINISHED;
  }
}

// Streams the track at player_state's current index until it ends or a
// command switches the track. Sets the state to PLAYING on entry (so a
// track change also clears a pause); the caller sets STOPPED when there is
// nothing more to play (TRACK_FINISHED, or TRACK_EOF on the last track).
static track_result_t stream_current_track(void) {
  char path[SD_MAX_NAME + PATH_EXTRA_CHARS];
  stream_ctx_t ctx = {0};
  ctx.file = open_current_track(path, sizeof(path));
  if (ctx.file == NULL) {
    return TRACK_FINISHED;
  }

  ESP_LOGI(TAG, "Streaming %s", path);

  // Also leaves the file at the first audio byte. An unsupported or broken
  // file is logged by wav_get_info() and stops playback.
  if (read_track_info(path, ctx.file, &ctx.info) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot play %s", path);
    fclose(ctx.file);
    return TRACK_FINISHED;
  }
  ctx.bytes_left = ctx.info.data_size;
  player_state_set_progress(0, ctx.info.duration_sec);
  ctx.last_progress_us = esp_timer_get_time();

  // Also drops whatever the previous track left in the DMA buffers.
  if (pcm5102_set_sample_rate(ctx.info.sample_rate) != ESP_OK) {
    fclose(ctx.file);
    return TRACK_FINISHED;
  }

  player_state_set_playback(PLAYBACK_PLAYING);

  track_result_t result = finish_stream(stream_until_end(&ctx), &ctx);
  fclose(ctx.file);
  return result;
}

// Shows the track length once the SD card is back, without starting playback.
// *probed makes it run once per card insertion: an unreadable file must not be
// retried.
static void probe_duration_if_needed(bool* probed) {
  player_state_t state;
  player_state_get(&state);
  if (!state.sd_present) {
    *probed = false;  // probe again after the next insertion
  } else if (!*probed && state.track_count > 0 && state.duration_sec == 0) {
    *probed = true;
    load_track_duration();
  }
}

static void audio_task(void* arg) {
  player_cmd_t cmd;
  bool start_track = true;  // autoplay the current track on startup
  bool duration_probed = false;

  // The task never exits: after a track ends it waits for commands so
  // Next/Prev/Select/Play can start another track.
  while (1) {
    if (start_track) {
      track_result_t result = stream_current_track();
      if (result == TRACK_CHANGED) {
        continue;  // start_track stays true: open the newly selected track
      }
      // Auto-advance only after a real end of file, never after an error.
      if (result == TRACK_EOF) {
        if (player_state_next_track()) {
          ESP_LOGI(TAG, "Track ended, advancing to the next one");
          continue;  // start_track stays true: open the next track
        }
        ESP_LOGI(TAG, "Last track ended");
      }
      player_state_set_playback(PLAYBACK_STOPPED);
      ESP_LOGI(TAG, "Playback stopped, waiting for commands");
      start_track = false;
      continue;
    }

    // --- STOPPED: block on the Queue (near 0% CPU). The timeout lets us probe
    //     the track length once the SD card is back ---
    if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(SD_POLL_PERIOD_MS)) == pdTRUE) {
      bool track_changed = handle_cmd(&cmd);
      if (sd_is_present() && (track_changed || cmd.type == CMD_PLAY_PAUSE)) {
        start_track = true;  // track changed, or Play pressed — (re)start current track
      }
      continue;
    }

    probe_duration_if_needed(&duration_probed);
  }
}

esp_err_t audio_task_init(QueueHandle_t cmd_queue, SemaphoreHandle_t spi_mutex) {
  if (cmd_queue == NULL || spi_mutex == NULL) {
    ESP_LOGE(TAG, "audio_task_init: invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  s_cmd_queue = cmd_queue;
  s_spi_mutex = spi_mutex;

  // The DAC has no volume register: apply the initial level from player_state.
  player_state_t state;
  player_state_get(&state);
  pcm5102_set_volume(state.volume);
  pcm5102_set_mute(state.muted);

#if CONFIG_AUDIO_DEBUG_MODE
  ESP_LOGW(TAG, "CONFIG_AUDIO_DEBUG_MODE enabled: always playing %s, track list ignored",
           CONFIG_AUDIO_DEBUG_FILENAME);
#endif

  BaseType_t ret =
      xTaskCreate(audio_task, "audio_task", AUDIO_TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIORITY, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio_task");
    return ESP_FAIL;
  }

  return ESP_OK;
}
