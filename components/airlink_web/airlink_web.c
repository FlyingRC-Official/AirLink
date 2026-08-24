// SPDX-License-Identifier: Apache-2.0
#include "airlink_web.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airlink_can.h"
#include "airlink_config.h"
#include "airlink_core.h"
#include "airlink_diag.h"
#include "airlink_led.h"
#include "airlink_ota.h"
#include "airlink_router.h"
#include "airlink_uart.h"
#include "airlink_wifi.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
static bool s_recovery_mode;
static bool s_read_only_mode;

/* The standalone configurator is opened from file:// on Windows and macOS,
 * which browsers serialize as the opaque "null" origin. Authentication is
 * still required for every API operation. */
static void set_local_file_cors(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "null");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(request, "Access-Control-Max-Age", "600");
}

static bool authorized(httpd_req_t *request)
{
    char supplied[160];
    if (httpd_req_get_hdr_value_str(request, "Authorization", supplied, sizeof(supplied)) != ESP_OK) return false;
    char credentials[96];
    airlink_config_t config; airlink_config_get(&config);
    snprintf(credentials, sizeof(credentials), "admin:%s", config.admin_password);
    unsigned char encoded[144];
    size_t length = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &length,
                              (const unsigned char *)credentials, strlen(credentials)) != 0) return false;
    char expected[160];
    snprintf(expected, sizeof(expected), "Basic %.*s", (int)length, encoded);
    return strlen(supplied) == strlen(expected) && memcmp(supplied, expected, strlen(expected)) == 0;
}

static esp_err_t require_auth(httpd_req_t *request)
{
    if (authorized(request)) return ESP_OK;
    set_local_file_cors(request);
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_hdr(request, "WWW-Authenticate", "Basic realm=\"AirLink\"");
    return httpd_resp_sendstr(request, "Unauthorized");
}

static esp_err_t send_json(httpd_req_t *request, const char *json)
{
    set_local_file_cors(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json);
}

static esp_err_t cors_options_handler(httpd_req_t *request)
{
    set_local_file_cors(request);
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)index_html_gz_start,
                           (ssize_t)(index_html_gz_end - index_html_gz_start));
}

static esp_err_t status_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    airlink_diag_status_t diag; airlink_diag_get(&diag);
    airlink_wifi_status_t wifi; airlink_wifi_get_status(&wifi);
    airlink_uart_health_t uart; airlink_uart_get_health(&uart);
    airlink_config_t active_config; airlink_config_get(&active_config);
    const uint8_t vehicle_endpoint = active_config.bridge_role == AIRLINK_BRIDGE_GROUND ?
                                     AIRLINK_ENDPOINT_ID_BRIDGE : AIRLINK_ENDPOINT_ID_FC_UART;
    airlink_endpoint_stats_t fc; airlink_router_get_stats(vehicle_endpoint, &fc);
    const esp_app_desc_t *app = esp_app_get_description();
    char json[1536];
    snprintf(json, sizeof(json),
        "{\"product\":\"%s\",\"hardware_id\":\"%s\",\"firmware\":\"%s\","
        "\"build_date\":\"%s %s\",\"serial\":\"%s\",\"recovery\":%s,\"read_only\":%s,"
        "\"fc_seen\":%s,\"fc_armed\":%s,\"uptime_s\":%" PRIu32 ","
        "\"free_heap\":%" PRIu32 ",\"min_heap\":%" PRIu32 ",\"boot_count\":%" PRIu32 ","
        "\"reset_reason\":%" PRIu32 ",\"wifi\":{\"ap\":%s,\"sta\":%s,\"rssi\":%d,"
        "\"channel\":%u,\"udp_clients\":%u,\"tcp_clients\":%u,"
        "\"bridge_connected\":%s},"
        "\"uart\":{\"bytes_in\":%" PRIu64 ",\"bytes_out\":%" PRIu64 ",\"frames_in\":%" PRIu32
        ",\"drops\":%" PRIu32 ",\"rx_overflow\":%" PRIu32 "}}",
        AIRLINK_PRODUCT_NAME, AIRLINK_HARDWARE_ID, app->version, app->date, app->time,
        active_config.serial_number, s_recovery_mode ? "true" : "false",
        s_read_only_mode ? "true" : "false",
        diag.fc_seen ? "true" : "false", diag.fc_armed ? "true" : "false",
        diag.uptime_seconds, diag.free_heap, diag.minimum_free_heap, diag.boot_count,
        diag.reset_reason, wifi.ap_started ? "true" : "false", wifi.sta_connected ? "true" : "false",
        wifi.rssi, wifi.channel, wifi.udp_clients, wifi.tcp_clients,
        wifi.bridge_connected ? "true" : "false",
        fc.bytes_in, fc.bytes_out, fc.frames_in, fc.queue_drops, uart.rx_overflow);
    return send_json(request, json);
}

