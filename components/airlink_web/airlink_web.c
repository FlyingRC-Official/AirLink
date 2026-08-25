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
#include "airlink_usb.h"
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
static httpd_handle_t s_server;

/* The standalone configurator is opened from file:// on Windows and macOS,
 * which browsers serialize as the opaque "null" origin. Authentication is
 * still required for every API operation. */
static void set_local_file_cors(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "null");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Headers",
                       "Authorization, Content-Type, X-AirLink-Hardware, X-AirLink-Flash-Bytes, "
                       "X-AirLink-PSRAM-Bytes, X-AirLink-SHA256");
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
    airlink_can_status_t can; airlink_can_get_status(&can);
    airlink_config_t active_config; airlink_config_get(&active_config);
    const uint8_t vehicle_endpoint = active_config.bridge_role == AIRLINK_BRIDGE_GROUND ?
                                     AIRLINK_ENDPOINT_ID_BRIDGE : AIRLINK_ENDPOINT_ID_FC_UART;
    airlink_endpoint_stats_t fc; airlink_router_get_stats(vehicle_endpoint, &fc);
    const uint32_t bridge_tx_queue_drops = wifi.bridge_tx_queue_drops;
    const esp_app_desc_t *app = esp_app_get_description();
    char json[2048];
    snprintf(json, sizeof(json),
        "{\"product\":\"%s\",\"hardware_id\":\"%s\",\"firmware\":\"%s\","
        "\"build_date\":\"%s %s\",\"serial\":\"%s\",\"recovery\":%s,\"read_only\":%s,"
        "\"fc_seen\":%s,\"fc_armed\":%s,\"uptime_s\":%" PRIu32 ","
        "\"free_heap\":%" PRIu32 ",\"min_heap\":%" PRIu32 ",\"boot_count\":%" PRIu32 ","
        "\"reset_reason\":%" PRIu32 ",\"diagnostics\":{\"coredump_present\":%s,"
        "\"coredump_size\":%" PRIu32 ",\"previous_boot_stage\":\"%s\","
        "\"boot_stage\":\"%s\"},\"wifi\":{\"ap\":%s,\"sta\":%s,\"rssi\":%d,"
        "\"channel\":%u,\"udp_clients\":%u,\"tcp_clients\":%u,"
        "\"reconnects\":%" PRIu32 ",\"reconnects_total\":%" PRIu32 ","
        "\"reconnect_streak\":%" PRIu32 ",\"bridge_connected\":%s,"
        "\"bridge_reconnects\":%" PRIu32 ",\"bridge_last_errno\":%" PRIu32
        ",\"tcp_last_errno\":%" PRIu32 ",\"bridge_connects_total\":%" PRIu32
        ",\"tcp_accepts_total\":%" PRIu32 ",\"tcp_disconnects_total\":%" PRIu32
        ",\"tcp_queue_alloc_failures\":%" PRIu32
        ",\"tcp_queue_peak\":%" PRIu32 ",\"tcp_queue_current\":%" PRIu32
        ",\"tcp_send_would_block\":%" PRIu32
        ",\"network_task_loops\":%" PRIu32 ",\"tcp_listener_active\":%s},"
        "\"uart\":{\"bytes_in\":%" PRIu64 ",\"bytes_out\":%" PRIu64 ",\"frames_in\":%" PRIu32
        ",\"drops\":%" PRIu32 ",\"vehicle_queue_drops\":%" PRIu32
        ",\"bridge_tx_queue_drops\":%" PRIu32 ",\"rx_overflow\":%" PRIu32
        ",\"driver_restarts\":%" PRIu32 ",\"high_queue_drops\":%" PRIu32
        ",\"normal_queue_drops\":%" PRIu32 "},"
        "\"can\":{\"rx_frames\":%" PRIu32 ",\"tx_frames\":%" PRIu32
        ",\"bus_errors\":%" PRIu32 ",\"dronecan_errors\":%" PRIu32
        ",\"arbitration_lost\":%" PRIu32 ",\"bus_off\":%" PRIu32
        ",\"nodes\":%u},"
        "\"listeners\":{\"udp_port\":%u,\"tcp_port\":%u},"
        "\"ota\":{\"in_progress\":%s,\"running_partition\":\"%s\","
        "\"image_state\":%" PRId32 "}}",
        AIRLINK_PRODUCT_NAME, AIRLINK_HARDWARE_ID, app->version, app->date, app->time,
        active_config.serial_number, s_recovery_mode ? "true" : "false",
        s_read_only_mode ? "true" : "false",
        diag.fc_seen ? "true" : "false", diag.fc_armed ? "true" : "false",
        diag.uptime_seconds, diag.free_heap, diag.minimum_free_heap, diag.boot_count,
        diag.reset_reason, diag.coredump_present ? "true" : "false", diag.coredump_size,
        diag.previous_boot_stage, diag.boot_stage,
        wifi.ap_started ? "true" : "false", wifi.sta_connected ? "true" : "false",
        wifi.rssi, wifi.channel, wifi.udp_clients, wifi.tcp_clients,
        wifi.reconnects, wifi.reconnects_total, wifi.reconnect_streak,
        wifi.bridge_connected ? "true" : "false", wifi.bridge_reconnects,
        wifi.bridge_last_errno, wifi.tcp_last_errno,
        wifi.bridge_connects_total, wifi.tcp_accepts_total,
        wifi.tcp_disconnects_total, wifi.tcp_queue_alloc_failures,
        wifi.tcp_queue_peak, wifi.tcp_queue_current, wifi.tcp_send_would_block,
        wifi.network_task_loops,
        wifi.tcp_listener_active ? "true" : "false",
        fc.bytes_in, fc.bytes_out, fc.frames_in, fc.queue_drops, fc.queue_drops,
        bridge_tx_queue_drops, uart.rx_overflow, uart.driver_restarts,
        uart.high_queue_drops, uart.normal_queue_drops,
        can.rx_frames, can.tx_frames, can.bus_errors, can.dronecan_errors,
        can.arbitration_lost, can.bus_off_count, can.dronecan_nodes,
        active_config.udp_port, active_config.tcp_port,
        airlink_ota_in_progress() ? "true" : "false",
        airlink_ota_running_partition(), airlink_ota_image_state());
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

