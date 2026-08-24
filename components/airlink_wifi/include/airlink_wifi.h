// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "airlink_config.h"
#include "esp_err.h"

typedef struct {
    bool ap_started;
    bool sta_connected;
    int8_t rssi;
    uint8_t channel;
    uint8_t udp_clients;
    uint8_t tcp_clients;
    /* reconnects is retained as a compatibility alias of reconnects_total. */
    uint32_t reconnects;
    uint32_t reconnects_total;
    uint32_t reconnect_streak;
    bool bridge_connected;
    uint32_t bridge_reconnects;
} airlink_wifi_status_t;

esp_err_t airlink_wifi_start(const airlink_config_t *config);
void airlink_wifi_get_status(airlink_wifi_status_t *status);
size_t airlink_wifi_clients_json(char *output, size_t capacity);
esp_err_t airlink_wifi_scan_json(char *output, size_t capacity);
