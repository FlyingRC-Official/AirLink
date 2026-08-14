// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include "airlink_core.h"
#include "esp_err.h"

typedef void (*airlink_usb_cli_handler_t)(const char *line);

esp_err_t airlink_usb_start(airlink_usb_mode_t mode);
void airlink_usb_set_cli_handler(airlink_usb_cli_handler_t handler);
esp_err_t airlink_usb_write_cli(const char *text);
bool airlink_usb_connected(void);
