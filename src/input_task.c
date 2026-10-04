#include "input_task.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "player_types.h"

static const char* TAG = "input_task";

#define INPUT_TASK_STACK_SIZE 2048
#define INPUT_TASK_PRIORITY 4
#define EDGE_QUEUE_LENGTH 8

#define US_PER_MS 1000
#define DEBOUNCE_MS 30
#define REPEAT_DELAY_MS 500
#define REPEAT_PERIOD_MS 150

typedef struct {
  gpio_num_t pin;
  cmd_type_t cmd;
  bool repeat;  // auto-repeat the command while the button is held
} button_cfg_t;

// Active-low: the other contact of every button goes to GND.
// DRAM_ATTR: button_isr() is IRAM-safe and reads this table while the flash
// cache may be disabled (NVS writes by the BLE stack), so it must not live in
// flash-mapped .rodata.
static const DRAM_ATTR button_cfg_t s_buttons[] = {
    {.pin = GPIO_NUM_2, .cmd = CMD_PLAY_PAUSE, .repeat = false},
    {.pin = GPIO_NUM_1, .cmd = CMD_NEXT, .repeat = false},
    {.pin = GPIO_NUM_42, .cmd = CMD_PREV, .repeat = false},
    {.pin = GPIO_NUM_21, .cmd = CMD_VOLUME_UP, .repeat = true},
    {.pin = GPIO_NUM_47, .cmd = CMD_VOLUME_DOWN, .repeat = true},
};
#define BUTTON_COUNT (sizeof(s_buttons) / sizeof(s_buttons[0]))

typedef struct {
  bool pressed;  // last debounced state
  esp_timer_handle_t debounce_timer;
  esp_timer_handle_t repeat_timer;
} button_ctx_t;

static button_ctx_t s_ctx[BUTTON_COUNT];
static QueueHandle_t s_cmd_queue = NULL;
static QueueHandle_t s_edge_queue = NULL;

static bool is_pressed(size_t idx) {
  return gpio_get_level(s_buttons[idx].pin) == 0;
}

static void send_cmd(size_t idx) {
  player_cmd_t cmd = {.type = s_buttons[idx].cmd};
  if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
    ESP_LOGW(TAG, "cmd_queue full, dropped command %d", (int)cmd.type);
  }
}

// Runs in the GPIO ISR: masks the pin (so contact bounce can't flood us) and
// hands the button index to input_task.
static void IRAM_ATTR button_isr(void* arg) {
  size_t idx = (size_t)arg;
  BaseType_t woken = pdFALSE;

  gpio_intr_disable(s_buttons[idx].pin);
  xQueueSendFromISR(s_edge_queue, &idx, &woken);
  if (woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// Debounce window elapsed: the level has settled, so decide if it is a real
// change. Runs in the esp_timer task — must not block.
static void debounce_cb(void* arg) {
  size_t idx = (size_t)arg;
  button_ctx_t* ctx = &s_ctx[idx];
  bool pressed = is_pressed(idx);

  if (pressed != ctx->pressed) {
    ctx->pressed = pressed;
    ESP_LOGD(TAG, "gpio %d %s", (int)s_buttons[idx].pin, pressed ? "pressed" : "released");

    if (pressed) {
      send_cmd(idx);
      if (s_buttons[idx].repeat) {
        esp_timer_start_once(ctx->repeat_timer, (uint64_t)REPEAT_DELAY_MS * US_PER_MS);
      }
    } else {
      esp_timer_stop(ctx->repeat_timer);
    }
  }

  gpio_intr_enable(s_buttons[idx].pin);
  // An edge that arrived after the read above but before the re-enable would
  // be lost: re-check and run another debounce round if the level moved.
  if (is_pressed(idx) != ctx->pressed) {
    esp_timer_start_once(ctx->debounce_timer, (uint64_t)DEBOUNCE_MS * US_PER_MS);
  }
}

static void repeat_cb(void* arg) {
  size_t idx = (size_t)arg;

  if (!s_ctx[idx].pressed) {
    return;
  }
  send_cmd(idx);
  esp_timer_start_once(s_ctx[idx].repeat_timer, (uint64_t)REPEAT_PERIOD_MS * US_PER_MS);
}

static void input_task(void* arg) {
  (void)arg;
  size_t idx;

  for (;;) {
    if (xQueueReceive(s_edge_queue, &idx, portMAX_DELAY) == pdTRUE) {
      esp_timer_start_once(s_ctx[idx].debounce_timer, (uint64_t)DEBOUNCE_MS * US_PER_MS);
    }
  }
}

static esp_err_t create_timer(esp_timer_cb_t cb, size_t idx, const char* name,
                              esp_timer_handle_t* out) {
  const esp_timer_create_args_t args = {
      .callback = cb,
      .arg = (void*)idx,
      .dispatch_method = ESP_TIMER_TASK,
      .name = name,
  };
  return esp_timer_create(&args, out);
}

esp_err_t input_task_init(QueueHandle_t cmd_queue) {
  if (cmd_queue == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  s_cmd_queue = cmd_queue;

  s_edge_queue = xQueueCreate(EDGE_QUEUE_LENGTH, sizeof(size_t));
  if (s_edge_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create edge queue");
    return ESP_FAIL;
  }

  uint64_t pin_mask = 0;
  for (size_t i = 0; i < BUTTON_COUNT; i++) {
    pin_mask |= (1ULL << s_buttons[i].pin);
  }

  const gpio_config_t io_conf = {
      .pin_bit_mask = pin_mask,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
    return err;
  }

  // ESP_INTR_FLAG_IRAM: the BLE stack uses NVS, so flash can be written at run
  // time and the ISR must keep working while the flash cache is disabled.
  err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
    return err;
  }

  // Timers first, ISRs last: an edge must never find a missing timer.
  for (size_t i = 0; i < BUTTON_COUNT; i++) {
    err = create_timer(debounce_cb, i, "btn_debounce", &s_ctx[i].debounce_timer);
    if (err == ESP_OK) {
      err = create_timer(repeat_cb, i, "btn_repeat", &s_ctx[i].repeat_timer);
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
      return err;
    }
  }

  if (xTaskCreate(input_task, "input_task", INPUT_TASK_STACK_SIZE, NULL, INPUT_TASK_PRIORITY,
                  NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create input_task");
    return ESP_FAIL;
  }

  for (size_t i = 0; i < BUTTON_COUNT; i++) {
    err = gpio_isr_handler_add(s_buttons[i].pin, button_isr, (void*)i);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "gpio_isr_handler_add(%d) failed: %s", (int)s_buttons[i].pin,
               esp_err_to_name(err));
      return err;
    }
  }

  ESP_LOGI(TAG, "Initialized: %d buttons, debounce %d ms", (int)BUTTON_COUNT, DEBOUNCE_MS);

  return ESP_OK;
}
