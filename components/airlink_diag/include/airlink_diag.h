// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t boot_count;
    uint32_t reset_reason;
    uint32_t free_heap;
    uint32_t minimum_free_heap;
    uint32_t uptime_seconds;
    bool coredump_present;
    uint32_t coredump_size;
    char previous_boot_stage[24];
    char boot_stage[24];
    bool fc_seen;
    bool fc_armed;
} airlink_diag_status_t;

esp_err_t airlink_diag_init(void);
esp_err_t airlink_diag_mark_boot_stage(const char *stage);
void airlink_diag_get(airlink_diag_status_t *status);
