// SPDX-License-Identifier: Apache-2.0
#include "airlink_usb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airlink_config.h"
#include "airlink_can.h"
#include "airlink_core.h"
#include "airlink_diag.h"
#include "airlink_ota.h"
#include "airlink_router.h"
#include "airlink_stream.h"
#include "airlink_uart.h"
#include "airlink_wifi.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

#define USB_QUEUE_DEPTH 64
#define USB_TASK_PRIORITY 19
#define USB_TASK_STACK_SIZE 8192
#define USB_CLI_ESCAPE "+++AIRLINK-CLI\r\n"
#define USB_RX_CHUNK 256U
typedef struct { uint16_t length; uint8_t data[AIRLINK_MAX_FRAME_SIZE]; } usb_packet_t;

static airlink_usb_mode_t s_mode;
static QueueHandle_t s_tx_queue;
static TaskHandle_t s_usb_task;
static airlink_usb_cli_handler_t s_cli_handler;
static vprintf_like_t s_console_vprintf;
static bool s_config_transaction_active;
static airlink_config_t s_staged_config;
static int64_t s_config_transaction_deadline_us;

#define CONFIG_TRANSACTION_TIMEOUT_US INT64_C(30000000)
#define USB_ESCAPE_TIMEOUT_US INT64_C(250000)
#define USB_OTA_TIMEOUT_US INT64_C(30000000)
#define USB_DOWNLOAD_WINDOW_US INT64_C(15000000)

static int64_t s_usb_ota_deadline_us;
static int64_t s_usb_download_deadline_us;

static void usb_reset_protection(bool enabled)
{
    if (enabled) {
        REG_SET_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                    USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS);
    } else {
        REG_CLR_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                    USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS);
    }
}

void airlink_usb_reset_guard_enable(void) { usb_reset_protection(true); }

static bool parse_sha256_text(const char *text, uint8_t digest[32])
{
    if (text == NULL || strlen(text) != 64U) return false;
    for (size_t i = 0; i < 32U; ++i) {
        char pair[3] = {text[i * 2U], text[i * 2U + 1U], '\0'};
        char *end = NULL;
        errno = 0;
        const unsigned long value = strtoul(pair, &end, 16);
        if (errno != 0 || end == NULL || *end != '\0') return false;
        digest[i] = (uint8_t)value;
    }
    return true;
}

static const char *route_mode_name(airlink_route_mode_t mode)
{
    return mode == AIRLINK_ROUTE_TRANSPARENT ? "transparent" : "mavlink";
}

static const char *wifi_mode_name(airlink_wifi_mode_t mode)
{
    if (mode == AIRLINK_WIFI_STA) return "sta";
    if (mode == AIRLINK_WIFI_APSTA) return "apsta";
    return "ap";
}

static const char *wifi_band_name(airlink_wifi_band_t band)
{
    if (band == AIRLINK_WIFI_BAND_2G) return "2g";
    if (band == AIRLINK_WIFI_BAND_5G) return "5g";
    return "auto";
}

static const char *usb_mode_name(airlink_usb_mode_t mode)
{
    return mode == AIRLINK_USB_MAVLINK ? "mavlink" : "log";
}

static const char *bridge_role_name(airlink_bridge_role_t role)
{
    if (role == AIRLINK_BRIDGE_AIR) return "air";
    if (role == AIRLINK_BRIDGE_GROUND) return "ground";
    return "off";
}

