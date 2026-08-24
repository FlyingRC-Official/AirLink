// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRLINK_PRODUCT_NAME "AirLink C5 Mesh V1"
#define AIRLINK_HARDWARE_ID "airlink-c5-mesh-v1"
#define AIRLINK_IMAGE_HARDWARE_MARKER \
    "AIRLINK_HW=airlink-c5-mesh-v1;FLASH=8388608;PSRAM=8388608"
#define AIRLINK_CONFIG_SCHEMA_VERSION 1U
#define AIRLINK_MAX_FRAME_SIZE 280U
#define AIRLINK_MAX_UDP_CLIENTS 8U
#define AIRLINK_MAX_TCP_CLIENTS 2U
#define AIRLINK_ENDPOINT_ID_FC_UART 1U
#define AIRLINK_ENDPOINT_ID_USB 2U
#define AIRLINK_ENDPOINT_ID_BRIDGE 3U
#define AIRLINK_ENDPOINT_ID_UDP_BASE 16U
#define AIRLINK_ENDPOINT_ID_TCP_BASE 32U

typedef enum {
    AIRLINK_ROUTE_MAVLINK = 0,
    AIRLINK_ROUTE_TRANSPARENT = 1,
} airlink_route_mode_t;

typedef enum {
    AIRLINK_WIFI_AP = 0,
    AIRLINK_WIFI_STA = 1,
    AIRLINK_WIFI_APSTA = 2,
} airlink_wifi_mode_t;

typedef enum {
    AIRLINK_WIFI_BAND_AUTO = 0,
    AIRLINK_WIFI_BAND_2G = 1,
    AIRLINK_WIFI_BAND_5G = 2,
} airlink_wifi_band_t;

typedef enum {
    AIRLINK_USB_LOG_CLI = 0,
    AIRLINK_USB_MAVLINK = 1,
} airlink_usb_mode_t;

typedef enum {
    AIRLINK_BRIDGE_OFF = 0,
    AIRLINK_BRIDGE_AIR = 1,
    AIRLINK_BRIDGE_GROUND = 2,
} airlink_bridge_role_t;

typedef enum {
    AIRLINK_ENDPOINT_UART = 0,
    AIRLINK_ENDPOINT_UDP = 1,
    AIRLINK_ENDPOINT_TCP = 2,
    AIRLINK_ENDPOINT_USB = 3,
    AIRLINK_ENDPOINT_BRIDGE = 4,
} airlink_endpoint_type_t;

typedef struct {
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t parse_errors;
    uint32_t queue_drops;
    uint32_t queue_peak;
    int64_t last_activity_us;
} airlink_endpoint_stats_t;

uint32_t airlink_crc32(const void *data, size_t len);
const char *airlink_image_hardware_marker(void);
