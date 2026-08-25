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
    /* Last socket errno observed by the ground bridge and air TCP server.
     * Zero means an orderly peer close. These fields are diagnostic and are
     * intentionally retained for the remainder of the boot. */
    uint32_t bridge_last_errno;
    uint32_t tcp_last_errno;
    uint32_t bridge_connects_total;
    uint32_t tcp_accepts_total;
    uint32_t tcp_disconnects_total;
    uint32_t tcp_queue_alloc_failures;
    uint32_t tcp_queue_peak;
    uint32_t tcp_queue_current;
    uint32_t tcp_send_would_block;
    uint32_t network_task_loops;
    bool tcp_listener_active;
    /* Monotonic for this boot and includes closed/replaced TCP sessions. */
    uint32_t bridge_tx_queue_drops;
} airlink_wifi_status_t;

esp_err_t airlink_wifi_start(const airlink_config_t *config);
void airlink_wifi_get_status(airlink_wifi_status_t *status);
size_t airlink_wifi_clients_json(char *output, size_t capacity);
esp_err_t airlink_wifi_scan_json(char *output, size_t capacity);
void airlink_wifi_prepare_restart(void);
