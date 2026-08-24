// SPDX-License-Identifier: Apache-2.0
#include "airlink_factory_ble.h"

#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include <string.h>
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "factory_ble";
static uint8_t s_address_type;
static volatile bool s_advertising;

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) s_advertising = false;
    return 0;
}

static void advertise(void)
{
    static const char name[] = "AirLink-C5-FACTORY";
    static const uint8_t manufacturer[] = {0x46, 0x52, 0x43, 0x01}; /* FRC + format version */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = sizeof(name) - 1U;
    fields.name_is_complete = true;
    fields.mfg_data = (uint8_t *)manufacturer;
    fields.mfg_data_len = sizeof(manufacturer);
    if (ble_gap_adv_set_fields(&fields) != 0) return;
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(s_address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    s_advertising = rc == 0;
    if (rc != 0) ESP_LOGE(TAG, "advertising start failed: %d", rc);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &s_address_type) == 0) advertise();
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t airlink_factory_ble_start(void)
{
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool airlink_factory_ble_advertising(void) { return s_advertising; }
#else
esp_err_t airlink_factory_ble_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool airlink_factory_ble_advertising(void) { return false; }
#endif
