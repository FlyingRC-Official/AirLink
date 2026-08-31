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
    uint64_t tunnel_rx_bytes;
    uint64_t tunnel_tx_bytes;
    uint32_t tunnel_rx_transfers;
    uint32_t tunnel_tx_transfers;
    uint32_t tunnel_drops;
    uint32_t high_queue_drops;
    uint32_t normal_queue_drops;
    uint32_t keepalives;
    bool peer_online;
} airlink_can_status_t;

typedef struct {
    uint32_t bitrate;
    uint32_t virtual_baud;
    uint8_t local_node_id;
    uint8_t remote_node_id;
    int8_t serial_id;
    bool tunnel_enabled;
} airlink_can_options_t;

esp_err_t airlink_can_start(const airlink_can_options_t *options);
bool airlink_can_ready(void);
void airlink_can_get_status(airlink_can_status_t *status);
size_t airlink_can_json(char *output, size_t capacity);
