// SPDX-License-Identifier: Apache-2.0
#include "airlink_ota.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "airlink_core.h"
#include "airlink_led.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static const char *TAG = "ota";
static volatile bool s_services_ready;
static volatile bool s_in_progress;

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
    /* Keep the image pending for a real stability window.  A panic or watchdog
     * reset during this period leaves it unconfirmed so the bootloader can
     * roll back instead of accepting a merely-started image. */
    for (unsigned second = 0; second < 30; ++second) vTaskDelay(pdMS_TO_TICKS(1000));
    if (s_services_ready) {
        ESP_LOGI(TAG, "services healthy; confirming OTA image");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGE(TAG, "services not healthy; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelete(NULL);
}

esp_err_t airlink_ota_init(void)
{
    return xTaskCreate(rollback_confirmation_task, "ota_confirm", 3072, NULL, 8, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void airlink_ota_services_ready(bool ready) { s_services_ready = ready; }
bool airlink_ota_in_progress(void) { return s_in_progress; }

esp_err_t airlink_ota_http_upload(httpd_req_t *request)
{
    if (request == NULL || request->content_len <= 0) return ESP_ERR_INVALID_ARG;
    if (!header_equals(request, "X-AirLink-Hardware", AIRLINK_HARDWARE_ID) ||
        !header_equals(request, "X-AirLink-Flash-Bytes", "8388608") ||
        !header_equals(request, "X-AirLink-PSRAM-Bytes", "8388608")) return ESP_ERR_INVALID_VERSION;
    uint8_t expected_sha[32];
    if (!parse_sha256(request, expected_sha)) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || request->content_len > target->size) return ESP_ERR_INVALID_SIZE;
    esp_ota_handle_t handle;
    ESP_RETURN_ON_ERROR(esp_ota_begin(target, request->content_len, &handle), TAG, "OTA begin");
    s_in_progress = true;
    airlink_led_set(AIRLINK_LED_OTA);
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        esp_ota_abort(handle);
        s_in_progress = false;
        return ESP_FAIL;
    }
    uint8_t buffer[2048];
    int remaining = request->content_len;
    esp_err_t err = ESP_OK;
    while (remaining > 0) {
        const int wanted = remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer);
        const int received = httpd_req_recv(request, (char *)buffer, wanted);
        if (received <= 0) { err = ESP_FAIL; break; }
        if (psa_hash_update(&sha, buffer, (size_t)received) != PSA_SUCCESS) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(handle, buffer, (size_t)received);
        if (err != ESP_OK) break;
        remaining -= received;
    }
    uint8_t actual_sha[32];
    size_t actual_sha_length = 0;
    if (err == ESP_OK && psa_hash_finish(&sha, actual_sha, sizeof(actual_sha), &actual_sha_length) != PSA_SUCCESS) {
        err = ESP_FAIL;
    } else if (err != ESP_OK) {
        psa_hash_abort(&sha);
    }
    if (err == ESP_OK && actual_sha_length != sizeof(actual_sha)) err = ESP_FAIL;
    if (err == ESP_OK && memcmp(actual_sha, expected_sha, sizeof(actual_sha)) != 0) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) err = esp_ota_end(handle); else esp_ota_abort(handle);
    if (err == ESP_OK) {
        esp_app_desc_t descriptor;
        err = esp_ota_get_partition_description(target, &descriptor);
        if (err == ESP_OK && strcmp(descriptor.project_name, "airlink") != 0) err = ESP_ERR_INVALID_VERSION;
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
    s_in_progress = false;
    if (err != ESP_OK) airlink_led_set(AIRLINK_LED_ERROR);
    return err;
}
