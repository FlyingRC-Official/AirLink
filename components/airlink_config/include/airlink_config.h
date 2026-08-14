// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "airlink_core.h"
#include "esp_err.h"

#define AIRLINK_SSID_MAX 32
#define AIRLINK_PASSWORD_MAX 64
#define AIRLINK_SERIAL_MAX 24

typedef struct {
    uint16_t schema_version;
    airlink_route_mode_t route_mode;
    uint32_t uart_baud;
    airlink_wifi_mode_t wifi_mode;
    airlink_wifi_band_t wifi_band;
    char ap_ssid[AIRLINK_SSID_MAX + 1];
    char ap_password[AIRLINK_PASSWORD_MAX + 1];
    char sta_ssid[AIRLINK_SSID_MAX + 1];
    char sta_password[AIRLINK_PASSWORD_MAX + 1];
    uint16_t udp_port;
    uint16_t tcp_port;
    airlink_usb_mode_t usb_mode;
    uint32_t can_bitrate;
    uint8_t led_brightness;
    char serial_number[AIRLINK_SERIAL_MAX + 1];
    char admin_password[AIRLINK_PASSWORD_MAX + 1];
    bool mesh_reserved_enabled;
    uint32_t mesh_reserved_network_id;
} airlink_config_t;

typedef struct {
    airlink_config_t value;
    uint32_t generation;
    bool loaded_defaults;
} airlink_config_snapshot_t;

esp_err_t airlink_config_init(airlink_config_snapshot_t *snapshot);
const airlink_config_t *airlink_config_get(void);
uint32_t airlink_config_generation(void);
esp_err_t airlink_config_save(const airlink_config_t *config);
esp_err_t airlink_config_factory_reset(void);
esp_err_t airlink_config_set_identity(const char *serial, const char *password);
bool airlink_config_validate(const airlink_config_t *config);
