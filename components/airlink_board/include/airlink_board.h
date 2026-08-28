// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define AIRLINK_GPIO_CAN_TX 0
#define AIRLINK_GPIO_CAN_RX 1
#define AIRLINK_GPIO_CAN_SILENT 8
#define AIRLINK_GPIO_WS2812 4
#define AIRLINK_GPIO_ACT_LED 9
#define AIRLINK_GPIO_UART0_TX 11
#define AIRLINK_GPIO_UART0_RX 12
#define AIRLINK_GPIO_USB_DN 13
#define AIRLINK_GPIO_USB_DP 14
#define AIRLINK_GPIO_FC_TX 23
#define AIRLINK_GPIO_FC_RX 24
#define AIRLINK_GPIO_BOOT 28

typedef struct {
    bool chip_ok;
    bool flash_ok;
    bool psram_ok;
    bool recovery_requested;
    uint32_t flash_bytes;
    uint32_t psram_bytes;
} airlink_board_probe_t;

esp_err_t airlink_board_init(airlink_board_probe_t *probe);
bool airlink_board_boot_pressed(void);
void airlink_board_act_pulse(void);