static void status_show(void)
{
    airlink_wifi_status_t wifi = {0};
    airlink_uart_health_t uart = {0};
    airlink_can_status_t can = {0};
    airlink_diag_status_t diag = {0};
    airlink_endpoint_stats_t fc = {0};
    airlink_endpoint_stats_t usb = {0};
    airlink_endpoint_stats_t bridge_tcp = {0};
    airlink_config_t config;
    airlink_config_get(&config);
    airlink_wifi_get_status(&wifi);
    airlink_uart_get_health(&uart);
    airlink_can_get_status(&can);
    airlink_diag_get(&diag);
    const uint8_t vehicle_endpoint = config.bridge_role == AIRLINK_BRIDGE_GROUND ?
                                     AIRLINK_ENDPOINT_ID_BRIDGE : AIRLINK_ENDPOINT_ID_FC_UART;
    airlink_router_get_stats(vehicle_endpoint, &fc);
    airlink_router_get_stats(AIRLINK_ENDPOINT_ID_USB, &usb);
    if (config.bridge_role == AIRLINK_BRIDGE_AIR) {
        for (uint8_t i = 0; i < AIRLINK_MAX_TCP_CLIENTS; ++i) {
            airlink_endpoint_stats_t client = {0};
            airlink_router_get_stats((uint8_t)(AIRLINK_ENDPOINT_ID_TCP_BASE + i), &client);
            bridge_tcp.queue_drops = UINT32_MAX - bridge_tcp.queue_drops < client.queue_drops ?
                                     UINT32_MAX : bridge_tcp.queue_drops + client.queue_drops;
        }
    } else if (config.bridge_role == AIRLINK_BRIDGE_GROUND) {
        bridge_tcp.queue_drops = fc.queue_drops;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char output[2048];
    snprintf(output, sizeof(output),
             "OK status\r\n"
             "firmware=%s\r\n"
             "hardware_id=%s\r\n"
             "serial_number=%s\r\n"
             "uptime_seconds=%" PRIu32 "\r\n"
             "free_heap=%" PRIu32 "\r\n"
             "minimum_free_heap=%" PRIu32 "\r\n"
             "boot_count=%" PRIu32 "\r\n"
             "reset_reason=%" PRIu32 "\r\n"
             "fc_seen=%u\r\n"
             "fc_armed=%u\r\n"
             "fc_bytes_in=%" PRIu64 "\r\n"
             "fc_bytes_out=%" PRIu64 "\r\n"
             "fc_frames_in=%" PRIu32 "\r\n"
             "fc_frames_out=%" PRIu32 "\r\n"
             "fc_parse_errors=%" PRIu32 "\r\n"
             "wifi_ap_started=%u\r\n"
             "wifi_sta_connected=%u\r\n"
             "wifi_rssi=%d\r\n"
             "wifi_channel=%u\r\n"
             "udp_clients=%u\r\n"
             "tcp_clients=%u\r\n"
             "wifi_reconnects=%" PRIu32 "\r\n"
             "wifi_reconnects_total=%" PRIu32 "\r\n"
             "wifi_reconnect_streak=%" PRIu32 "\r\n"
             "bridge_role=%s\r\n"
             "bridge_connected=%u\r\n"
             "bridge_reconnects=%" PRIu32 "\r\n"
             "usb_frames_out=%" PRIu32 "\r\n"
             "usb_queue_drops=%" PRIu32 "\r\n"
             "bridge_tcp_queue_drops=%" PRIu32 "\r\n"
             "vehicle_queue_drops=%" PRIu32 "\r\n"
             "bridge_tx_queue_drops=%" PRIu32 "\r\n"
             "uart_rx_overflow=%" PRIu32 "\r\n"
             "uart_driver_restarts=%" PRIu32 "\r\n"
             "uart_high_queue_drops=%" PRIu32 "\r\n"
             "uart_normal_queue_drops=%" PRIu32 "\r\n"
             "can_rx_frames=%" PRIu32 "\r\n"
             "can_tx_frames=%" PRIu32 "\r\n"
             "can_bus_errors=%" PRIu32 "\r\n"
             "can_dronecan_errors=%" PRIu32 "\r\n"
             "can_arbitration_lost=%" PRIu32 "\r\n"
             "can_bus_off=%" PRIu32 "\r\n"
             "can_dronecan_nodes=%u\r\n"
             "udp_listener_port=%u\r\n"
             "tcp_listener_port=%u\r\n"
             "ota_in_progress=%u\r\n"
             "ota_running_partition=%s\r\n"
             "ota_image_state=%" PRId32 "\r\n",
             app->version, AIRLINK_HARDWARE_ID, config.serial_number,
             diag.uptime_seconds, diag.free_heap, diag.minimum_free_heap,
             diag.boot_count, diag.reset_reason,
             airlink_router_fc_seen(), airlink_router_fc_armed(),
             fc.bytes_in, fc.bytes_out, fc.frames_in, fc.frames_out,
             fc.parse_errors, wifi.ap_started, wifi.sta_connected,
             (int)wifi.rssi, wifi.channel, wifi.udp_clients,
             wifi.tcp_clients, wifi.reconnects, wifi.reconnects_total,
             wifi.reconnect_streak, bridge_role_name(config.bridge_role),
             wifi.bridge_connected, wifi.bridge_reconnects,
             usb.frames_out, usb.queue_drops, bridge_tcp.queue_drops,
             fc.queue_drops, bridge_tcp.queue_drops,
             uart.rx_overflow,
             uart.driver_restarts, uart.high_queue_drops,
             uart.normal_queue_drops, can.rx_frames, can.tx_frames,
             can.bus_errors, can.dronecan_errors, can.arbitration_lost,
             can.bus_off_count, can.dronecan_nodes, config.udp_port,
             config.tcp_port, airlink_ota_in_progress(),
             airlink_ota_running_partition(), airlink_ota_image_state());
    airlink_usb_write_cli(output);
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (text == NULL || value == NULL || text[0] == '\0' || text[0] == '-') return false;
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool copy_config_text(char *destination, size_t capacity,
                             const char *value, bool dash_clears)
{
    if (destination == NULL || capacity == 0 || value == NULL) return false;
    if (dash_clears && strcmp(value, "-") == 0) {
        destination[0] = '\0';
        return true;
    }
    if (strlen(value) >= capacity) return false;
    strlcpy(destination, value, capacity);
    return true;
}

static void config_show(void)
{
    airlink_config_t config;
    airlink_config_get(&config);
    char output[768];
    snprintf(output, sizeof(output),
             "OK config generation=%" PRIu32 "\r\n"
             "route_mode=%s\r\n"
             "uart_baud=%" PRIu32 "\r\n"
             "wifi_mode=%s\r\n"
             "wifi_band=%s\r\n"
             "ap_ssid=%s\r\n"
             "ap_password=%s\r\n"
             "sta_ssid=%s\r\n"
             "sta_password=%s\r\n"
             "udp_port=%u\r\n"
             "tcp_port=%u\r\n"
             "usb_mode=%s\r\n"
             "bridge_role=%s\r\n"
             "can_bitrate=%" PRIu32 "\r\n"
             "led_brightness=%u\r\n"
             "serial_number=%s\r\n"
             "admin_password=%s\r\n",
             airlink_config_generation(), route_mode_name(config.route_mode),
             config.uart_baud, wifi_mode_name(config.wifi_mode),
             wifi_band_name(config.wifi_band), config.ap_ssid, config.ap_password,
             config.sta_ssid, config.sta_password, config.udp_port, config.tcp_port,
             usb_mode_name(config.usb_mode), bridge_role_name(config.bridge_role), config.can_bitrate,
             config.led_brightness, config.serial_number, config.admin_password);
    airlink_usb_write_cli(output);
}

static void config_help(void)
{
    airlink_usb_write_cli(
        "config show\r\n"
        "config set route_mode mavlink|transparent\r\n"
        "config set uart_baud 57600|115200|230400|460800|921600\r\n"
        "config set wifi_mode ap|sta|apsta\r\n"
        "config set wifi_band auto|2g|5g\r\n"
        "config set ap_ssid VALUE\r\n"
        "config set ap_password VALUE\r\n"
        "config set sta_ssid VALUE|-\r\n"
        "config set sta_password VALUE|-\r\n"
        "config set udp_port 1..65535\r\n"
        "config set tcp_port 1..65535\r\n"
        "config set usb_mode log|mavlink\r\n"
        "config set bridge_role off|air|ground\r\n"
        "config set can_bitrate 125000|250000|500000|1000000\r\n"
        "config set led_brightness 0..100\r\n"
        "config set admin_password VALUE\r\n"
        "config begin\r\n"
        "config stage KEY VALUE\r\n"
        "config validate\r\n"
        "config commit\r\n"
        "config abort\r\n"
        "config reset\r\n"
        "Legacy set saves immediately; transaction commit saves once. Reboot required.\r\n");
}

static bool config_apply_value(airlink_config_t *config, const char *arguments,
                               bool *recognized)
{
    const char *separator = arguments == NULL ? NULL : strchr(arguments, ' ');
    *recognized = true;
    if (separator == NULL) return false;
    const size_t key_length = (size_t)(separator - arguments);
    const char *value = separator + 1;
    while (*value == ' ') value++;
    if (key_length == 0 || *value == '\0') return false;
    bool value_ok = true;
    uint32_t number = 0;
#define KEY_IS(name) (key_length == sizeof(name) - 1U && strncmp(arguments, name, key_length) == 0)
    if (KEY_IS("route_mode")) {
        if (strcmp(value, "mavlink") == 0) config->route_mode = AIRLINK_ROUTE_MAVLINK;
        else if (strcmp(value, "transparent") == 0) config->route_mode = AIRLINK_ROUTE_TRANSPARENT;
        else value_ok = false;
    } else if (KEY_IS("uart_baud")) {
        value_ok = parse_u32(value, &number);
        if (value_ok) config->uart_baud = number;
    } else if (KEY_IS("wifi_mode")) {
        if (strcmp(value, "ap") == 0) config->wifi_mode = AIRLINK_WIFI_AP;
        else if (strcmp(value, "sta") == 0) config->wifi_mode = AIRLINK_WIFI_STA;
        else if (strcmp(value, "apsta") == 0) config->wifi_mode = AIRLINK_WIFI_APSTA;
        else value_ok = false;
    } else if (KEY_IS("wifi_band")) {
        if (strcmp(value, "auto") == 0) config->wifi_band = AIRLINK_WIFI_BAND_AUTO;
        else if (strcmp(value, "2g") == 0) config->wifi_band = AIRLINK_WIFI_BAND_2G;
        else if (strcmp(value, "5g") == 0) config->wifi_band = AIRLINK_WIFI_BAND_5G;
        else value_ok = false;
    } else if (KEY_IS("ap_ssid")) {
        value_ok = copy_config_text(config->ap_ssid, sizeof(config->ap_ssid), value, false);
    } else if (KEY_IS("ap_password")) {
        value_ok = copy_config_text(config->ap_password, sizeof(config->ap_password), value, false);
    } else if (KEY_IS("sta_ssid")) {
        value_ok = copy_config_text(config->sta_ssid, sizeof(config->sta_ssid), value, true);
    } else if (KEY_IS("sta_password")) {
        value_ok = copy_config_text(config->sta_password, sizeof(config->sta_password), value, true);
    } else if (KEY_IS("udp_port")) {
        value_ok = parse_u32(value, &number) && number <= UINT16_MAX;
        if (value_ok) config->udp_port = (uint16_t)number;
    } else if (KEY_IS("tcp_port")) {
        value_ok = parse_u32(value, &number) && number <= UINT16_MAX;
        if (value_ok) config->tcp_port = (uint16_t)number;
    } else if (KEY_IS("usb_mode")) {
        if (strcmp(value, "log") == 0) config->usb_mode = AIRLINK_USB_LOG_CLI;
        else if (strcmp(value, "mavlink") == 0) config->usb_mode = AIRLINK_USB_MAVLINK;
        else value_ok = false;
    } else if (KEY_IS("bridge_role")) {
        if (strcmp(value, "off") == 0) {
            config->bridge_enabled = false;
            config->bridge_role = AIRLINK_BRIDGE_OFF;
            config->wifi_mode = AIRLINK_WIFI_AP;
            config->usb_mode = AIRLINK_USB_LOG_CLI;
        } else if (strcmp(value, "air") == 0) {
            config->bridge_enabled = true;
            config->bridge_role = AIRLINK_BRIDGE_AIR;
            config->wifi_mode = AIRLINK_WIFI_AP;
            config->usb_mode = AIRLINK_USB_LOG_CLI;
        } else if (strcmp(value, "ground") == 0) {
            config->bridge_enabled = true;
            config->bridge_role = AIRLINK_BRIDGE_GROUND;
            config->wifi_mode = AIRLINK_WIFI_STA;
            config->usb_mode = AIRLINK_USB_MAVLINK;
        } else {
            value_ok = false;
        }
    } else if (KEY_IS("can_bitrate")) {
        value_ok = parse_u32(value, &number);
        if (value_ok) config->can_bitrate = number;
    } else if (KEY_IS("led_brightness")) {
        value_ok = parse_u32(value, &number) && number <= UINT8_MAX;
        if (value_ok) config->led_brightness = (uint8_t)number;
    } else if (KEY_IS("admin_password")) {
        value_ok = copy_config_text(config->admin_password, sizeof(config->admin_password), value, false);
    } else {
        *recognized = false;
    }
#undef KEY_IS
    return value_ok;
}

static void config_set(const char *arguments)
{
    if (airlink_router_fc_armed()) {
        airlink_usb_write_cli("ERR flight controller armed\r\n");
        return;
    }
    airlink_config_t config;
    airlink_config_get(&config);
    bool recognized = false;
    const bool value_ok = config_apply_value(&config, arguments, &recognized);

    if (!recognized) {
        airlink_usb_write_cli("ERR unknown config key; use config help\r\n");
    } else if (!value_ok || !airlink_config_validate(&config)) {
        airlink_usb_write_cli("ERR invalid value or incomplete configuration\r\n");
    } else if (airlink_config_save(&config) != ESP_OK) {
        airlink_usb_write_cli("ERR could not save configuration\r\n");
    } else {
        airlink_usb_write_cli("OK saved; reboot required\r\n");
    }
}

static void config_transaction_expire(void)
{
    if (s_config_transaction_active && esp_timer_get_time() > s_config_transaction_deadline_us) {
        memset(&s_staged_config, 0, sizeof(s_staged_config));
        s_config_transaction_active = false;
    }
}

static void config_transaction(const char *line)
{
    config_transaction_expire();
    if (strcmp(line, "config begin") == 0) {
        if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
            return;
        }
        airlink_config_get(&s_staged_config);
        s_config_transaction_active = true;
        s_config_transaction_deadline_us = esp_timer_get_time() + CONFIG_TRANSACTION_TIMEOUT_US;
        airlink_usb_write_cli("OK transaction begun timeout_s=30\r\n");
    } else if (strncmp(line, "config stage ", 13) == 0) {
        if (!s_config_transaction_active) {
            airlink_usb_write_cli("ERR no active transaction\r\n");
            return;
        }
        bool recognized = false;
        const bool value_ok = config_apply_value(&s_staged_config, line + 13, &recognized);
        if (!recognized) airlink_usb_write_cli("ERR unknown config key; use config help\r\n");
        else if (!value_ok) airlink_usb_write_cli("ERR invalid staged value\r\n");
        else {
            s_config_transaction_deadline_us = esp_timer_get_time() + CONFIG_TRANSACTION_TIMEOUT_US;
            airlink_usb_write_cli("OK staged\r\n");
        }
    } else if (strcmp(line, "config validate") == 0) {
        if (!s_config_transaction_active) airlink_usb_write_cli("ERR no active transaction\r\n");
        else if (!airlink_config_validate(&s_staged_config)) airlink_usb_write_cli("ERR invalid staged configuration\r\n");
        else airlink_usb_write_cli("OK valid\r\n");
    } else if (strcmp(line, "config commit") == 0) {
        if (!s_config_transaction_active) {
            airlink_usb_write_cli("ERR no active transaction\r\n");
        } else if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
        } else if (!airlink_config_validate(&s_staged_config)) {
            airlink_usb_write_cli("ERR invalid staged configuration\r\n");
        } else if (airlink_config_save(&s_staged_config) != ESP_OK) {
            airlink_usb_write_cli("ERR could not save configuration\r\n");
        } else {
            memset(&s_staged_config, 0, sizeof(s_staged_config));
            s_config_transaction_active = false;
            airlink_usb_write_cli("OK committed; reboot required\r\n");
        }
    } else if (strcmp(line, "config abort") == 0) {
        memset(&s_staged_config, 0, sizeof(s_staged_config));
        s_config_transaction_active = false;
        airlink_usb_write_cli("OK transaction aborted\r\n");
    }
}

