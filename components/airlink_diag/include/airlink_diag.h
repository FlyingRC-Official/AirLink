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
    bool fc_seen;
    bool fc_armed;
} airlink_diag_status_t;

esp_err_t airlink_diag_init(void);
void airlink_diag_get(airlink_diag_status_t *status);
