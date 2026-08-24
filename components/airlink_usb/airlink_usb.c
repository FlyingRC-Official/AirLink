// SPDX-License-Identifier: Apache-2.0
#include "airlink_usb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airlink_config.h"
#include "airlink_router.h"
#include "airlink_uart.h"
#include "airlink_wifi.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define USB_QUEUE_DEPTH 64
#define USB_TASK_PRIORITY 19
#define USB_CLI_ESCAPE "+++AIRLINK-CLI\r\n"
typedef struct { uint16_t length; uint8_t data[AIRLINK_MAX_FRAME_SIZE]; } usb_packet_t;

static airlink_usb_mode_t s_mode;
static QueueHandle_t s_tx_queue;
static airlink_usb_cli_handler_t s_cli_handler;
static vprintf_like_t s_console_vprintf;

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
    airlink_endpoint_stats_t fc = {0};
    airlink_endpoint_stats_t usb = {0};
    airlink_endpoint_stats_t bridge_tcp = {0};
    airlink_config_t config;
    airlink_config_get(&config);
    airlink_wifi_get_status(&wifi);
    airlink_uart_get_health(&uart);
    const uint8_t vehicle_endpoint = config.bridge_role == AIRLINK_BRIDGE_GROUND ?
                                     AIRLINK_ENDPOINT_ID_BRIDGE : AIRLINK_ENDPOINT_ID_FC_UART;
    airlink_router_get_stats(vehicle_endpoint, &fc);
    airlink_router_get_stats(AIRLINK_ENDPOINT_ID_USB, &usb);
    if (config.bridge_role == AIRLINK_BRIDGE_AIR) {
        airlink_router_get_stats(AIRLINK_ENDPOINT_ID_TCP_BASE, &bridge_tcp);
    }

    char output[768];
    snprintf(output, sizeof(output),
             "OK status\r\n"
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
             "bridge_role=%s\r\n"
             "bridge_connected=%u\r\n"
             "bridge_reconnects=%" PRIu32 "\r\n"
             "usb_frames_out=%" PRIu32 "\r\n"
             "usb_queue_drops=%" PRIu32 "\r\n"
             "bridge_tcp_queue_drops=%" PRIu32 "\r\n"
             "uart_rx_overflow=%" PRIu32 "\r\n"
             "uart_driver_restarts=%" PRIu32 "\r\n"
             "uart_high_queue_drops=%" PRIu32 "\r\n"
             "uart_normal_queue_drops=%" PRIu32 "\r\n",
             airlink_router_fc_seen(), airlink_router_fc_armed(),
             fc.bytes_in, fc.bytes_out, fc.frames_in, fc.frames_out,
             fc.parse_errors, wifi.ap_started, wifi.sta_connected,
             (int)wifi.rssi, wifi.channel, wifi.udp_clients,
             wifi.tcp_clients, wifi.reconnects, bridge_role_name(config.bridge_role),
             wifi.bridge_connected, wifi.bridge_reconnects,
             usb.frames_out, usb.queue_drops, bridge_tcp.queue_drops,
             uart.rx_overflow,
             uart.driver_restarts, uart.high_queue_drops,
             uart.normal_queue_drops);
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
        "config reset\r\n"
        "Changes are saved immediately and take effect after reboot.\r\n");
}