static void handle_config(const char *line)
{
    if (strcmp(line, "config show") == 0) {
        config_show();
    } else if (strcmp(line, "config help") == 0) {
        config_help();
    } else if (strcmp(line, "config begin") == 0 ||
               strncmp(line, "config stage ", 13) == 0 ||
               strcmp(line, "config validate") == 0 ||
               strcmp(line, "config commit") == 0 ||
               strcmp(line, "config abort") == 0) {
        config_transaction(line);
    } else if (strncmp(line, "config set ", 11) == 0) {
        config_set(line + 11);
    } else if (strcmp(line, "config reset") == 0) {
        if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
        } else if (airlink_config_factory_reset() != ESP_OK) {
            airlink_usb_write_cli("ERR could not reset configuration\r\n");
        } else {
            airlink_usb_write_cli("OK factory defaults saved; reboot required\r\n");
        }
    } else {
        airlink_usb_write_cli("ERR config command; use config help\r\n");
    }
}

static esp_err_t usb_router_send(const uint8_t *data, size_t length,
                                 bool high_priority, void *context)
{
    (void)high_priority; (void)context;
    if (length > AIRLINK_MAX_FRAME_SIZE) return ESP_ERR_INVALID_SIZE;
    usb_packet_t packet = {.length = (uint16_t)length};
    memcpy(packet.data, data, length);
    return xQueueSend(s_tx_queue, &packet, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static int usb_log_tee(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int result = s_console_vprintf != NULL ? s_console_vprintf(format, args) : vprintf(format, args);
    if (s_mode == AIRLINK_USB_LOG_CLI && s_tx_queue != NULL && !xPortInIsrContext()) {
        usb_packet_t packet = {0};
        const int length = vsnprintf((char *)packet.data, sizeof(packet.data), format, copy);
        if (length > 0) {
            packet.length = (uint16_t)(length < (int)sizeof(packet.data) ?
                                       length : (int)sizeof(packet.data) - 1);
            xQueueSend(s_tx_queue, &packet, 0);
        }
    }
    va_end(copy);
    return result;
}

static bool escape_emit(const uint8_t *data, size_t length, void *context)
{
    (void)context;
    return airlink_router_ingest(AIRLINK_ENDPOINT_ID_USB, data, length) == ESP_OK;
}

/* Returns true after consuming an escape sequence and provides the first byte
 * that belongs to the CLI. Bytes that cannot be part of the sequence are
 * forwarded in their original order, including overlapping '+' prefixes. */
static bool escape_process(airlink_escape_matcher_t *matcher, const uint8_t *input,
                           size_t length, size_t *cli_offset)
{
    const airlink_escape_result_t result = airlink_escape_feed(
        matcher, (const uint8_t *)USB_CLI_ESCAPE, sizeof(USB_CLI_ESCAPE) - 1U,
        input, length, (uint64_t)esp_timer_get_time(), escape_emit, NULL, cli_offset);
    if (result != AIRLINK_ESCAPE_MATCHED) return false;
    airlink_router_unregister(AIRLINK_ENDPOINT_ID_USB);
    xQueueReset(s_tx_queue);
    s_mode = AIRLINK_USB_LOG_CLI;
    s_console_vprintf = esp_log_set_vprintf(usb_log_tee);
    airlink_usb_write_cli("\r\nOK temporary USB CLI; reboot restores configured mode.\r\n> ");
    return true;
}

esp_err_t airlink_usb_write_cli(const char *text)
{
    if (text == NULL || s_mode != AIRLINK_USB_LOG_CLI) return ESP_ERR_INVALID_STATE;
    const size_t length = strlen(text);
    return usb_serial_jtag_write_bytes(text, length, pdMS_TO_TICKS(20)) == (int)length ? ESP_OK : ESP_FAIL;
}

static void handle_builtin(const char *line)
{
    if (strcmp(line, "help") == 0) {
        airlink_usb_write_cli("commands: help, status, config ..., wifi scan, ota begin <bytes> <sha256>, reboot, usb log, usb mavlink, usb download, factory ...\r\n");
    } else if (strcmp(line, "status") == 0) {
        status_show();
    } else if (strncmp(line, "ota begin ", 10) == 0) {
        if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
        } else {
            char *end = NULL;
            errno = 0;
            const unsigned long image_size = strtoul(line + 10, &end, 10);
            uint8_t expected_sha256[32];
            if (errno != 0 || image_size == 0UL || end == NULL || *end != ' ' ||
                !parse_sha256_text(end + 1, expected_sha256)) {
                airlink_usb_write_cli("ERR ota syntax\r\n");
            } else {
                const esp_err_t err = airlink_ota_stream_begin((size_t)image_size, expected_sha256);
                if (err != ESP_OK) {
                    airlink_usb_write_cli("ERR ota begin failed\r\n");
                } else {
                    s_usb_ota_deadline_us = esp_timer_get_time() + USB_OTA_TIMEOUT_US;
                    airlink_usb_write_cli("OK ota ready\r\n");
                }
            }
        }
    } else if (strcmp(line, "wifi scan") == 0) {
        if (airlink_router_fc_armed() || airlink_ota_in_progress()) {
            airlink_usb_write_cli("ERR wifi scan not allowed\r\n");
        } else {
            const size_t capacity = 8U * 1024U;
            char *json = malloc(capacity);
            if (json == NULL || airlink_wifi_scan_json(json, capacity) != ESP_OK) {
                free(json);
                airlink_usb_write_cli("ERR wifi scan failed\r\n");
            } else {
                airlink_usb_write_cli("OK wifi scan\r\n");
                airlink_usb_write_cli(json);
                airlink_usb_write_cli("\r\n");
                free(json);
            }
        }
    } else if (strncmp(line, "config", 6) == 0 &&
               (line[6] == '\0' || line[6] == ' ')) {
        handle_config(line);
    } else if (strcmp(line, "usb download") == 0) {
        if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
            return;
        }
        airlink_usb_write_cli("OK USB downloader reset window open for 15 seconds\r\n");
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
        s_usb_download_deadline_us = esp_timer_get_time() + USB_DOWNLOAD_WINDOW_US;
        usb_reset_protection(false);
    } else if (strcmp(line, "reboot") == 0) {
        if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
            return;
        }
        airlink_usb_write_cli("OK rebooting\r\n");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else if (strcmp(line, "usb log") == 0 || strcmp(line, "usb mavlink") == 0) {
        if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
            return;
        }
        airlink_config_t config; airlink_config_get(&config);
        config.usb_mode = strcmp(line, "usb mavlink") == 0 ? AIRLINK_USB_MAVLINK : AIRLINK_USB_LOG_CLI;
        if (airlink_config_save(&config) == ESP_OK) {
            airlink_usb_write_cli("OK mode saved; rebooting\r\n");
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        } else {
            airlink_usb_write_cli("ERR could not save mode\r\n");
        }
    } else if (s_cli_handler != NULL) {
        s_cli_handler(line);
    } else {
        airlink_usb_write_cli("ERR unknown command\r\n");
    }
}

