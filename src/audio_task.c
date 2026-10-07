#include "audio_task.h"

#include <stdio.h>

#include "audio_decoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "pcm5102.h"
#include "player_state.h"
#include "player_types.h"
#include "sd_card.h"

static const char* TAG = "audio_task";

// Helix keeps its state on the heap, but a decode call, the logging and the SD/FATFS calls
// below it need stack headroom.
#define AUDIO_TASK_STACK_SIZE 8192
#define AUDIO_TASK_PRIORITY 5
#define PROGRESS_UPDATE_PERIOD_US 500000  // how often elapsed time is published
#define SD_POLL_PERIOD_MS 200             // how often idle/paused states check the SD card
#define VOLUME_STEP 5                     // percent per CMD_VOLUME_UP / CMD_VOLUME_DOWN
#define PATH_EXTRA_CHARS 16               // room for "<mount point>/" around a track name
#define US_PER_SEC 1000000U

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

// Publishes the duration of the current track without starting playback. Only the file
// header is read — no decoder is created.
static void load_track_duration(void) {
  char path[SD_MAX_NAME + PATH_EXTRA_CHARS];
  if (get_current_track_path(path, sizeof(path)) != ESP_OK) {
    ESP_LOGE(TAG, "No track to play");
    return;
  }
  audio_track_info_t info;
  if (audio_decoder_probe(path, s_spi_mutex, &info) != ESP_OK) {
    return;
  }
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
  TRACK_FINISHED,  // aborted: decode/I2S/SD error, SD removed, or the track could not be played
  TRACK_CHANGED,   // a command selected another track — start it right away
} track_result_t;

// Why the streaming loop ended; END_NONE means "keep streaming".
typedef enum {
  END_NONE,
  END_EOF,           // real end of the audio data
  END_CHANGED,       // a command switched the track
  END_SD_REMOVED,    // the card was pulled
  END_READ_ERROR,    // the SD read failed (e.g. an SD CRC error), not a real EOF
  END_DECODE_ERROR,  // the decoder gave up on a corrupt file
  END_I2S_ERROR,     // the I2S write failed
} stream_end_t;

// Per-track streaming state, owned by stream_current_track().
typedef struct {
  audio_decoder_t decoder;
  audio_track_info_t info;
  uint32_t sample_rate;  // rate the I2S clock is set to; follows decoder.sample_rate
  // Elapsed time is the duration of the PCM handed to the DAC. The I2S write blocks until
  // the DMA has room, so this follows real playback within the DMA depth; pause is accounted
  // for automatically because nothing is written while paused.
  uint64_t elapsed_us;
  int64_t last_progress_us;
} stream_ctx_t;

// Decoded stereo frames of one block, always interleaved L/R whatever the file format is.
static int16_t s_out_buf[AUDIO_DECODER_OUT_SAMPLES];

// Publishes the playback position for the display, at most once per
// PROGRESS_UPDATE_PERIOD_US.
static void publish_progress(stream_ctx_t* ctx) {
  int64_t now_us = esp_timer_get_time();
  if (now_us - ctx->last_progress_us < PROGRESS_UPDATE_PERIOD_US) {
    return;
  }
  ctx->last_progress_us = now_us;
  player_state_set_progress((uint32_t)(ctx->elapsed_us / US_PER_SEC), ctx->info.duration_sec);
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

// Decodes the next block of the track and sends it to the DAC. Both calls can block (the SD read
// and decode take a few ms, the I2S write waits for DMA space), which also paces the stream.
static stream_end_t play_next_block(stream_ctx_t* ctx) {
  if (!sd_is_present()) {
    return END_SD_REMOVED;
  }

  size_t frames = 0;
  esp_err_t err = ctx->decoder.read(&ctx->decoder, s_out_buf, AUDIO_DECODER_MAX_FRAMES, &frames);
  if (err == ESP_FAIL) {
    return END_READ_ERROR;
  }
  if (err != ESP_OK) {
    return END_DECODE_ERROR;
  }
  if (frames == 0) {
    return END_EOF;
  }

  // A stream that changes its sample rate midway (rare) needs a new I2S clock.
  if (ctx->decoder.sample_rate != ctx->sample_rate) {
    ctx->sample_rate = ctx->decoder.sample_rate;
    if (pcm5102_set_sample_rate(ctx->sample_rate) != ESP_OK) {
      return END_I2S_ERROR;
    }
  }

  publish_progress(ctx);

  err = pcm5102_write(s_out_buf, frames);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
    return END_I2S_ERROR;
  }
  ctx->elapsed_us += ((uint64_t)frames * US_PER_SEC) / ctx->sample_rate;
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
    // command (e.g. pause) doesn't wait for the decoder.
    player_cmd_t cmd;
    if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
      if (handle_cmd(&cmd)) {
        end = END_CHANGED;
      }
      continue;  // the state may have changed (e.g. to PAUSED) — don't decode a block yet
    }

    end = play_next_block(ctx);
  }
  return end;
}

