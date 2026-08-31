// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include "airlink_core.h"
#include "esp_err.h"

void airlink_usb_reset_guard_enable(void);
void airlink_usb_system_restart(void) __attribute__((noreturn));
esp_err_t airlink_usb_start(airlink_usb_mode_t mode);
esp_err_t airlink_usb_write_cli(const char *text);
bool airlink_usb_connected(void);
bool airlink_usb_ready(void);
