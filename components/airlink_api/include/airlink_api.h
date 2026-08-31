// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t airlink_api_start(bool recovery_mode, bool read_only_mode);
bool airlink_api_ready(void);
