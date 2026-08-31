// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "airlink_core.h"
#include "esp_err.h"

typedef esp_err_t (*airlink_router_send_fn)(const uint8_t *data, size_t length,
                                            bool high_priority, void *context);

typedef struct {
    uint8_t id;
    airlink_endpoint_type_t type;
    airlink_endpoint_direction_t direction;
    airlink_router_send_fn send;
    void *context;
    const char *name;
} airlink_router_endpoint_t;

esp_err_t airlink_router_init(airlink_route_mode_t mode);
esp_err_t airlink_router_register(const airlink_router_endpoint_t *endpoint);
void airlink_router_unregister(uint8_t endpoint_id);
esp_err_t airlink_router_ingest(uint8_t endpoint_id, const uint8_t *data, size_t length);
void airlink_router_set_mode(airlink_route_mode_t mode);
bool airlink_router_fc_armed(void);
bool airlink_router_fc_seen(void);
bool airlink_router_fc_system_id(uint8_t *system_id);
void airlink_router_get_stats(uint8_t endpoint_id, airlink_endpoint_stats_t *stats);