static void usb_task(void *argument)
{
    (void)argument;
    uint8_t rx[USB_RX_CHUNK];
    char line[192];
    size_t line_length = 0;
    usb_packet_t packet;
    airlink_escape_matcher_t escape = {0};
    if (s_mode == AIRLINK_USB_LOG_CLI) airlink_usb_write_cli("\r\nAirLink LOG_CLI ready. Type help.\r\n> ");
    while (true) {
        if (s_usb_download_deadline_us > 0 &&
            esp_timer_get_time() >= s_usb_download_deadline_us) {
            usb_reset_protection(true);
            s_usb_download_deadline_us = 0;
            airlink_usb_write_cli("\r\nUSB downloader reset window closed\r\n> ");
        }
        if (airlink_ota_stream_active()) {
            const int ota_count = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(20));
            if (ota_count <= 0) {
                if (esp_timer_get_time() >= s_usb_ota_deadline_us) {
                    airlink_ota_stream_abort();
                    airlink_usb_write_cli("\r\nERR ota receive timeout\r\n> ");
                }
                continue;
            }
            const size_t remaining = airlink_ota_stream_remaining();
            if ((size_t)ota_count > remaining ||
                airlink_ota_stream_write(rx, (size_t)ota_count) != ESP_OK) {
                airlink_ota_stream_abort();
                airlink_usb_write_cli("\r\nERR ota write failed\r\n> ");
                continue;
            }
            s_usb_ota_deadline_us = esp_timer_get_time() + USB_OTA_TIMEOUT_US;
            if (airlink_ota_stream_remaining() == 0U) {
                const esp_err_t finish_err = airlink_ota_stream_finish();
                if (finish_err != ESP_OK) {
                    airlink_usb_write_cli("\r\nERR ota verification failed\r\n> ");
                    continue;
                }
                airlink_usb_write_cli("\r\nOK ota verified; rebooting\r\n");
                usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            continue;
        }
        while (xQueueReceive(s_tx_queue, &packet, 0) == pdTRUE) {
            usb_serial_jtag_write_bytes(packet.data, packet.length, pdMS_TO_TICKS(5));
        }
        const int count = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(10));
        if (count <= 0) {
            if (s_mode == AIRLINK_USB_MAVLINK && escape.length > 0) {
                airlink_escape_flush_expired(&escape, (uint64_t)esp_timer_get_time(),
                                             USB_ESCAPE_TIMEOUT_US, escape_emit, NULL);
            }
            continue;
        }
        size_t cli_offset = 0;
        if (s_mode == AIRLINK_USB_MAVLINK) {
            if (!escape_process(&escape, rx, (size_t)count, &cli_offset)) continue;
        }
        for (size_t i = cli_offset; i < (size_t)count; ++i) {
            if (rx[i] == '\r' || rx[i] == '\n') {
                if (line_length == 0) continue;
                line[line_length] = '\0';
                handle_builtin(line);
                line_length = 0;
                airlink_usb_write_cli("> ");
            } else if (line_length + 1U < sizeof(line) && rx[i] >= 0x20 && rx[i] < 0x7f) {
                line[line_length++] = (char)rx[i];
            }
        }
    }
}

