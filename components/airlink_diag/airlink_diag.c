// SPDX-License-Identifier: Apache-2.0
#include "airlink_diag.h"

#include "airlink_router.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static uint32_t s_boot_count;

esp_err_t airlink_diag_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("air_diag", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (nvs_get_u32(nvs, "boot_count", &s_boot_count) == ESP_ERR_NVS_NOT_FOUND) s_boot_count = 0;
    s_boot_count++;
    err = nvs_set_u32(nvs, "boot_count", s_boot_count);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "last_reset", (uint32_t)esp_reset_reason());
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

void airlink_diag_get(airlink_diag_status_t *status)
{
    if (status == NULL) return;
    *status = (airlink_diag_status_t){
        .boot_count = s_boot_count,
        .reset_reason = (uint32_t)esp_reset_reason(),
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = esp_get_minimum_free_heap_size(),
        .uptime_seconds = (uint32_t)(esp_timer_get_time() / INT64_C(1000000)),
        .fc_seen = airlink_router_fc_seen(),
        .fc_armed = airlink_router_fc_armed(),
    };
}
