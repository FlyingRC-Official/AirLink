// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "airlink_config.h"
#include "esp_err.h"

#define AIRLINK_MESH_CONFIG_SCHEMA 1U
#define AIRLINK_MESH_NETWORK_ID_SIZE 6U
#define AIRLINK_MESH_FLEET_KEY_SIZE 32U
#define AIRLINK_MESH_MAX_NODES 8U
#define AIRLINK_MESH_SERIAL_SIZE AIRLINK_SERIAL_MAX

typedef enum {
    AIRLINK_MESH_ROLE_OFF = 0,
    AIRLINK_MESH_ROLE_AIR = 1,
    AIRLINK_MESH_ROLE_GROUND_ROOT = 2,
} airlink_mesh_role_t;

typedef enum {
    AIRLINK_MESH_BAND_2G = 1,
    AIRLINK_MESH_BAND_5G_RESERVED = 2,
} airlink_mesh_band_t;

typedef struct {
    uint16_t schema_version;
    uint8_t configured;
    uint8_t role;
    uint8_t network_id[AIRLINK_MESH_NETWORK_ID_SIZE];
    uint8_t fleet_key[AIRLINK_MESH_FLEET_KEY_SIZE];
    uint8_t band;
    uint8_t channel;
    char country[3];
    uint8_t max_nodes;
    uint8_t max_hops;
    uint8_t reserved[6];
} airlink_mesh_config_t;

typedef struct {
    char serial[AIRLINK_MESH_SERIAL_SIZE + 1U];
    uint8_t sta_mac[6];
} airlink_mesh_approval_t;

typedef struct {
    uint8_t count;
    airlink_mesh_approval_t entries[AIRLINK_MESH_MAX_NODES];
} airlink_mesh_approval_list_t;

typedef struct {
    airlink_mesh_config_t value;
    uint32_t generation;
    bool loaded_defaults;
    bool pending_present;
} airlink_mesh_config_snapshot_t;

esp_err_t airlink_mesh_config_init(airlink_mesh_config_snapshot_t *snapshot);
void airlink_mesh_config_get(airlink_mesh_config_t *config);
uint32_t airlink_mesh_config_generation(void);
bool airlink_mesh_config_validate(const airlink_mesh_config_t *config,
                                  const airlink_config_t *base_config);
bool airlink_mesh_channel_allowed(const char country[3], uint8_t channel);
esp_err_t airlink_mesh_config_create(airlink_mesh_role_t role,
                                     airlink_mesh_config_t *created);
esp_err_t airlink_mesh_config_save(const airlink_mesh_config_t *config);
esp_err_t airlink_mesh_config_stage(const airlink_mesh_config_t *config,
                                    uint32_t generation);
esp_err_t airlink_mesh_config_commit_staged(void);
esp_err_t airlink_mesh_config_abort_staged(void);
esp_err_t airlink_mesh_config_reset(void);
esp_err_t airlink_mesh_config_export_json(const airlink_mesh_config_t *config,
                                          bool include_secret, char *output,
                                          size_t capacity);
esp_err_t airlink_mesh_config_import_json(const char *json,
                                          airlink_mesh_role_t role,
                                          airlink_mesh_config_t *config);

esp_err_t airlink_mesh_approval_get(airlink_mesh_approval_list_t *list);
bool airlink_mesh_approval_contains(const airlink_mesh_approval_list_t *list,
                                    const char *serial, const uint8_t sta_mac[6]);
esp_err_t airlink_mesh_approval_add(const char *serial, const uint8_t sta_mac[6]);
esp_err_t airlink_mesh_approval_remove(const char *serial, const uint8_t sta_mac[6]);
