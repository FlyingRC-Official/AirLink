// SPDX-License-Identifier: Apache-2.0
#include "airlink_api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define API_SESSION_COUNT 4U
#define API_CHALLENGE_TIMEOUT_US INT64_C(30000000)
#define API_SESSION_IDLE_TIMEOUT_US INT64_C(600000000)
#define AIRLINK_AP_ADDRESS UINT32_C(0xc0a80401)

typedef struct {
    bool used;
    bool authenticated;
    uint8_t id[16];
    uint8_t client_nonce[32];
    uint8_t server_nonce[32];
    uint8_t key[32];
    uint64_t counter;
    int64_t last_seen_us;
} api_session_t;

static bool s_recovery_mode;
static bool s_read_only_mode;
static httpd_handle_t s_server;
static api_session_t s_sessions[API_SESSION_COUNT];

static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

static bool decode_hex(const char *text, uint8_t *output, size_t length)
{
    if (text == NULL || strlen(text) != length * 2U) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char high = (unsigned char)text[i * 2U];
        const unsigned char low = (unsigned char)text[i * 2U + 1U];
        const int high_value = high >= '0' && high <= '9' ? high - '0' :
                               high >= 'a' && high <= 'f' ? high - 'a' + 10 :
                               high >= 'A' && high <= 'F' ? high - 'A' + 10 : -1;
        const int low_value = low >= '0' && low <= '9' ? low - '0' :
                              low >= 'a' && low <= 'f' ? low - 'a' + 10 :
                              low >= 'A' && low <= 'F' ? low - 'A' + 10 : -1;
        if (high_value < 0 || low_value < 0) return false;
        output[i] = (uint8_t)((high_value << 4U) | low_value);
    }
    return true;
}

static void encode_hex(const uint8_t *input, size_t length, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        output[i * 2U] = digits[input[i] >> 4U];
        output[i * 2U + 1U] = digits[input[i] & 0x0fU];
    }
    output[length * 2U] = '\0';
}

static bool hmac_sha256(const uint8_t *key, size_t key_length,
                        const uint8_t *data, size_t data_length, uint8_t output[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != NULL && mbedtls_md_hmac(info, key, key_length,
                                           data, data_length, output) == 0;
}

static bool body_hash_matches(httpd_req_t *request, const uint8_t *body, size_t length)
{
    char supplied_text[65];
    uint8_t supplied[32], actual[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (httpd_req_get_hdr_value_str(request, "X-AirLink-Body-SHA256", supplied_text,
                                    sizeof(supplied_text)) != ESP_OK ||
        !decode_hex(supplied_text, supplied, sizeof(supplied)) ||
        info == NULL ||
        mbedtls_md(info, body != NULL ? body : (const uint8_t *)"", length, actual) != 0) return false;
    return constant_time_equal(supplied, actual, sizeof(actual));
}

static bool request_arrived_on_ap(httpd_req_t *request)
{
    struct sockaddr_in local = {0};
    socklen_t length = sizeof(local);
    const int socket = httpd_req_to_sockfd(request);
    return socket >= 0 && getsockname(socket, (struct sockaddr *)&local, &length) == 0 &&
           local.sin_family == AF_INET && ntohl(local.sin_addr.s_addr) == AIRLINK_AP_ADDRESS;
}

static api_session_t *find_session(const char *encoded_id, bool authenticated)
{
    uint8_t id[16];
    if (!decode_hex(encoded_id, id, sizeof(id))) return NULL;
    const int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < API_SESSION_COUNT; ++i) {
        api_session_t *session = &s_sessions[i];
        if (!session->used) continue;
        const int64_t timeout = session->authenticated ? API_SESSION_IDLE_TIMEOUT_US :
                                                        API_CHALLENGE_TIMEOUT_US;
        if (now - session->last_seen_us > timeout) {
            memset(session, 0, sizeof(*session));
            continue;
        }
        if (session->authenticated == authenticated &&
            constant_time_equal(session->id, id, sizeof(id))) return session;
    }
    return NULL;
}

static const char *method_name(httpd_method_t method)
{
    if (method == HTTP_GET) return "GET";
    if (method == HTTP_POST) return "POST";
    if (method == HTTP_PUT) return "PUT";
    if (method == HTTP_DELETE) return "DELETE";
    return "UNKNOWN";
}

static bool authorized(httpd_req_t *request)
{
    if (!request_arrived_on_ap(request)) return false;
    char session_id[33], counter_text[24], body_hash[65], supplied_signature[65];
    if (httpd_req_get_hdr_value_str(request, "X-AirLink-Session", session_id,
                                    sizeof(session_id)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "X-AirLink-Counter", counter_text,
                                    sizeof(counter_text)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "X-AirLink-Body-SHA256", body_hash,
                                    sizeof(body_hash)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "X-AirLink-Signature", supplied_signature,
                                    sizeof(supplied_signature)) != ESP_OK) return false;
    uint8_t decoded_hash[32], signature[32];
    if (!decode_hex(body_hash, decoded_hash, sizeof(decoded_hash)) ||
        !decode_hex(supplied_signature, signature, sizeof(signature))) return false;
    if (request->content_len == 0 && !body_hash_matches(request, NULL, 0)) return false;
    char *end = NULL;
    errno = 0;
    const unsigned long long parsed_counter = strtoull(counter_text, &end, 10);
    if (errno != 0 || end == counter_text || *end != '\0') return false;
    api_session_t *session = find_session(session_id, true);
    if (session == NULL || parsed_counter <= session->counter) return false;
    char canonical[384];
    const int canonical_length = snprintf(canonical, sizeof(canonical), "%s\n%s\n%s\n%s",
                                          method_name(request->method), request->uri,
                                          counter_text, body_hash);
    uint8_t expected[32];
    if (canonical_length <= 0 || (size_t)canonical_length >= sizeof(canonical) ||
        !hmac_sha256(session->key, sizeof(session->key),
                     (const uint8_t *)canonical, (size_t)canonical_length, expected) ||
        !constant_time_equal(signature, expected, sizeof(expected))) return false;
    session->counter = (uint64_t)parsed_counter;
    session->last_seen_us = esp_timer_get_time();
    return true;
}

static esp_err_t require_auth(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, request_arrived_on_ap(request) ?
                                  "401 Unauthorized" : "403 Forbidden");
    return httpd_resp_sendstr(request, request_arrived_on_ap(request) ?
                             "{\"error\":\"authentication_required\"}" :
                             "{\"error\":\"management_ap_only\"}");
}

