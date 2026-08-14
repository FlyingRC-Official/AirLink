// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t airlink_ota_init(void);
void airlink_ota_services_ready(bool ready);
esp_err_t airlink_ota_http_upload(httpd_req_t *request);
bool airlink_ota_in_progress(void);
