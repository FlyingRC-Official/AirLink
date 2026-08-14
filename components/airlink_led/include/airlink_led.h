// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    AIRLINK_LED_WAIT_WIFI,
    AIRLINK_LED_CONNECTED,
    AIRLINK_LED_MAVLINK,
    AIRLINK_LED_NO_FC,
    AIRLINK_LED_MESH,
    AIRLINK_LED_OTA,
    AIRLINK_LED_ERROR,
} airlink_led_state_t;

esp_err_t airlink_led_start(uint8_t brightness);
void airlink_led_set(airlink_led_state_t state);
void airlink_led_clear_error(void);
