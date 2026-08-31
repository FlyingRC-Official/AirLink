// SPDX-License-Identifier: Apache-2.0
#include "airlink_ota.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "airlink_core.h"
#include "airlink_led.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static const char *TAG = "ota";
static atomic_bool s_services_ready;
static atomic_bool s_in_progress;
static atomic_int_fast64_t s_services_ready_since_us;
static atomic_int_fast64_t s_last_health_us;

typedef struct {
    bool active;
    bool verified;
    const esp_partition_t *target;
    esp_ota_handle_t handle;
    size_t image_size;
    size_t written;
    uint8_t expected_sha256[32];
    char expected_version[32];
    psa_hash_operation_t sha;
} ota_stream_t;

static ota_stream_t s_stream;

#define OTA_CONFIRM_WINDOW_US (30LL * 1000LL * 1000LL)
#define OTA_HEALTH_LEASE_US (3LL * 1000LL * 1000LL)
#define OTA_CONFIRM_DEADLINE_US (60LL * 1000LL * 1000LL)
#define OTA_RECV_TIMEOUT_LIMIT 5U

static bool partition_contains_marker(const esp_partition_t *partition, size_t image_size)
{
    const uint8_t *marker = (const uint8_t *)AIRLINK_IMAGE_HARDWARE_MARKER;
    const size_t marker_len = strlen(AIRLINK_IMAGE_HARDWARE_MARKER);
    uint8_t buffer[1024];
    if (partition == NULL || image_size < marker_len) return false;
    const size_t step = sizeof(buffer) - marker_len + 1U;
    for (size_t offset = 0; offset < image_size; offset += step) {
        const size_t count = image_size - offset < sizeof(buffer) ? image_size - offset : sizeof(buffer);
        if (esp_partition_read(partition, offset, buffer, count) != ESP_OK) return false;
        for (size_t i = 0; i + marker_len <= count; ++i) {
            if (memcmp(&buffer[i], marker, marker_len) == 0) return true;
        }
        if (count < sizeof(buffer)) break;
    }
    return false;
}

static bool header_equals(httpd_req_t *request, const char *name, const char *expected)
{
    char value[96];
    return httpd_req_get_hdr_value_str(request, name, value, sizeof(value)) == ESP_OK &&
           strcmp(value, expected) == 0;
}

static bool parse_sha256(httpd_req_t *request, uint8_t expected[32])
{
    char text[65];
    if (httpd_req_get_hdr_value_str(request, "X-AirLink-SHA256", text, sizeof(text)) != ESP_OK || strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        if (!isxdigit((unsigned char)text[i * 2]) || !isxdigit((unsigned char)text[i * 2 + 1])) return false;
        unsigned value = 0;
        if (sscanf(&text[i * 2], "%2x", &value) != 1) return false;
        expected[i] = (uint8_t)value;
    }
    return true;
}

static void rollback_confirmation_task(void *argument)
{
    (void)argument;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(NULL);
        return;
    }
    const int64_t task_started_us = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time();
        const int64_t ready_since = atomic_load(&s_services_ready_since_us);
        const int64_t last_health = atomic_load(&s_last_health_us);
        const bool ready = atomic_load(&s_services_ready);
        if (ready && ready_since > 0 && now - ready_since >= OTA_CONFIRM_WINDOW_US &&
            last_health >= ready_since && now - last_health <= OTA_HEALTH_LEASE_US) {
            ESP_LOGI(TAG, "services remained healthy; confirming OTA image");
            const esp_err_t confirm_err = esp_ota_mark_app_valid_cancel_rollback();
            if (confirm_err == ESP_OK) break;
            ESP_LOGE(TAG, "could not confirm OTA image: %s", esp_err_to_name(confirm_err));
            airlink_ota_services_ready(false);
        }
        if (now - task_started_us >= OTA_CONFIRM_DEADLINE_US) {
            ESP_LOGE(TAG, "services did not sustain health; rolling back");
            const esp_err_t rollback_err = esp_ota_mark_app_invalid_rollback_and_reboot();
            ESP_LOGE(TAG, "OTA rollback request failed: %s; retrying",
                     esp_err_to_name(rollback_err));
        }
    }
    vTaskDelete(NULL);
}