static esp_err_t send_json(httpd_req_t *request, const char *json)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json);
}

static esp_err_t challenge_handler(httpd_req_t *request)
{
    if (!request_arrived_on_ap(request)) return require_auth(request);
    char client_nonce_text[65];
    uint8_t client_nonce[32];
    if (httpd_req_get_hdr_value_str(request, "X-AirLink-Client-Nonce", client_nonce_text,
                                    sizeof(client_nonce_text)) != ESP_OK ||
        !decode_hex(client_nonce_text, client_nonce, sizeof(client_nonce))) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"invalid_client_nonce\"}");
    }
    const int64_t now = esp_timer_get_time();
    api_session_t *slot = NULL;
    for (size_t i = 0; i < API_SESSION_COUNT; ++i) {
        if (!s_sessions[i].used || now - s_sessions[i].last_seen_us >
            (s_sessions[i].authenticated ? API_SESSION_IDLE_TIMEOUT_US :
                                           API_CHALLENGE_TIMEOUT_US)) {
            slot = &s_sessions[i];
            break;
        }
    }
    if (slot == NULL) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return send_json(request, "{\"error\":\"session_capacity\"}");
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    memcpy(slot->client_nonce, client_nonce, sizeof(slot->client_nonce));
    esp_fill_random(slot->id, sizeof(slot->id));
    esp_fill_random(slot->server_nonce, sizeof(slot->server_nonce));
    slot->last_seen_us = now;
    char id[33], server_nonce[65], json[192];
    encode_hex(slot->id, sizeof(slot->id), id);
    encode_hex(slot->server_nonce, sizeof(slot->server_nonce), server_nonce);
    snprintf(json, sizeof(json),
             "{\"protocol\":2,\"session\":\"%s\",\"server_nonce\":\"%s\",\"expires_s\":30}",
             id, server_nonce);
    return send_json(request, json);
}

