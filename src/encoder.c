#include "encoder.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "player_types.h"

static const char* TAG = "encoder";

// Swap the two pins if turning clockwise lowers the volume.
#define ENCODER_PIN_A GPIO_NUM_21
#define ENCODER_PIN_B GPIO_NUM_47

// PCNT counts every edge of A and B (x4 decoding). A 20-pulse/20-detent EC11
// gives 4 edges per detent; the 30-detent/15-pulse variant gives 2.
#define EDGES_PER_DETENT 4

// Filters electrical spikes only (the PCNT maximum is ~10 us); contact bounce
// is handled by the quadrature decoding itself, which nets it out to zero.
#define GLITCH_FILTER_NS 1000

static QueueHandle_t s_cmd_queue = NULL;

// Runs in the PCNT ISR. The counter is reset to 0 by the hardware on reaching
// a limit, so this fires exactly once per detent. IRAM-safe (see
// CONFIG_PCNT_ISR_IRAM_SAFE in sdkconfig.defaults): the BLE stack writes NVS,
// so the flash cache can be off while the knob is turned.
static bool IRAM_ATTR on_detent(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t* edata,
                                void* user_ctx) {
  (void)unit;
  (void)user_ctx;
  player_cmd_t cmd = {.type = edata->watch_point_value > 0 ? CMD_VOLUME_UP : CMD_VOLUME_DOWN};
  BaseType_t woken = pdFALSE;

  (void)xQueueSendFromISR(s_cmd_queue, &cmd, &woken);
  return woken == pdTRUE;
}

static esp_err_t setup_channel(pcnt_unit_handle_t unit, gpio_num_t edge_pin, gpio_num_t level_pin,
                               pcnt_channel_edge_action_t pos_edge_action,
                               pcnt_channel_edge_action_t neg_edge_action) {
  const pcnt_chan_config_t chan_cfg = {
      .edge_gpio_num = edge_pin,
      .level_gpio_num = level_pin,
  };
  pcnt_channel_handle_t chan = NULL;

  ESP_RETURN_ON_ERROR(pcnt_new_channel(unit, &chan_cfg, &chan), TAG, "pcnt_new_channel failed");
  ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(chan, pos_edge_action, neg_edge_action), TAG,
                      "pcnt_channel_set_edge_action failed");
  // The level of the other pin decides the direction: inverted when it is high.
  return pcnt_channel_set_level_action(chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                       PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
}

esp_err_t encoder_init(QueueHandle_t cmd_queue) {
  if (cmd_queue == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  s_cmd_queue = cmd_queue;

  const pcnt_unit_config_t unit_cfg = {
      .low_limit = -EDGES_PER_DETENT,
      .high_limit = EDGES_PER_DETENT,
  };
  pcnt_unit_handle_t unit = NULL;
  ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &unit), TAG, "pcnt_new_unit failed");

  const pcnt_glitch_filter_config_t filter_cfg = {.max_glitch_ns = GLITCH_FILTER_NS};
  ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(unit, &filter_cfg), TAG,
                      "pcnt_unit_set_glitch_filter failed");

  // The classic quadrature wiring (as in the ESP-IDF rotary encoder example):
  // each channel counts edges of one pin, with the other pin as the level.
  ESP_RETURN_ON_ERROR(setup_channel(unit, ENCODER_PIN_A, ENCODER_PIN_B,
                                    PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                    PCNT_CHANNEL_EDGE_ACTION_INCREASE),
                      TAG, "channel A failed");
  ESP_RETURN_ON_ERROR(setup_channel(unit, ENCODER_PIN_B, ENCODER_PIN_A,
                                    PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                    PCNT_CHANNEL_EDGE_ACTION_DECREASE),
                      TAG, "channel B failed");

  // After the channels: the PCNT driver reconfigures the pins when creating them.
  ESP_RETURN_ON_ERROR(gpio_set_pull_mode(ENCODER_PIN_A, GPIO_PULLUP_ONLY), TAG, "pull-up A failed");
  ESP_RETURN_ON_ERROR(gpio_set_pull_mode(ENCODER_PIN_B, GPIO_PULLUP_ONLY), TAG, "pull-up B failed");

  ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(unit, EDGES_PER_DETENT), TAG,
                      "watch point (+) failed");
  ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(unit, -EDGES_PER_DETENT), TAG,
                      "watch point (-) failed");

  const pcnt_event_callbacks_t cbs = {.on_reach = on_detent};
  ESP_RETURN_ON_ERROR(pcnt_unit_register_event_callbacks(unit, &cbs, NULL), TAG,
                      "register callbacks failed");

  ESP_RETURN_ON_ERROR(pcnt_unit_enable(unit), TAG, "pcnt_unit_enable failed");
  ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(unit), TAG, "pcnt_unit_clear_count failed");
  ESP_RETURN_ON_ERROR(pcnt_unit_start(unit), TAG, "pcnt_unit_start failed");

  ESP_LOGI(TAG, "Initialized: A=%d B=%d, %d edges per detent", (int)ENCODER_PIN_A,
           (int)ENCODER_PIN_B, EDGES_PER_DETENT);

  return ESP_OK;
}
