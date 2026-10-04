#include "ble_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nvs_flash.h"
#include "player_types.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char* TAG = "ble_task";

#define BLE_TASK_STACK_SIZE 4096
// Above audio_task (5) so GATT/HCI events are not starved by long codec feeds,
// below the Bluetooth controller task (configMAX_PRIORITIES - 2).
#define BLE_TASK_PRIORITY 6

#define BLE_DEVICE_NAME "MP3 Player"

// Custom 128-bit UUIDs, little-endian byte order as required by NimBLE.
// Service: 5f1c2a40-8b3e-4d7a-9c61-2e4f0a7b3d15
// Command characteristic: 5f1c2a41-8b3e-4d7a-9c61-2e4f0a7b3d15
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x15, 0x3d, 0x7b, 0x0a, 0x4f, 0x2e, 0x61, 0x9c, 0x7a, 0x4d, 0x3e, 0x8b, 0x40, 0x2a, 0x1c, 0x5f);
static const ble_uuid128_t s_cmd_chr_uuid = BLE_UUID128_INIT(
    0x15, 0x3d, 0x7b, 0x0a, 0x4f, 0x2e, 0x61, 0x9c, 0x7a, 0x4d, 0x3e, 0x8b, 0x41, 0x2a, 0x1c, 0x5f);

static QueueHandle_t s_cmd_queue = NULL;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static void start_advertising(void);

static bool byte_to_cmd(uint8_t byte, cmd_type_t* out) {
  switch (byte) {
    case 'P':
      *out = CMD_PLAY_PAUSE;
      return true;
    case 'N':
      *out = CMD_NEXT;
      return true;
    case 'B':
      *out = CMD_PREV;
      return true;
    case '+':
      *out = CMD_VOLUME_UP;
      return true;
    case '-':
      *out = CMD_VOLUME_DOWN;
      return true;
    case 'M':
      *out = CMD_TOGGLE_MUTE;
      return true;
    default:
      return false;
  }
}

static void send_cmd(cmd_type_t type) {
  player_cmd_t cmd = {.type = type};
  if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
    ESP_LOGW(TAG, "cmd_queue full, dropped command %d", (int)type);
  }
}

// Runs in the NimBLE host task — must not block.
static int cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt* ctxt, void* arg) {
  (void)conn_handle;
  (void)attr_handle;
  (void)arg;

  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
  if (len != 1) {
    ESP_LOGW(TAG, "Rejected write of %u bytes, expected 1", (unsigned)len);
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  uint8_t byte = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, &byte, sizeof(byte), NULL) != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  cmd_type_t type;
  if (!byte_to_cmd(byte, &type)) {
    ESP_LOGW(TAG, "Unknown command byte 0x%02x", (unsigned)byte);
    return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
  }

  ESP_LOGI(TAG, "Command '%c'", (char)byte);
  send_cmd(type);
  return 0;
}

// No *_ENC / *_AUTHEN flags: the characteristic is writable without pairing.
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_cmd_chr_uuid.u,
                    .access_cb = cmd_access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                },
                {0},  // end of characteristics
            },
    },
    {0},  // end of services
};

static int gap_event_cb(struct ble_gap_event* event, void* arg) {
  (void)arg;

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        ESP_LOGI(TAG, "Central connected, handle %d", (int)event->connect.conn_handle);
      } else {
        ESP_LOGW(TAG, "Connection failed, status %d", event->connect.status);
        start_advertising();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "Central disconnected, reason %d", event->disconnect.reason);
      start_advertising();
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      ESP_LOGW(TAG, "Advertising completed, restarting");
      start_advertising();
      break;
    default:
      break;
  }
  return 0;
}

static void start_advertising(void) {
  // The 128-bit UUID plus flags use 21 of the 31 advertising bytes, so the
  // device name goes into the scan response.
  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = (ble_uuid128_t*)&s_svc_uuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
    return;
  }

  struct ble_hs_adv_fields rsp_fields;
  memset(&rsp_fields, 0, sizeof(rsp_fields));
  const char* name = ble_svc_gap_device_name();
  rsp_fields.name = (const uint8_t*)name;
  rsp_fields.name_len = (uint8_t)strlen(name);
  rsp_fields.name_is_complete = 1;

  rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
    return;
  }

  struct ble_gap_adv_params adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    return;
  }
  ESP_LOGI(TAG, "Advertising as \"%s\"", BLE_DEVICE_NAME);
}

// Host and controller are in sync: the address is known, advertising can start.
static void on_sync(void) {
  int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
    return;
  }
  start_advertising();
}

static void on_reset(int reason) {
  ESP_LOGW(TAG, "Host reset, reason %d", reason);
}

// The NimBLE host task: blocks inside nimble_port_run() until nimble_port_stop().
static void ble_task(void* arg) {
  (void)arg;
  nimble_port_run();
  vTaskDelete(NULL);
}

static esp_err_t init_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS needs erasing: %s", esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }
  return err;
}

esp_err_t ble_task_init(QueueHandle_t cmd_queue) {
  if (cmd_queue == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  s_cmd_queue = cmd_queue;

  ESP_LOGI(TAG, "Free heap before BLE init: %u bytes", (unsigned)esp_get_free_heap_size());

  esp_err_t err = init_nvs();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
    return err;
  }

  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;

  ble_svc_gap_init();
  ble_svc_gatt_init();

  int rc = ble_gatts_count_cfg(s_gatt_svcs);
  if (rc == 0) {
    rc = ble_gatts_add_svcs(s_gatt_svcs);
  }
  if (rc != 0) {
    ESP_LOGE(TAG, "GATT table setup failed: %d", rc);
    return ESP_FAIL;
  }

  rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
    return ESP_FAIL;
  }

  // Callbacks and services are in place; now the host may start running.
  if (xTaskCreate(ble_task, "ble_task", BLE_TASK_STACK_SIZE, NULL, BLE_TASK_PRIORITY, NULL) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create ble_task");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Initialized, free heap: %u bytes", (unsigned)esp_get_free_heap_size());

  return ESP_OK;
}
