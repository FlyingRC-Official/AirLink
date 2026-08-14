// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t rx_overflow;
    uint32_t driver_restarts;
    uint32_t high_queue_drops;
    uint32_t normal_queue_drops;
} airlink_uart_health_t;

esp_err_t airlink_uart_start(uint32_t baud);
esp_err_t airlink_uart_set_baud(uint32_t baud);
void airlink_uart_get_health(airlink_uart_health_t *health);
esp_err_t airlink_uart_factory_loopback(size_t length, uint32_t *errors);