// Logs why the stream ended and maps it to the result for audio_task.
static track_result_t finish_stream(stream_end_t end, const stream_ctx_t* ctx) {
  switch (end) {
    case END_EOF:
      // The periodic update can lag up to PROGRESS_UPDATE_PERIOD_US and the elapsed time is
      // rounded down, so snap to the full length once the file is done.
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
      ESP_LOGE(TAG, "SD read error (errno %d), stopping playback", ctx->decoder.read_errno);
      return TRACK_FINISHED;

    case END_DECODE_ERROR:
      ESP_LOGE(TAG, "Cannot decode the file, stopping playback");
      return TRACK_FINISHED;

    case END_I2S_ERROR:
      ESP_LOGE(TAG, "I2S output failed, stopping playback");
      return TRACK_FINISHED;

    default:
      return TRACK_FINISHED;
  }
}

static const char* format_name(audio_format_t format) {
  switch (format) {
    case AUDIO_FORMAT_WAV:
      return "WAV";
    case AUDIO_FORMAT_MP3:
      return "MP3";
    default:
      return "?";
  }
}

// Streams the track at player_state's current index until it ends or a
// command switches the track. Sets the state to PLAYING on entry (so a
// track change also clears a pause); the caller sets STOPPED when there is
// nothing more to play (TRACK_FINISHED, or TRACK_EOF on the last track).
static track_result_t stream_current_track(void) {
  char path[SD_MAX_NAME + PATH_EXTRA_CHARS];
  if (get_current_track_path(path, sizeof(path)) != ESP_OK) {
    ESP_LOGE(TAG, "No track to play");
    return TRACK_FINISHED;
  }

  ESP_LOGI(TAG, "Streaming %s", path);

  stream_ctx_t ctx = {0};
  // An unsupported or broken file is logged by the decoder and stops playback.
  if (audio_decoder_open(path, s_spi_mutex, &ctx.decoder, &ctx.info) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot play %s", path);
    return TRACK_FINISHED;
  }
  ESP_LOGI(TAG, "%s, %u Hz, %u ch, %u s", format_name(ctx.info.format),
           (unsigned)ctx.info.sample_rate, ctx.info.channels, (unsigned)ctx.info.duration_sec);

  player_state_set_progress(0, ctx.info.duration_sec);
  ctx.last_progress_us = esp_timer_get_time();

  // Also drops whatever the previous track left in the DMA buffers.
  ctx.sample_rate = ctx.decoder.sample_rate;
  if (pcm5102_set_sample_rate(ctx.sample_rate) != ESP_OK) {
    audio_decoder_close(&ctx.decoder);
    return TRACK_FINISHED;
  }

  player_state_set_playback(PLAYBACK_PLAYING);

  track_result_t result = finish_stream(stream_until_end(&ctx), &ctx);
  audio_decoder_close(&ctx.decoder);
  ESP_LOGI(TAG, "Stack high-water mark: %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
