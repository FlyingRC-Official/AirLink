// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t airlink_ota_init(void);
void airlink_ota_services_ready(bool ready);
void airlink_ota_health_heartbeat(bool healthy);
esp_err_t airlink_ota_http_upload(httpd_req_t *request);
esp_err_t airlink_ota_stream_begin(size_t image_size, const uint8_t expected_sha256[32]);
esp_err_t airlink_ota_stream_write(const void *data, size_t length);
esp_err_t airlink_ota_stream_finish(void);
void airlink_ota_stream_abort(void);
bool airlink_ota_stream_active(void);
size_t airlink_ota_stream_remaining(void);
bool airlink_ota_in_progress(void);
const char *airlink_ota_running_partition(void);
int32_t airlink_ota_image_state(void);