esp_err_t airlink_ota_init(void)
{
    return xTaskCreate(rollback_confirmation_task, "ota_confirm", 3072, NULL, 8, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void airlink_ota_services_ready(bool ready)
{
    const bool was_ready = atomic_exchange(&s_services_ready, ready);
    if (ready && !was_ready) {
        atomic_store(&s_services_ready_since_us, esp_timer_get_time());
        atomic_store(&s_last_health_us, 0);
    } else if (!ready && was_ready) {
        atomic_store(&s_services_ready_since_us, 0);
        atomic_store(&s_last_health_us, 0);
    }
}

void airlink_ota_health_heartbeat(bool healthy)
{
    if (healthy && atomic_load(&s_services_ready)) atomic_store(&s_last_health_us, esp_timer_get_time());
}

bool airlink_ota_in_progress(void) { return atomic_load(&s_in_progress); }

const char *airlink_ota_running_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running != NULL ? running->label : "unknown";
}

int32_t airlink_ota_image_state(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK ?
           (int32_t)state : -1;
}

bool airlink_ota_stream_active(void) { return s_stream.active; }
bool airlink_ota_stream_staged(void) { return s_stream.verified; }

size_t airlink_ota_stream_remaining(void)
{
    return s_stream.active && s_stream.written < s_stream.image_size ?
           s_stream.image_size - s_stream.written : 0U;
}

void airlink_ota_stream_abort(void)
{
    if (!s_stream.active && !s_stream.verified) return;
    if (s_stream.active) {
        psa_hash_abort(&s_stream.sha);
        esp_ota_abort(s_stream.handle);
    }
    memset(&s_stream, 0, sizeof(s_stream));
    atomic_store(&s_in_progress, false);
    airlink_led_set(AIRLINK_LED_ERROR);
}

esp_err_t airlink_ota_stream_begin(size_t image_size, const uint8_t expected_sha256[32])
{
    return airlink_ota_stream_begin_versioned(image_size, expected_sha256, NULL);
}

esp_err_t airlink_ota_stream_begin_versioned(size_t image_size,
                                             const uint8_t expected_sha256[32],
                                             const char *expected_version)
{
    if (image_size == 0U || expected_sha256 == NULL || s_stream.active ||
        s_stream.verified || (expected_version != NULL &&
        (expected_version[0] == '\0' || strlen(expected_version) >=
         sizeof(s_stream.expected_version)))) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || image_size > target->size) return ESP_ERR_INVALID_SIZE;
    bool expected_idle = false;
    if (!atomic_compare_exchange_strong(&s_in_progress, &expected_idle, true)) return ESP_ERR_INVALID_STATE;

    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.target = target;
    s_stream.image_size = image_size;
    memcpy(s_stream.expected_sha256, expected_sha256, sizeof(s_stream.expected_sha256));
    if (expected_version != NULL) {
        strlcpy(s_stream.expected_version, expected_version, sizeof(s_stream.expected_version));
    }
    esp_err_t err = esp_ota_begin(target, image_size, &s_stream.handle);
    if (err == ESP_OK) {
        s_stream.sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
        if (psa_hash_setup(&s_stream.sha, PSA_ALG_SHA_256) != PSA_SUCCESS) err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        if (s_stream.handle != 0) esp_ota_abort(s_stream.handle);
        memset(&s_stream, 0, sizeof(s_stream));
        atomic_store(&s_in_progress, false);
        return err;
    }
    s_stream.active = true;
    airlink_led_set(AIRLINK_LED_OTA);
    return ESP_OK;
}

esp_err_t airlink_ota_stream_write(const void *data, size_t length)
{
    if (!s_stream.active || data == NULL || length == 0U ||
        length > airlink_ota_stream_remaining()) return ESP_ERR_INVALID_ARG;
    if (psa_hash_update(&s_stream.sha, data, length) != PSA_SUCCESS) {
        airlink_ota_stream_abort();
        return ESP_FAIL;
    }
    const esp_err_t err = esp_ota_write(s_stream.handle, data, length);
    if (err != ESP_OK) {
        airlink_ota_stream_abort();
        return err;
    }
    s_stream.written += length;
    return ESP_OK;
}