static bool receive_config_request(httpd_req_t *request, airlink_config_t *c)
{
    if (request->content_len <= 0 || request->content_len >= 2048) return false;
    char body[2048];
    size_t received_total = 0;
    unsigned timeout_retries = 0;
    while (received_total < (size_t)request->content_len) {
        const int received = httpd_req_recv(request, body + received_total,
                                            (size_t)request->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeout_retries++ < 3) continue;
        if (received <= 0) return false;
        received_total += (size_t)received;
        timeout_retries = 0;
    }
    body[received_total] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return false;
    airlink_config_get(c);
    uint32_t value = c->route_mode;
    bool valid = json_u32(root, "route_mode", &value); c->route_mode = (airlink_route_mode_t)value;
    valid &= json_u32(root, "uart_baud", &c->uart_baud);
    value = c->wifi_mode; valid &= json_u32(root, "wifi_mode", &value); c->wifi_mode = (airlink_wifi_mode_t)value;
    value = c->wifi_band; valid &= json_u32(root, "wifi_band", &value); c->wifi_band = (airlink_wifi_band_t)value;
    valid &= json_string(root, "ap_ssid", c->ap_ssid, sizeof(c->ap_ssid));
    valid &= json_string(root, "ap_password", c->ap_password, sizeof(c->ap_password));
    valid &= json_string(root, "sta_ssid", c->sta_ssid, sizeof(c->sta_ssid));
    valid &= json_string(root, "sta_password", c->sta_password, sizeof(c->sta_password));
    valid &= json_string(root, "admin_password", c->admin_password, sizeof(c->admin_password));
    value = c->udp_port; valid &= json_u32(root, "udp_port", &value) && value <= UINT16_MAX;
    c->udp_port = (uint16_t)value;
    value = c->tcp_port; valid &= json_u32(root, "tcp_port", &value) && value <= UINT16_MAX;
    c->tcp_port = (uint16_t)value;
    value = c->usb_mode; valid &= json_u32(root, "usb_mode", &value); c->usb_mode = (airlink_usb_mode_t)value;
    value = c->bridge_role; valid &= json_u32(root, "bridge_role", &value);
    c->bridge_role = (airlink_bridge_role_t)value;
    c->bridge_enabled = c->bridge_role != AIRLINK_BRIDGE_OFF;
    valid &= json_u32(root, "can_bitrate", &c->can_bitrate);
    value = c->led_brightness; valid &= json_u32(root, "led_brightness", &value) && value <= 100;
    c->led_brightness = (uint8_t)value;
    cJSON_Delete(root);
    return valid && airlink_config_validate(c);
}

static esp_err_t capabilities_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    const esp_app_desc_t *app = esp_app_get_description();
    char json[1024];
    snprintf(json, sizeof(json),
        "{\"api_schema\":\"airlink-api/v1\",\"config_schema\":%u,"
        "\"hardware_id\":\"%s\",\"firmware\":\"%s\",\"features\":{"
        "\"usb_atomic_config\":true,\"config_validate\":true,\"wifi_scan\":true,"
        "\"udp_discovery\":true,\"diagnostics_v2\":true,\"ota_upload\":true,"
        "\"transparent_chunking\":true}}",
        AIRLINK_CONFIG_SCHEMA_VERSION, AIRLINK_HARDWARE_ID, app->version);
    return send_json(request, json);
}

