// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t bus_errors;
    uint32_t dronecan_errors;
    uint32_t arbitration_lost;
    uint32_t bus_off_count;
    uint16_t tx_error_count;
    uint16_t rx_error_count;
    uint8_t dronecan_nodes;
} airlink_can_status_t;

esp_err_t airlink_can_start(uint32_t bitrate, bool factory_mode);
esp_err_t airlink_can_factory_transmit(uint32_t id, bool extended,
                                       const uint8_t *data, size_t length);
esp_err_t airlink_can_factory_set_bitrate(uint32_t bitrate);
void airlink_can_get_status(airlink_can_status_t *status);
size_t airlink_can_json(char *output, size_t capacity);
