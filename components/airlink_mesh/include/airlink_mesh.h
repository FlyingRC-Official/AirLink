// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "airlink_mesh_config.h"
#include "esp_err.h"

typedef enum {
    AIRLINK_MESH_ISOLATION_NONE = 0,
    AIRLINK_MESH_ISOLATION_NOT_APPROVED = 1,
    AIRLINK_MESH_ISOLATION_IDENTITY_CHANGED = 2,
    AIRLINK_MESH_ISOLATION_DUPLICATE_SYSTEM_ID = 3,
    AIRLINK_MESH_ISOLATION_REMOVED = 4,
} airlink_mesh_isolation_reason_t;

typedef struct {
    bool started;
    bool connected;
    bool is_root;
    bool rootless;
    uint8_t layer;
    uint8_t parent[6];
    int8_t rssi;
    uint8_t discovered_nodes;
    uint8_t approved_online_nodes;
    uint32_t reorganizations;
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t auth_failures;
    uint32_t replay_drops;
    uint32_t queue_drops;
} airlink_mesh_status_t;

typedef struct {
    bool present;
    bool approved;
    bool online;
    bool status_known;
    bool armed_known;
    bool armed;
    bool system_id_known;
    uint8_t system_id;
    uint8_t sta_mac[6];
    uint8_t parent[6];
    uint8_t layer;
    int8_t rssi;
    char serial[AIRLINK_MESH_SERIAL_SIZE + 1U];
    char firmware[24];
    uint32_t uptime_s;
    uint32_t queue_drops;
    uint32_t mesh_generation;
    uint8_t ota_state;
    uint8_t ota_progress;
    int32_t ota_image_state;
    airlink_mesh_isolation_reason_t isolation_reason;
} airlink_mesh_node_info_t;

esp_err_t airlink_mesh_start(const airlink_mesh_config_t *mesh_config,
                             const airlink_config_t *base_config);
bool airlink_mesh_ready(void);
bool airlink_mesh_global_safe(void);
void airlink_mesh_get_status(airlink_mesh_status_t *status);
size_t airlink_mesh_get_nodes(airlink_mesh_node_info_t *nodes, size_t capacity);
size_t airlink_mesh_nodes_json(char *output, size_t capacity);
esp_err_t airlink_mesh_approve_node(const char *serial, const uint8_t sta_mac[6]);
esp_err_t airlink_mesh_remove_node(const char *serial, const uint8_t sta_mac[6]);
esp_err_t airlink_mesh_update_network(const airlink_mesh_config_t *config,
                                      uint32_t timeout_ms);
esp_err_t airlink_mesh_node_config_get(const uint8_t sta_mac[6],
                                       airlink_config_t *config, uint32_t timeout_ms);
esp_err_t airlink_mesh_node_config_set(const uint8_t sta_mac[6],
                                       const airlink_config_t *config,
                                       uint32_t timeout_ms);
esp_err_t airlink_mesh_reboot_node(const uint8_t sta_mac[6]);
esp_err_t airlink_mesh_ota_begin(size_t image_size, const uint8_t sha256[32],
                                 const char *expected_version,
                                 const uint8_t (*targets)[6], size_t target_count,
                                 bool include_root);
esp_err_t airlink_mesh_ota_write(const void *data, size_t length);
esp_err_t airlink_mesh_ota_finish(void);
void airlink_mesh_ota_abort(void);
bool airlink_mesh_ota_active(void);
void airlink_mesh_prepare_restart(void);