static esp_err_t config_validate_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    airlink_config_t current;
    airlink_config_get(&current);
    airlink_config_t candidate;
    if (!receive_config_request(request, &candidate)) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"valid\":false,\"error\":\"invalid_configuration\"}");
    }
    const bool network_change = current.wifi_mode != candidate.wifi_mode ||
        current.wifi_band != candidate.wifi_band || strcmp(current.ap_ssid, candidate.ap_ssid) != 0 ||
        strcmp(current.ap_password, candidate.ap_password) != 0 ||
        strcmp(current.sta_ssid, candidate.sta_ssid) != 0 ||
        strcmp(current.sta_password, candidate.sta_password) != 0;
    const bool usb_change = current.usb_mode != candidate.usb_mode;
    char json[256];
    snprintf(json, sizeof(json),
        "{\"valid\":true,\"reboot_required\":true,\"warnings\":{"
        "\"network_disconnect\":%s,\"usb_mode_change\":%s,\"role_change\":%s}}",
        network_change ? "true" : "false", usb_change ? "true" : "false",
        current.bridge_role != candidate.bridge_role ? "true" : "false");
    return send_json(request, json);
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
    airlink_config_t c;
    if (!receive_config_request(request, &c) || airlink_config_save(&c) != ESP_OK) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"invalid_configuration\"}");
    }
    return send_json(request, "{\"ok\":true,\"reboot_required\":true}");
}

static esp_err_t wifi_scan_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    if (airlink_router_fc_armed() || airlink_ota_in_progress()) {
        httpd_resp_set_status(request, "423 Locked");
        return send_json(request, "{\"error\":\"wifi_scan_not_allowed\"}");
    }
    const size_t capacity = 8U * 1024U;
    char *json = malloc(capacity);
    if (json == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    const esp_err_t error = airlink_wifi_scan_json(json, capacity);
    if (error != ESP_OK) {
        free(json);
        httpd_resp_set_status(request, "503 Service Unavailable");
        return send_json(request, "{\"error\":\"wifi_scan_failed\"}");
    }
    const esp_err_t response = send_json(request, json);
    free(json);
    return response;
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
    (void)airlink_diag_mark_boot_stage("restarting");
    send_json(request, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(100)); airlink_usb_system_restart();
}

static esp_err_t reset_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    if (!destructive_allowed()) { httpd_resp_set_status(request, "423 Locked"); return send_json(request, "{\"error\":\"flight_controller_armed\"}"); }
    esp_err_t err = airlink_config_factory_reset();
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "reset failed");
    (void)airlink_diag_mark_boot_stage("factory-restarting");
    send_json(request, "{\"ok\":true}"); vTaskDelay(pdMS_TO_TICKS(100)); airlink_usb_system_restart();
}

static esp_err_t ota_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    if (!destructive_allowed()) { httpd_resp_set_status(request, "423 Locked"); return send_json(request, "{\"error\":\"ota_not_allowed\"}"); }
    const esp_err_t err = airlink_ota_http_upload(request);
    if (err != ESP_OK) { httpd_resp_set_status(request, "400 Bad Request"); char json[96]; snprintf(json, sizeof(json), "{\"error\":\"%s\"}", esp_err_to_name(err)); return send_json(request, json); }
    (void)airlink_diag_mark_boot_stage("ota-restarting");
    send_json(request, "{\"ok\":true,\"rebooting\":true}"); vTaskDelay(pdMS_TO_TICKS(200)); airlink_usb_system_restart();
}

esp_err_t airlink_web_start(bool recovery_mode, bool read_only_mode)
{
    s_recovery_mode = recovery_mode;
    s_read_only_mode = read_only_mode;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 26;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
#define URI(path_, method_, handler_) do { const httpd_uri_t u = {.uri=(path_),.method=(method_),.handler=(handler_)}; if ((err=httpd_register_uri_handler(server,&u))!=ESP_OK) goto fail; } while(0)
    URI("/", HTTP_GET, index_handler);
    URI("/api/v1/status", HTTP_GET, status_handler);
    URI("/api/v1/capabilities", HTTP_GET, capabilities_handler);
    URI("/api/v1/config", HTTP_GET, config_get_handler);
    URI("/api/v1/config", HTTP_PUT, config_put_handler);
    URI("/api/v1/config/validate", HTTP_POST, config_validate_handler);
    URI("/api/v1/wifi/scan", HTTP_POST, wifi_scan_handler);
    URI("/api/v1/clients", HTTP_GET, clients_handler);
    URI("/api/v1/can", HTTP_GET, can_handler);
    URI("/api/v1/actions/reboot", HTTP_POST, reboot_handler);
    URI("/api/v1/actions/factory-reset", HTTP_POST, reset_handler);
    URI("/api/v1/ota", HTTP_POST, ota_handler);
    URI("/api/v1/status", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/capabilities", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/config", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/config/validate", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/wifi/scan", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/clients", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/can", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/actions/reboot", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/actions/factory-reset", HTTP_OPTIONS, cors_options_handler);
    URI("/api/v1/ota", HTTP_OPTIONS, cors_options_handler);
#undef URI
    s_server = server;
    return ESP_OK;
fail:
    httpd_stop(server);
    return err;
}

bool airlink_web_ready(void) { return s_server != NULL; }
