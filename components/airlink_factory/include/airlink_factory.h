// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "airlink_board.h"
#include "esp_err.h"

esp_err_t airlink_factory_start(const airlink_board_probe_t *probe, bool enabled);