esp_err_t airlink_ota_stream_verify(void)
{
    if (!s_stream.active || s_stream.written != s_stream.image_size) return ESP_ERR_INVALID_STATE;
    uint8_t actual_sha[32];
    size_t actual_sha_length = 0;
    esp_err_t err = psa_hash_finish(&s_stream.sha, actual_sha, sizeof(actual_sha),
                                    &actual_sha_length) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK && (actual_sha_length != sizeof(actual_sha) ||
        memcmp(actual_sha, s_stream.expected_sha256, sizeof(actual_sha)) != 0)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) err = esp_ota_end(s_stream.handle);
    else esp_ota_abort(s_stream.handle);
    if (err == ESP_OK) {
        esp_app_desc_t descriptor;
        err = esp_ota_get_partition_description(s_stream.target, &descriptor);
        if (err == ESP_OK && strcmp(descriptor.project_name, "airlink") != 0) {
            err = ESP_ERR_INVALID_VERSION;
        }
        if (err == ESP_OK && s_stream.expected_version[0] != '\0' &&
            strcmp(descriptor.version, s_stream.expected_version) != 0) {
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    if (err == ESP_OK && !partition_contains_marker(s_stream.target, s_stream.image_size)) {
        err = ESP_ERR_INVALID_VERSION;
    }
    if (err == ESP_OK) {
        s_stream.active = false;
        s_stream.verified = true;
        s_stream.handle = 0;
    } else {
        memset(&s_stream, 0, sizeof(s_stream));
        atomic_store(&s_in_progress, false);
        airlink_led_set(AIRLINK_LED_ERROR);
    }
    return err;
}

esp_err_t airlink_ota_stream_activate(void)
{
    if (!s_stream.verified || s_stream.target == NULL) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = esp_ota_set_boot_partition(s_stream.target);
    memset(&s_stream, 0, sizeof(s_stream));
    atomic_store(&s_in_progress, false);
    if (err != ESP_OK) airlink_led_set(AIRLINK_LED_ERROR);
    return err;
}

esp_err_t airlink_ota_stream_finish(void)
{
    esp_err_t err = airlink_ota_stream_verify();
    return err == ESP_OK ? airlink_ota_stream_activate() : err;
}

esp_err_t airlink_ota_http_upload(httpd_req_t *request)
{
    if (request == NULL || request->content_len <= 0) return ESP_ERR_INVALID_ARG;
    if (!header_equals(request, "X-AirLink-Hardware", AIRLINK_HARDWARE_ID) ||
        !header_equals(request, "X-AirLink-Flash-Bytes", "8388608") ||
        !header_equals(request, "X-AirLink-PSRAM-Bytes", "8388608")) return ESP_ERR_INVALID_VERSION;
    uint8_t expected_sha[32];
    if (!parse_sha256(request, expected_sha)) return ESP_ERR_INVALID_ARG;

    ESP_RETURN_ON_ERROR(airlink_ota_stream_begin((size_t)request->content_len, expected_sha),
                        TAG, "OTA stream begin");
    uint8_t buffer[2048];
    int remaining = request->content_len;
    unsigned consecutive_timeouts = 0;
    esp_err_t err = ESP_OK;
    while (remaining > 0) {
        const int wanted = remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer);
        const int received = httpd_req_recv(request, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && consecutive_timeouts++ < OTA_RECV_TIMEOUT_LIMIT) continue;
        if (received <= 0) { err = ESP_FAIL; break; }
        consecutive_timeouts = 0;
        err = airlink_ota_stream_write(buffer, (size_t)received);
        if (err != ESP_OK) break;
        remaining -= received;
    }
    if (err != ESP_OK || remaining != 0) {
        airlink_ota_stream_abort();
        return err == ESP_OK ? ESP_FAIL : err;
    }
    return airlink_ota_stream_finish();
}
