// SPDX-License-Identifier: Apache-2.0
#include "airlink_diag.h"

#include "airlink_router.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include <string.h>

static uint32_t s_boot_count;
static char s_previous_boot_stage[24] = "unknown";
static char s_boot_stage[24] = "pre-diagnostics";
static bool s_initialized;

static esp_err_t read_previous_stage(nvs_handle_t nvs)
{
    size_t length = sizeof(s_previous_boot_stage);
    const esp_err_t err = nvs_get_str(nvs, "boot_stage", s_previous_boot_stage, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(s_previous_boot_stage, "unknown", sizeof(s_previous_boot_stage));
        return ESP_OK;
    }
    return err;
}

esp_err_t airlink_diag_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("air_diag", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = read_previous_stage(nvs);
    if (err != ESP_OK) goto out;
    if (nvs_get_u32(nvs, "boot_count", &s_boot_count) == ESP_ERR_NVS_NOT_FOUND) s_boot_count = 0;
    if (s_boot_count != UINT32_MAX) s_boot_count++;
    err = nvs_set_u32(nvs, "boot_count", s_boot_count);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "last_reset", (uint32_t)esp_reset_reason());
    if (err == ESP_OK) err = nvs_set_str(nvs, "boot_stage", "diagnostics");
    if (err == ESP_OK) err = nvs_commit(nvs);
out:
    nvs_close(nvs);
    if (err == ESP_OK) {
        strlcpy(s_boot_stage, "diagnostics", sizeof(s_boot_stage));
        s_initialized = true;
    }
    return err;
}

esp_err_t airlink_diag_mark_boot_stage(const char *stage)
{
    if (!s_initialized || stage == NULL || stage[0] == '\0' ||
        strnlen(stage, sizeof(s_boot_stage)) >= sizeof(s_boot_stage)) return ESP_ERR_INVALID_ARG;
    if (strcmp(stage, s_boot_stage) == 0) return ESP_OK;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("air_diag", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "boot_stage", stage);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) strlcpy(s_boot_stage, stage, sizeof(s_boot_stage));
    return err;
}

void airlink_diag_get(airlink_diag_status_t *status)
{
    if (status == NULL) return;
    size_t coredump_address = 0;
    size_t coredump_size = 0;
    const bool coredump_present = esp_core_dump_image_get(&coredump_address, &coredump_size) == ESP_OK;
    (void)coredump_address;
    *status = (airlink_diag_status_t){
        .boot_count = s_boot_count,
        .reset_reason = (uint32_t)esp_reset_reason(),
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = esp_get_minimum_free_heap_size(),
        .uptime_seconds = (uint32_t)(esp_timer_get_time() / INT64_C(1000000)),
        .coredump_present = coredump_present,
        .coredump_size = coredump_present && coredump_size <= UINT32_MAX ?
                         (uint32_t)coredump_size : 0U,
        .fc_seen = airlink_router_fc_seen(),
        .fc_armed = airlink_router_fc_armed(),
    };
    strlcpy(status->previous_boot_stage, s_previous_boot_stage,
            sizeof(status->previous_boot_stage));
    strlcpy(status->boot_stage, s_boot_stage, sizeof(status->boot_stage));
}