static esp_err_t session_auth_handler(httpd_req_t *request)
{
    if (!request_arrived_on_ap(request)) return require_auth(request);
    char session_id[33], proof_text[65];
    uint8_t proof[32];
    if (httpd_req_get_hdr_value_str(request, "X-AirLink-Session", session_id,
                                    sizeof(session_id)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "X-AirLink-Proof", proof_text,
                                    sizeof(proof_text)) != ESP_OK ||
        !decode_hex(proof_text, proof, sizeof(proof))) return require_auth(request);
    api_session_t *session = find_session(session_id, false);
    if (session == NULL) return require_auth(request);
    uint8_t material[15 + 16 + 32 + 32];
    memcpy(material, "AIRLINK-AUTH-V2", 15);
    memcpy(material + 15, session->id, 16);
    memcpy(material + 31, session->client_nonce, 32);
    memcpy(material + 63, session->server_nonce, 32);
    airlink_config_t config;
    airlink_config_get(&config);
    uint8_t expected[32];
    if (!hmac_sha256((const uint8_t *)config.admin_password,
                     strlen(config.admin_password), material, sizeof(material), expected) ||
        !constant_time_equal(proof, expected, sizeof(expected))) {
        memset(session, 0, sizeof(*session));
        return require_auth(request);
    }
    uint8_t session_material[18 + 32 + 32];
    memcpy(session_material, "AIRLINK-SESSION-V2", 18);
    memcpy(session_material + 18, session->client_nonce, 32);
    memcpy(session_material + 50, session->server_nonce, 32);
    hmac_sha256((const uint8_t *)config.admin_password, strlen(config.admin_password),
                session_material, sizeof(session_material), session->key);
    session->authenticated = true;
    session->counter = 0;
    session->last_seen_us = esp_timer_get_time();
    uint8_t server_proof[32];
    hmac_sha256(session->key, sizeof(session->key),
                (const uint8_t *)"AIRLINK-SERVER-V2", 17, server_proof);
    char server_proof_text[65], json[128];
    encode_hex(server_proof, sizeof(server_proof), server_proof_text);
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"idle_timeout_s\":600,\"server_proof\":\"%s\"}",
             server_proof_text);
    return send_json(request, json);
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
        AIRLINK_ENDPOINT_ID_BRIDGE :
        (active_config.fc_transport == AIRLINK_FC_TRANSPORT_DRONECAN ?
         AIRLINK_ENDPOINT_ID_FC_CAN : AIRLINK_ENDPOINT_ID_FC_UART);
    airlink_endpoint_stats_t fc; airlink_router_get_stats(vehicle_endpoint, &fc);
    const uint32_t bridge_tx_queue_drops = wifi.bridge_tx_queue_drops;
    const esp_app_desc_t *app = esp_app_get_description();
    char json[3072];
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
        ",\"nodes\":%u,\"tunnel_rx_bytes\":%" PRIu64
        ",\"tunnel_tx_bytes\":%" PRIu64 ",\"tunnel_rx_transfers\":%" PRIu32
        ",\"tunnel_tx_transfers\":%" PRIu32 ",\"tunnel_drops\":%" PRIu32
        ",\"high_queue_drops\":%" PRIu32 ",\"normal_queue_drops\":%" PRIu32
        ",\"keepalives\":%" PRIu32 ",\"peer_online\":%s},"
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
        can.tunnel_rx_bytes, can.tunnel_tx_bytes, can.tunnel_rx_transfers,
        can.tunnel_tx_transfers, can.tunnel_drops, can.high_queue_drops,
        can.normal_queue_drops, can.keepalives, can.peer_online ? "true" : "false",
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
    cJSON_AddNumberToObject(root, "fc_transport", c->fc_transport);
    cJSON_AddNumberToObject(root, "can_node_id", c->can_node_id);
    cJSON_AddNumberToObject(root, "can_remote_node_id", c->can_remote_node_id);
    cJSON_AddNumberToObject(root, "can_serial_id", c->can_serial_id);
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
    if (!body_hash_matches(request, (const uint8_t *)body, received_total)) return false;
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
    value = c->fc_transport; valid &= json_u32(root, "fc_transport", &value);
    c->fc_transport = (airlink_fc_transport_t)value;
    value = c->can_node_id; valid &= json_u32(root, "can_node_id", &value) && value <= UINT8_MAX;
    c->can_node_id = (uint8_t)value;
    value = c->can_remote_node_id; valid &= json_u32(root, "can_remote_node_id", &value) && value <= UINT8_MAX;
    c->can_remote_node_id = (uint8_t)value;
    value = (uint8_t)c->can_serial_id; valid &= json_u32(root, "can_serial_id", &value) && value <= INT8_MAX;
    c->can_serial_id = (int8_t)value;
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
        "{\"api_schema\":\"airlink-api/v2\",\"protocol\":2,\"config_schema\":%u,"
        "\"hardware_id\":\"%s\",\"firmware\":\"%s\",\"features\":{"
        "\"usb_atomic_config\":true,\"config_validate\":true,\"wifi_scan\":true,"
        "\"udp_discovery\":true,\"diagnostics_v2\":true,\"ota_upload\":true,"
        "\"transparent_chunking\":true,\"dronecan_tunnel\":true}}",
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