esp_err_t airlink_usb_start(airlink_usb_mode_t mode)
{
    s_mode = mode;
    /* Windows serial stacks can repeatedly pulse the native USB CDC reset
     * signal while a handle is open.  Disable that application-time reset
     * source before installing the USB driver so Web Serial and pyserial can
     * keep a stable bidirectional session.  This register is volatile, so the
     * physical BOOT strap still enters the ROM downloader after power-up. */
    usb_reset_protection(true);
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 2048,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&config), "usb", "driver install");
    s_tx_queue = xQueueCreate(USB_QUEUE_DEPTH, sizeof(usb_packet_t));
    if (s_tx_queue == NULL) return ESP_ERR_NO_MEM;
    if (mode == AIRLINK_USB_MAVLINK) {
        const airlink_router_endpoint_t endpoint = {
            .id = AIRLINK_ENDPOINT_ID_USB, .type = AIRLINK_ENDPOINT_USB,
            .send = usb_router_send, .name = "usb-mavlink",
        };
        ESP_RETURN_ON_ERROR(airlink_router_register(&endpoint), "usb", "router endpoint");
    }
    if (xTaskCreate(usb_task, "usb_mux", USB_TASK_STACK_SIZE, NULL,
                    USB_TASK_PRIORITY, &s_usb_task) != pdPASS) {
        if (mode == AIRLINK_USB_MAVLINK) airlink_router_unregister(AIRLINK_ENDPOINT_ID_USB);
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    /* Install the log tee only after the high-priority USB task has been
     * created and reached its blocking read. This avoids re-entering the USB
     * driver from scheduler/task-creation logging on the creator's stack. */
    if (mode == AIRLINK_USB_LOG_CLI) s_console_vprintf = esp_log_set_vprintf(usb_log_tee);
    return ESP_OK;
}

void airlink_usb_set_cli_handler(airlink_usb_cli_handler_t handler) { s_cli_handler = handler; }
bool airlink_usb_connected(void) { return usb_serial_jtag_is_connected(); }
bool airlink_usb_ready(void)
{
    return s_usb_task != NULL && usb_serial_jtag_is_driver_installed();
}
