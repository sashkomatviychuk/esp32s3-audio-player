#include "audio_task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "mp3_info.h"
#include "player_state.h"
#include "player_types.h"
#include "sd_card.h"
#include "vs1053.h"

static const char* TAG = "audio_task";

#define CHUNK_SIZE 32  // VS1053: 32-byte chunks over SPI
#define AUDIO_TASK_STACK_SIZE 4096
#define AUDIO_TASK_PRIORITY 5
#define PROGRESS_UPDATE_PERIOD_US 500000  // how often elapsed time is published
#define MAX_CONSECUTIVE_CODEC_ERRORS 10   // stop playback if VS1053 stays unresponsive
#define SD_POLL_PERIOD_MS 200             // how often idle/paused states check the SD card
#define VOLUME_STEP 5                     // percent per CMD_VOLUME_UP / CMD_VOLUME_DOWN
#define CODEC_FILLER_BYTES 10             // zero bytes sent before the first MP3 data
#define PATH_EXTRA_CHARS 16               // room for "<mount point>/" around a track name
#define BITS_PER_BYTE 8
#define YIELD_EVERY_N_CHUNKS 32  // taskYIELD() cadence, keeps the watchdog fed

// 2048 reintroduced the SD CRC failures sd-card-issues.md already fixed
// once (sdspi_host: data CRC failed, mid-stream): a longer continuous SPI
// transaction on this breadboard wiring/100kHz is more likely to glitch.
// 512 matches the FAT/SD sector size — still batches the mutex/queue-check
// overhead that came with per-32-byte reads, without the long transaction.
#define READ_BUF_SIZE 512  // read in large blocks, feed VS1053 in 32-byte SDI chunks from it

// CONFIG_AUDIO_DEBUG_* options come from src/Kconfig.projbuild

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

// Reads the track length and bitrate for the display. Reads the file head, so
// it needs spi_mutex (the SD card shares the bus with the VS1053).
static void read_track_info(const char* path, FILE* f, mp3_info_t* info) {
  struct stat st;
  if (stat(path, &st) == 0 && xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
    mp3_get_info(f, (size_t)st.st_size, info);
    xSemaphoreGive(s_spi_mutex);
  }
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
  mp3_info_t info = {0};
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
      // takes spi_mutex INTERNALLY, we do NOT hold it here (nor the state lock)
      vs1053_set_volume(volume);
      ESP_LOGI(TAG, "Volume %s -> %d", cmd->type == CMD_VOLUME_UP ? "up" : "down", volume);
      break;
    }

    case CMD_TOGGLE_MUTE: {
      bool muted = player_state_toggle_mute();
      // The stored volume is kept, so unmuting restores the previous level.
      vs1053_set_volume(muted ? 0 : player_state_get_volume());
      ESP_LOGI(TAG, "%s", muted ? "Muted" : "Unmuted");
      break;
    }

    default:
      ESP_LOGW(TAG, "Unknown command type %d, ignored", cmd->type);
      break;
  }
  return false;
}

// Primes the decoder before the first MP3 data: reference VS1053 libraries
// (e.g. Adafruit, esp-idf-vs1053) send a handful of zero "filler" bytes over
// SDI at the start of every stream.
static void send_codec_filler(void) {
  uint8_t filler[CODEC_FILLER_BYTES] = {0};
  if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
    send_to_codec(filler, sizeof(filler));
    xSemaphoreGive(s_spi_mutex);
  }
}

typedef enum {
  TRACK_EOF,       // the file was played to its end — the caller may advance to the next track
  TRACK_FINISHED,  // aborted: codec/SD error, SD removed, or the track could not be opened
  TRACK_CHANGED,   // a command selected another track — start it right away
} track_result_t;

// Why the streaming loop ended; END_NONE means "keep streaming".
typedef enum {
  END_NONE,
  END_EOF,          // real end of file
  END_CHANGED,      // a command switched the track
  END_SD_REMOVED,   // the card was pulled
  END_READ_ERROR,   // fread() failed (e.g. an SD CRC error), not a real EOF
  END_CODEC_ERROR,  // the VS1053 stayed unresponsive
} stream_end_t;

// Per-track streaming state, owned by stream_current_track().
typedef struct {
  FILE* file;
  mp3_info_t info;
  // Elapsed time is derived from the bytes handed to the codec (the VS1053
  // decode-time register is not usable while the decoder does not lock on).
  // DREQ paces the transfer, so this follows real playback; pause is
  // accounted for automatically because no bytes are sent while paused.
  uint64_t bytes_sent;
  int64_t last_progress_us;
  size_t buf_len;  // valid bytes in s_read_buf
  size_t buf_pos;  // next byte of s_read_buf to send
  int read_errno;  // errno of the failed fread (END_READ_ERROR)
  int consecutive_codec_errors;
  unsigned chunks_sent;  // yield cadence; never reset
  int chunks_logged;     // CONFIG_AUDIO_DEBUG_DUMP_CHUNKS counter
  int chunks_since_status;
} stream_ctx_t;

static uint8_t s_read_buf[READ_BUF_SIZE];

// --- Diagnostic: dump the first few chunks as read from the SD card, to
//     confirm fread is actually returning real file bytes (MP3 sync word
//     0xFF 0xFB expected at the very start) before they get sent to the codec ---
static void debug_dump_chunk(stream_ctx_t* ctx, const uint8_t* chunk, size_t len) {
  if (ctx->chunks_logged >= CONFIG_AUDIO_DEBUG_DUMP_CHUNKS) {
    return;
  }
  char hex[(CHUNK_SIZE * 3) + 1];
  for (size_t i = 0; i < len; i++) {
    snprintf(hex + (i * 3), 4, "%02X ", chunk[i]);
  }
  ESP_LOGI(TAG, "chunk %d (%d bytes): %s", ctx->chunks_logged, (int)len, hex);
  ctx->chunks_logged++;
}