static esp_err_t diagnostics_handler(httpd_req_t *request)
{
    if (!authorized(request)) return require_auth(request);
    airlink_diag_status_t diag;
    airlink_diag_get(&diag);
    char json[512];
    snprintf(json, sizeof(json),
             "{\"uptime_s\":%" PRIu32 ",\"free_heap\":%" PRIu32
             ",\"minimum_free_heap\":%" PRIu32 ",\"boot_count\":%" PRIu32
             ",\"reset_reason\":%" PRIu32 ",\"coredump_present\":%s"
             ",\"coredump_size\":%" PRIu32 ",\"previous_boot_stage\":\"%s\""
             ",\"boot_stage\":\"%s\"}",
             diag.uptime_seconds, diag.free_heap, diag.minimum_free_heap,
             diag.boot_count, diag.reset_reason,
             diag.coredump_present ? "true" : "false", diag.coredump_size,
             diag.previous_boot_stage, diag.boot_stage);
    return send_json(request, json);
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
    char body_hash_text[65], image_hash_text[65];
    uint8_t body_hash[32], image_hash[32];
    if (httpd_req_get_hdr_value_str(request, "X-AirLink-Body-SHA256", body_hash_text,
                                    sizeof(body_hash_text)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "X-AirLink-SHA256", image_hash_text,
                                    sizeof(image_hash_text)) != ESP_OK ||
        !decode_hex(body_hash_text, body_hash, sizeof(body_hash)) ||
        !decode_hex(image_hash_text, image_hash, sizeof(image_hash)) ||
        !constant_time_equal(body_hash, image_hash, sizeof(body_hash))) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"ota_body_hash_mismatch\"}");
    }
    const esp_err_t err = airlink_ota_http_upload(request);
    if (err != ESP_OK) { httpd_resp_set_status(request, "400 Bad Request"); char json[96]; snprintf(json, sizeof(json), "{\"error\":\"%s\"}", esp_err_to_name(err)); return send_json(request, json); }
    (void)airlink_diag_mark_boot_stage("ota-restarting");
    send_json(request, "{\"ok\":true,\"rebooting\":true}"); vTaskDelay(pdMS_TO_TICKS(200)); airlink_usb_system_restart();
}

esp_err_t airlink_api_start(bool recovery_mode, bool read_only_mode)
{
    s_recovery_mode = recovery_mode;
    s_read_only_mode = read_only_mode;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
#define URI(path_, method_, handler_) do { const httpd_uri_t u = {.uri=(path_),.method=(method_),.handler=(handler_)}; if ((err=httpd_register_uri_handler(server,&u))!=ESP_OK) goto fail; } while(0)
    URI("/api/v2/session/challenge", HTTP_POST, challenge_handler);
    URI("/api/v2/session/auth", HTTP_POST, session_auth_handler);
    URI("/api/v2/capabilities", HTTP_GET, capabilities_handler);
    URI("/api/v2/status", HTTP_GET, status_handler);
    URI("/api/v2/config", HTTP_GET, config_get_handler);
    URI("/api/v2/config", HTTP_PUT, config_put_handler);
    URI("/api/v2/config/validate", HTTP_POST, config_validate_handler);
    URI("/api/v2/wifi/scan", HTTP_POST, wifi_scan_handler);
    URI("/api/v2/clients", HTTP_GET, clients_handler);
    URI("/api/v2/can", HTTP_GET, can_handler);
    URI("/api/v2/diagnostics", HTTP_GET, diagnostics_handler);
    URI("/api/v2/reboot", HTTP_POST, reboot_handler);
    URI("/api/v2/reset", HTTP_POST, reset_handler);
    URI("/api/v2/ota", HTTP_POST, ota_handler);
#undef URI
    s_server = server;
    return ESP_OK;
fail:
    httpd_stop(server);
    return err;
}

bool airlink_api_ready(void) { return s_server != NULL; }