static void config_set(const char *arguments)
{
    const char *separator = arguments == NULL ? NULL : strchr(arguments, ' ');
    if (separator == NULL) {
        airlink_usb_write_cli("ERR usage: config set KEY VALUE\r\n");
        return;
    }
    const size_t key_length = (size_t)(separator - arguments);
    const char *value = separator + 1;
    while (*value == ' ') value++;
    if (key_length == 0 || *value == '\0') {
        airlink_usb_write_cli("ERR usage: config set KEY VALUE\r\n");
        return;
    }
    if (airlink_router_fc_armed()) {
        airlink_usb_write_cli("ERR flight controller armed\r\n");
        return;
    }

    airlink_config_t config;
    airlink_config_get(&config);
    bool recognized = true;
    bool value_ok = true;
    uint32_t number = 0;
#define KEY_IS(name) (key_length == sizeof(name) - 1U && strncmp(arguments, name, key_length) == 0)
    if (KEY_IS("route_mode")) {
        if (strcmp(value, "mavlink") == 0) config.route_mode = AIRLINK_ROUTE_MAVLINK;
        else if (strcmp(value, "transparent") == 0) config.route_mode = AIRLINK_ROUTE_TRANSPARENT;
        else value_ok = false;
    } else if (KEY_IS("uart_baud")) {
        value_ok = parse_u32(value, &number);
        if (value_ok) config.uart_baud = number;
    } else if (KEY_IS("wifi_mode")) {
        if (strcmp(value, "ap") == 0) config.wifi_mode = AIRLINK_WIFI_AP;
        else if (strcmp(value, "sta") == 0) config.wifi_mode = AIRLINK_WIFI_STA;
        else if (strcmp(value, "apsta") == 0) config.wifi_mode = AIRLINK_WIFI_APSTA;
        else value_ok = false;
    } else if (KEY_IS("wifi_band")) {
        if (strcmp(value, "auto") == 0) config.wifi_band = AIRLINK_WIFI_BAND_AUTO;
        else if (strcmp(value, "2g") == 0) config.wifi_band = AIRLINK_WIFI_BAND_2G;
        else if (strcmp(value, "5g") == 0) config.wifi_band = AIRLINK_WIFI_BAND_5G;
        else value_ok = false;
    } else if (KEY_IS("ap_ssid")) {
        value_ok = copy_config_text(config.ap_ssid, sizeof(config.ap_ssid), value, false);
    } else if (KEY_IS("ap_password")) {
        value_ok = copy_config_text(config.ap_password, sizeof(config.ap_password), value, false);
    } else if (KEY_IS("sta_ssid")) {
        value_ok = copy_config_text(config.sta_ssid, sizeof(config.sta_ssid), value, true);
    } else if (KEY_IS("sta_password")) {
        value_ok = copy_config_text(config.sta_password, sizeof(config.sta_password), value, true);
    } else if (KEY_IS("udp_port")) {
        value_ok = parse_u32(value, &number) && number <= UINT16_MAX;
        if (value_ok) config.udp_port = (uint16_t)number;
    } else if (KEY_IS("tcp_port")) {
        value_ok = parse_u32(value, &number) && number <= UINT16_MAX;
        if (value_ok) config.tcp_port = (uint16_t)number;
    } else if (KEY_IS("usb_mode")) {
        if (strcmp(value, "log") == 0) config.usb_mode = AIRLINK_USB_LOG_CLI;
        else if (strcmp(value, "mavlink") == 0) config.usb_mode = AIRLINK_USB_MAVLINK;
        else value_ok = false;
    } else if (KEY_IS("bridge_role")) {
        if (strcmp(value, "off") == 0) {
            config.bridge_enabled = false;
            config.bridge_role = AIRLINK_BRIDGE_OFF;
            config.wifi_mode = AIRLINK_WIFI_AP;
            config.usb_mode = AIRLINK_USB_LOG_CLI;
        } else if (strcmp(value, "air") == 0) {
            config.bridge_enabled = true;
            config.bridge_role = AIRLINK_BRIDGE_AIR;
            config.wifi_mode = AIRLINK_WIFI_AP;
            config.usb_mode = AIRLINK_USB_LOG_CLI;
        } else if (strcmp(value, "ground") == 0) {
            config.bridge_enabled = true;
            config.bridge_role = AIRLINK_BRIDGE_GROUND;
            config.wifi_mode = AIRLINK_WIFI_STA;
            config.usb_mode = AIRLINK_USB_MAVLINK;
        } else {
            value_ok = false;
        }
    } else if (KEY_IS("can_bitrate")) {
        value_ok = parse_u32(value, &number);
        if (value_ok) config.can_bitrate = number;
    } else if (KEY_IS("led_brightness")) {
        value_ok = parse_u32(value, &number) && number <= UINT8_MAX;
        if (value_ok) config.led_brightness = (uint8_t)number;
    } else if (KEY_IS("admin_password")) {
        value_ok = copy_config_text(config.admin_password, sizeof(config.admin_password), value, false);
    } else {
        recognized = false;
    }
#undef KEY_IS

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

static void handle_config(const char *line)
{
    if (strcmp(line, "config show") == 0) {
        config_show();
    } else if (strcmp(line, "config help") == 0) {
        config_help();
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
    if (s_mode == AIRLINK_USB_LOG_CLI && usb_serial_jtag_is_driver_installed()) {
        char line[256];
        const int length = vsnprintf(line, sizeof(line), format, copy);
        if (length > 0) usb_serial_jtag_write_bytes(line, (size_t)(length < (int)sizeof(line) ? length : (int)sizeof(line) - 1), 0);
    }
    va_end(copy);
    return result;
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
        airlink_usb_write_cli("commands: help, status, config ..., reboot, usb log, usb mavlink, factory ...\r\n");
    } else if (strcmp(line, "status") == 0) {
        status_show();
    } else if (strncmp(line, "config", 6) == 0 &&
               (line[6] == '\0' || line[6] == ' ')) {
        handle_config(line);
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
    uint8_t rx[256];
    char line[192];
    size_t line_length = 0;
    usb_packet_t packet;
    if (s_mode == AIRLINK_USB_LOG_CLI) airlink_usb_write_cli("\r\nAirLink LOG_CLI ready. Type help.\r\n> ");
    while (true) {
        if (s_mode == AIRLINK_USB_MAVLINK) {
            while (xQueueReceive(s_tx_queue, &packet, 0) == pdTRUE) {
                usb_serial_jtag_write_bytes(packet.data, packet.length, pdMS_TO_TICKS(5));
            }
        }
        const int count = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(10));
        if (count <= 0) continue;
        if (s_mode == AIRLINK_USB_MAVLINK) {
            if ((size_t)count == strlen(USB_CLI_ESCAPE) &&
                memcmp(rx, USB_CLI_ESCAPE, strlen(USB_CLI_ESCAPE)) == 0) {
                airlink_router_unregister(AIRLINK_ENDPOINT_ID_USB);
                s_mode = AIRLINK_USB_LOG_CLI;
                s_console_vprintf = esp_log_set_vprintf(usb_log_tee);
                airlink_usb_write_cli("\r\nOK temporary USB CLI; reboot restores configured mode.\r\n> ");
            } else {
                airlink_router_ingest(AIRLINK_ENDPOINT_ID_USB, rx, (size_t)count);
            }
            continue;
        }
        for (int i = 0; i < count; ++i) {
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
    } else {
        s_console_vprintf = esp_log_set_vprintf(usb_log_tee);
    }
    return xTaskCreate(usb_task, "usb_mux", 4096, NULL, USB_TASK_PRIORITY, NULL) == pdPASS ?
           ESP_OK : ESP_ERR_NO_MEM;
}

void airlink_usb_set_cli_handler(airlink_usb_cli_handler_t handler) { s_cli_handler = handler; }
bool airlink_usb_connected(void) { return usb_serial_jtag_is_connected(); }