static esp_err_t config_get_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    airlink_config_t config; airlink_config_get(&config);
    const airlink_config_t *c = &config;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "schema_version", c->schema_version);
    cJSON_AddNumberToObject(root, "generation", airlink_config_generation());
    cJSON_AddNumberToObject(root, "route_mode", c->route_mode);
    cJSON_AddNumberToObject(root, "uart_baud", c->uart_baud);
    cJSON_AddNumberToObject(root, "wifi_mode", c->wifi_mode);
    cJSON_AddNumberToObject(root, "wifi_band", c->wifi_band);
    cJSON_AddStringToObject(root, "ap_ssid", c->ap_ssid);
    cJSON_AddStringToObject(root, "sta_ssid", c->sta_ssid);
    cJSON_AddNumberToObject(root, "udp_port", c->udp_port);
    cJSON_AddNumberToObject(root, "tcp_port", c->tcp_port);
    cJSON_AddNumberToObject(root, "usb_mode", c->usb_mode);
    cJSON_AddNumberToObject(root, "bridge_role", c->bridge_role);
    cJSON_AddNumberToObject(root, "can_bitrate", c->can_bitrate);
    cJSON_AddNumberToObject(root, "led_brightness", c->led_brightness);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;
    const esp_err_t err = send_json(request, json);
    cJSON_free(json);
    return err;
}

static bool json_u32(cJSON *root, const char *name, uint32_t *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) return false;
    *value = (uint32_t)item->valuedouble;
    return true;
}

static bool json_string(cJSON *root, const char *name, char *output, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL) return true;
    if (!cJSON_IsString(item) || item->valuestring == NULL || strlen(item->valuestring) >= capacity) return false;
    strlcpy(output, item->valuestring, capacity);
    return true;
}

static esp_err_t config_put_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    if (s_read_only_mode) {
        httpd_resp_set_status(request, "423 Locked");
        return send_json(request, "{\"error\":\"hardware_mismatch_read_only\"}");
    }
    if (airlink_router_fc_armed() && !s_recovery_mode) {
        httpd_resp_set_status(request, "423 Locked");
        return send_json(request, "{\"error\":\"flight_controller_armed\"}");
    }
    if (request->content_len <= 0 || request->content_len >= 2048) return ESP_ERR_INVALID_SIZE;
    char body[2048];
    size_t received_total = 0;
    unsigned timeout_retries = 0;
    while (received_total < (size_t)request->content_len) {
        const int received = httpd_req_recv(request, body + received_total,
                                            (size_t)request->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeout_retries++ < 3) continue;
        if (received <= 0) return ESP_FAIL;
        received_total += (size_t)received;
        timeout_retries = 0;
    }
    body[received_total] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid JSON");
    airlink_config_t c; airlink_config_get(&c);
    uint32_t value = c.route_mode;
    bool valid = json_u32(root, "route_mode", &value); c.route_mode = (airlink_route_mode_t)value;
    valid &= json_u32(root, "uart_baud", &c.uart_baud);
    value = c.wifi_mode; valid &= json_u32(root, "wifi_mode", &value); c.wifi_mode = (airlink_wifi_mode_t)value;
    value = c.wifi_band; valid &= json_u32(root, "wifi_band", &value); c.wifi_band = (airlink_wifi_band_t)value;
    valid &= json_string(root, "ap_ssid", c.ap_ssid, sizeof(c.ap_ssid));
    valid &= json_string(root, "ap_password", c.ap_password, sizeof(c.ap_password));
    valid &= json_string(root, "sta_ssid", c.sta_ssid, sizeof(c.sta_ssid));
    valid &= json_string(root, "sta_password", c.sta_password, sizeof(c.sta_password));
    value = c.udp_port; valid &= json_u32(root, "udp_port", &value) && value <= UINT16_MAX;
    c.udp_port = (uint16_t)value;
    value = c.tcp_port; valid &= json_u32(root, "tcp_port", &value) && value <= UINT16_MAX;
    c.tcp_port = (uint16_t)value;
    value = c.usb_mode; valid &= json_u32(root, "usb_mode", &value); c.usb_mode = (airlink_usb_mode_t)value;
    value = c.bridge_role; valid &= json_u32(root, "bridge_role", &value);
    c.bridge_role = (airlink_bridge_role_t)value;
    c.bridge_enabled = c.bridge_role != AIRLINK_BRIDGE_OFF;
    valid &= json_u32(root, "can_bitrate", &c.can_bitrate);
    value = c.led_brightness; valid &= json_u32(root, "led_brightness", &value) && value <= 100;
    c.led_brightness = (uint8_t)value;
    cJSON_Delete(root);
    if (!valid || !airlink_config_validate(&c) || airlink_config_save(&c) != ESP_OK) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"invalid_configuration\"}");
    }
    return send_json(request, "{\"ok\":true,\"reboot_required\":true}");
}