// --- Diagnostic: confirm the decoder is actually locking onto MP3 frames in
//     the data we're sending, not just accepting bytes ---
static void debug_log_decode_status(stream_ctx_t* ctx) {
#if CONFIG_AUDIO_DEBUG_DECODE_STATUS_PERIOD_CHUNKS > 0
  if (++ctx->chunks_since_status >= CONFIG_AUDIO_DEBUG_DECODE_STATUS_PERIOD_CHUNKS) {
    ctx->chunks_since_status = 0;
    vs1053_log_decode_status();
  }
#else
  (void)ctx;
#endif
}

// Publishes the playback position for the display, at most once per
// PROGRESS_UPDATE_PERIOD_US.
static void publish_progress(stream_ctx_t* ctx) {
  if (ctx->info.bitrate_bps == 0) {
    return;
  }
  int64_t now_us = esp_timer_get_time();
  if (now_us - ctx->last_progress_us < PROGRESS_UPDATE_PERIOD_US) {
    return;
  }
  ctx->last_progress_us = now_us;
  uint64_t audio_bytes =
      ctx->bytes_sent > ctx->info.audio_start ? ctx->bytes_sent - ctx->info.audio_start : 0;
  player_state_set_progress((uint32_t)(audio_bytes * BITS_PER_BYTE / ctx->info.bitrate_bps),
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

// Refills the read buffer from the SD card. Reading READ_BUF_SIZE at once
// instead of CHUNK_SIZE (32 bytes) means the mutex take/give and queue-check
// overhead around each fread only happens once per 512 bytes instead of once
// per 32. Returns END_NONE when new data is available.
static stream_end_t refill_buffer(stream_ctx_t* ctx) {
  if (!sd_is_present()) {
    return END_SD_REMOVED;
  }

  size_t len = 0;
  if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
    len = fread(s_read_buf, 1, READ_BUF_SIZE, ctx->file);
    xSemaphoreGive(s_spi_mutex);
  }
  ctx->buf_len = len;
  ctx->buf_pos = 0;
  if (len > 0) {
    return END_NONE;
  }

  // fread() returns 0 both at a real EOF and on a read error (e.g. an SD CRC
  // failure) — ferror() tells them apart.
  if (ferror(ctx->file)) {
    ctx->read_errno = errno;
    return END_READ_ERROR;
  }
  return END_EOF;
}

// Sends the next chunk of the read buffer to the VS1053 (under the mutex for
// the same SPI bus). Returns END_CODEC_ERROR once the codec has failed
// MAX_CONSECUTIVE_CODEC_ERRORS times in a row, END_NONE otherwise.
static stream_end_t send_next_chunk(stream_ctx_t* ctx) {
  size_t bytes_to_send = ctx->buf_len - ctx->buf_pos;
  if (bytes_to_send > CHUNK_SIZE) {
    bytes_to_send = CHUNK_SIZE;
  }
  const uint8_t* chunk = s_read_buf + ctx->buf_pos;

  debug_dump_chunk(ctx, chunk, bytes_to_send);
  publish_progress(ctx);

  esp_err_t codec_ret = ESP_FAIL;
  if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) == pdTRUE) {
    codec_ret = send_to_codec(chunk, bytes_to_send);
    xSemaphoreGive(s_spi_mutex);
  }
  ctx->buf_pos += bytes_to_send;

  if (codec_ret != ESP_OK) {
    if (++ctx->consecutive_codec_errors >= MAX_CONSECUTIVE_CODEC_ERRORS) {
      return END_CODEC_ERROR;
    }
  } else {
    ctx->consecutive_codec_errors = 0;
    ctx->bytes_sent += bytes_to_send;
  }

  ctx->chunks_sent++;
  debug_log_decode_status(ctx);

  // CONFIG_FREERTOS_HZ=100 means vTaskDelay(1) blocks for a whole 10ms
  // tick — calling it after every 32-byte chunk capped throughput at
  // ~3.2KB/s, far below the ~16-20KB/s a 128kbps MP3 needs. DREQ
  // (checked inside vs1053_write_sdi) is what actually paces us against
  // the codec; this is only here often enough to keep the watchdog fed
  // and let other tasks run.
  if ((ctx->chunks_sent % YIELD_EVERY_N_CHUNKS) == 0) {
    taskYIELD();
  }
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

    // In PLAYING mode: non-blocking queue check before each chunk, so a
    // command (e.g. pause) doesn't wait for fread.
    player_cmd_t cmd;
    if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
      if (handle_cmd(&cmd)) {
        end = END_CHANGED;
      }
      continue;  // the state may have changed (e.g. to PAUSED) — don't read a chunk yet
    }

    if (ctx->buf_pos >= ctx->buf_len) {
      end = refill_buffer(ctx);
      if (end != END_NONE) {
        continue;
      }
    }
    end = send_next_chunk(ctx);
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

    case END_CODEC_ERROR:
      ESP_LOGE(TAG, "VS1053 unresponsive after %d consecutive errors, stopping playback",
               MAX_CONSECUTIVE_CODEC_ERRORS);
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

  read_track_info(path, ctx.file, &ctx.info);
  player_state_set_progress(0, ctx.info.duration_sec);
  ctx.last_progress_us = esp_timer_get_time();

  player_state_set_playback(PLAYBACK_PLAYING);
  send_codec_filler();

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
