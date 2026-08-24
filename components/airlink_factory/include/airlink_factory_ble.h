// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t airlink_factory_ble_start(void);
bool airlink_factory_ble_advertising(void);