static esp_err_t clients_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    char json[1024]; airlink_wifi_clients_json(json, sizeof(json));
    return send_json(request, json);
}

static esp_err_t can_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    const size_t capacity = 12U * 1024U;
    char *json = malloc(capacity);
    if (json == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    airlink_can_json(json, capacity);
    const esp_err_t err = send_json(request, json);
    free(json);
    return err;
}

static bool destructive_allowed(void)
{
    return !s_read_only_mode && (!airlink_router_fc_armed() || s_recovery_mode);
}

static esp_err_t reboot_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    if (!destructive_allowed()) { httpd_resp_set_status(request, "423 Locked"); return send_json(request, "{\"error\":\"flight_controller_armed\"}"); }
    send_json(request, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(100)); esp_restart(); return ESP_OK;
}

static esp_err_t reset_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    if (!destructive_allowed()) { httpd_resp_set_status(request, "423 Locked"); return send_json(request, "{\"error\":\"flight_controller_armed\"}"); }
    esp_err_t err = airlink_config_factory_reset();
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "reset failed");
    send_json(request, "{\"ok\":true}"); vTaskDelay(pdMS_TO_TICKS(100)); esp_restart(); return ESP_OK;
}

static esp_err_t ota_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    if (!destructive_allowed()) { httpd_resp_set_status(request, "423 Locked"); return send_json(request, "{\"error\":\"ota_not_allowed\"}"); }
    const esp_err_t err = airlink_ota_http_upload(request);
    if (err != ESP_OK) { httpd_resp_set_status(request, "400 Bad Request"); char json[96]; snprintf(json, sizeof(json), "{\"error\":\"%s\"}", esp_err_to_name(err)); return send_json(request, json); }
    send_json(request, "{\"ok\":true,\"rebooting\":true}"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); return ESP_OK;
}

esp_err_t airlink_web_start(bool recovery_mode, bool read_only_mode)
{
    s_recovery_mode = recovery_mode;
    s_read_only_mode = read_only_mode;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
#define URI(path_, method_, handler_) do { const httpd_uri_t u = {.uri=(path_),.method=(method_),.handler=(handler_)}; if ((err=httpd_register_uri_handler(server,&u))!=ESP_OK) return err; } while(0)
    URI("/", HTTP_GET, index_handler);
    URI("/api/v1/status", HTTP_GET, status_handler);
    URI("/api/v1/config", HTTP_GET, config_get_handler);
    URI("/api/v1/config", HTTP_PUT, config_put_handler);
    URI("/api/v1/clients", HTTP_GET, clients_handler);
    URI("/api/v1/can", HTTP_GET, can_handler);
    URI("/api/v1/actions/reboot", HTTP_POST, reboot_handler);
    URI("/api/v1/actions/factory-reset", HTTP_POST, reset_handler);
    URI("/api/v1/ota", HTTP_POST, ota_handler);
    URI("/api/v1/status", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/config", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/clients", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/can", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/actions/reboot", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/actions/factory-reset", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/ota", HTTP_OPTIONS, cors_options_handler);
#undef URI
    return ESP_OK;
}
