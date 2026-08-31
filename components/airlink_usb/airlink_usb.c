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
#include "airlink_mesh.h"
#include "airlink_mesh_config.h"
#include "airlink_router.h"
#include "airlink_stream.h"
#include "airlink_uart.h"
#include "airlink_wifi.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
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
#define USB_RPC_MAX_PAYLOAD 3072U
#define USB_RPC_FRAME_MAX (USB_RPC_MAX_PAYLOAD + 64U)
typedef struct { uint16_t length; uint8_t data[AIRLINK_MAX_FRAME_SIZE]; } usb_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t kind;
    uint16_t method;
    uint16_t flags;
    uint16_t status;
    uint32_t request_id;
    uint32_t payload_length;
} usb_rpc_header_t;

enum {
    USB_RPC_KIND_REQUEST = 1,
    USB_RPC_KIND_RESPONSE = 2,
    USB_RPC_KIND_CONTROL = 3,
};

enum {
    USB_RPC_NETWORK_READ = 1,
    USB_RPC_NETWORK_CREATE = 2,
    USB_RPC_NETWORK_UPDATE = 3,
    USB_RPC_NODE_LIST = 16,
    USB_RPC_NODE_APPROVE = 17,
    USB_RPC_NODE_REMOVE = 18,
    USB_RPC_NODE_STATUS = 19,
    USB_RPC_CONFIG_READ = 32,
    USB_RPC_CONFIG_WRITE = 33,
    USB_RPC_REBOOT = 48,
    USB_RPC_OTA_BEGIN = 64,
    USB_RPC_OTA_CHUNK = 65,
    USB_RPC_OTA_COMMIT = 66,
    USB_RPC_OTA_ABORT = 67,
};

enum {
    USB_RPC_OK = 0,
    USB_RPC_ERR_MALFORMED = 1,
    USB_RPC_ERR_UNSUPPORTED = 2,
    USB_RPC_ERR_INVALID = 3,
    USB_RPC_ERR_ARMED_OR_UNKNOWN = 4,
    USB_RPC_ERR_NOT_FOUND = 5,
    USB_RPC_ERR_STORAGE = 6,
    USB_RPC_ERR_OTA = 7,
};

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
static bool s_binary_mode;
static int64_t s_binary_deadline_us;

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

void airlink_usb_reset_guard_enable(void)
{
    usb_reset_protection(true);
}

void airlink_usb_system_restart(void)
{
    /* ESP-IDF's C5 esp_restart() deliberately resets only the CPU so the ROM
     * can retain USB logging.  That also retains stale Serial/JTAG endpoint
     * state on Windows.  Use the C5 ROM system reset here so the USB digital
     * peripheral really resets and re-enumerates; all persistent state has
     * already been committed before this function is called. */
    airlink_wifi_prepare_restart();
    airlink_mesh_prepare_restart();
    vTaskDelay(pdMS_TO_TICKS(100));
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
    esp_rom_delay_us(20000);
    esp_rom_software_reset_system();
    while (true) { }
}

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

static const char *fc_transport_name(airlink_fc_transport_t transport)
{
    return transport == AIRLINK_FC_TRANSPORT_DRONECAN ? "dronecan" : "uart";
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

static const char *mesh_role_name(airlink_mesh_role_t role)
{
    if (role == AIRLINK_MESH_ROLE_AIR) return "air";
    if (role == AIRLINK_MESH_ROLE_GROUND_ROOT) return "ground_root";
    return "off";
}

static bool bridge_conflicts_with_mesh(const airlink_config_t *config)
{
    if (config == NULL || config->bridge_role == AIRLINK_BRIDGE_OFF) return false;
    airlink_mesh_config_t mesh;
    airlink_mesh_config_get(&mesh);
    return mesh.configured && mesh.role != AIRLINK_MESH_ROLE_OFF;
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
        AIRLINK_ENDPOINT_ID_BRIDGE :
        (config.fc_transport == AIRLINK_FC_TRANSPORT_DRONECAN ?
         AIRLINK_ENDPOINT_ID_FC_CAN : AIRLINK_ENDPOINT_ID_FC_UART);
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
    char output[3072];
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
             "coredump_present=%u\r\n"
             "coredump_size=%" PRIu32 "\r\n"
             "previous_boot_stage=%s\r\n"
             "boot_stage=%s\r\n"
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
             "bridge_last_errno=%" PRIu32 "\r\n"
             "tcp_last_errno=%" PRIu32 "\r\n"
             "bridge_connects_total=%" PRIu32 "\r\n"
             "tcp_accepts_total=%" PRIu32 "\r\n"
             "tcp_disconnects_total=%" PRIu32 "\r\n"
             "tcp_queue_alloc_failures=%" PRIu32 "\r\n"
             "tcp_queue_peak=%" PRIu32 "\r\n"
             "tcp_queue_current=%" PRIu32 "\r\n"
             "tcp_send_would_block=%" PRIu32 "\r\n"
             "network_task_loops=%" PRIu32 "\r\n"
             "tcp_listener_active=%u\r\n"
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
              "can_tx_error_count=%" PRIu16 "\r\n"
              "can_rx_error_count=%" PRIu16 "\r\n"
              "can_bus_off=%" PRIu32 "\r\n"
             "can_dronecan_nodes=%u\r\n"
             "can_tunnel_rx_bytes=%" PRIu64 "\r\n"
             "can_tunnel_tx_bytes=%" PRIu64 "\r\n"
             "can_tunnel_rx_transfers=%" PRIu32 "\r\n"
             "can_tunnel_tx_transfers=%" PRIu32 "\r\n"
             "can_tunnel_drops=%" PRIu32 "\r\n"
             "can_high_queue_drops=%" PRIu32 "\r\n"
             "can_normal_queue_drops=%" PRIu32 "\r\n"
             "can_keepalives=%" PRIu32 "\r\n"
             "can_peer_online=%u\r\n"
             "udp_listener_port=%u\r\n"
             "tcp_listener_port=%u\r\n"
             "ota_in_progress=%u\r\n"
             "ota_running_partition=%s\r\n"
             "ota_image_state=%" PRId32 "\r\n",
             app->version, AIRLINK_HARDWARE_ID, config.serial_number,
             diag.uptime_seconds, diag.free_heap, diag.minimum_free_heap,
             diag.boot_count, diag.reset_reason,
             diag.coredump_present, diag.coredump_size,
             diag.previous_boot_stage, diag.boot_stage,
             airlink_router_fc_seen(), airlink_router_fc_armed(),
             fc.bytes_in, fc.bytes_out, fc.frames_in, fc.frames_out,
             fc.parse_errors, wifi.ap_started, wifi.sta_connected,
             (int)wifi.rssi, wifi.channel, wifi.udp_clients,
             wifi.tcp_clients, wifi.reconnects, wifi.reconnects_total,
             wifi.reconnect_streak, bridge_role_name(config.bridge_role),
             wifi.bridge_connected, wifi.bridge_reconnects,
             wifi.bridge_last_errno, wifi.tcp_last_errno,
             wifi.bridge_connects_total, wifi.tcp_accepts_total,
             wifi.tcp_disconnects_total, wifi.tcp_queue_alloc_failures,
             wifi.tcp_queue_peak, wifi.tcp_queue_current,
             wifi.tcp_send_would_block,
             wifi.network_task_loops,
             wifi.tcp_listener_active,
             usb.frames_out, usb.queue_drops, bridge_tcp.queue_drops,
             fc.queue_drops, wifi.bridge_tx_queue_drops,
             uart.rx_overflow,
             uart.driver_restarts, uart.high_queue_drops,
             uart.normal_queue_drops, can.rx_frames, can.tx_frames,
             can.bus_errors, can.dronecan_errors, can.arbitration_lost,
             can.tx_error_count, can.rx_error_count,
             can.bus_off_count, can.dronecan_nodes,
             can.tunnel_rx_bytes, can.tunnel_tx_bytes,
             can.tunnel_rx_transfers, can.tunnel_tx_transfers,
             can.tunnel_drops, can.high_queue_drops, can.normal_queue_drops,
             can.keepalives, can.peer_online, config.udp_port,
             config.tcp_port, airlink_ota_in_progress(),
             airlink_ota_running_partition(), airlink_ota_image_state());
    airlink_usb_write_cli(output);
    airlink_mesh_config_t mesh_config;
    airlink_mesh_status_t mesh_status = {0};
    airlink_mesh_config_get(&mesh_config);
    airlink_mesh_get_status(&mesh_status);
    char mesh_output[640];
    snprintf(mesh_output, sizeof(mesh_output),
             "mesh_role=%s\r\n"
             "mesh_configured=%u\r\n"
             "mesh_generation=%" PRIu32 "\r\n"
             "mesh_connected=%u\r\n"
             "mesh_rootless=%u\r\n"
             "mesh_layer=%u\r\n"
             "mesh_nodes=%u\r\n"
             "mesh_approved_online=%u\r\n"
             "mesh_reorganizations=%" PRIu32 "\r\n"
             "mesh_tx_packets=%" PRIu32 "\r\n"
             "mesh_rx_packets=%" PRIu32 "\r\n"
             "mesh_auth_failures=%" PRIu32 "\r\n"
             "mesh_replay_drops=%" PRIu32 "\r\n"
             "mesh_queue_drops=%" PRIu32 "\r\n",
             mesh_role_name((airlink_mesh_role_t)mesh_config.role), mesh_config.configured,
             airlink_mesh_config_generation(), mesh_status.connected, mesh_status.rootless,
             mesh_status.layer, mesh_status.discovered_nodes,
             mesh_status.approved_online_nodes, mesh_status.reorganizations,
             mesh_status.tx_packets, mesh_status.rx_packets, mesh_status.auth_failures,
             mesh_status.replay_drops, mesh_status.queue_drops);
    airlink_usb_write_cli(mesh_output);
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
             "fc_transport=%s\r\n"
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
             "can_node_id=%u\r\n"
             "can_remote_node_id=%u\r\n"
             "can_serial_id=%d\r\n"
             "led_brightness=%u\r\n"
             "serial_number=%s\r\n"
             "admin_password=%s\r\n",
             airlink_config_generation(), route_mode_name(config.route_mode),
             fc_transport_name(config.fc_transport),
             config.uart_baud, wifi_mode_name(config.wifi_mode),
             wifi_band_name(config.wifi_band), config.ap_ssid, config.ap_password,
             config.sta_ssid, config.sta_password, config.udp_port, config.tcp_port,
             usb_mode_name(config.usb_mode), bridge_role_name(config.bridge_role), config.can_bitrate,
             config.can_node_id, config.can_remote_node_id, config.can_serial_id,
             config.led_brightness, config.serial_number, config.admin_password);
    airlink_usb_write_cli(output);
}

static void config_help(void)
{
    airlink_usb_write_cli(
        "config show\r\n"
        "config set route_mode mavlink|transparent\r\n"
        "config set fc_transport uart|dronecan\r\n"
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
        "config set can_node_id 1..127\r\n"
        "config set can_remote_node_id 1..127\r\n"
        "config set can_serial_id 0..15\r\n"
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
    } else if (KEY_IS("fc_transport")) {
        if (strcmp(value, "uart") == 0) config->fc_transport = AIRLINK_FC_TRANSPORT_UART;
        else if (strcmp(value, "dronecan") == 0) {
            config->fc_transport = AIRLINK_FC_TRANSPORT_DRONECAN;
            config->route_mode = AIRLINK_ROUTE_MAVLINK;
        } else value_ok = false;
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
    } else if (KEY_IS("can_node_id")) {
        value_ok = parse_u32(value, &number) && number <= UINT8_MAX;
        if (value_ok) config->can_node_id = (uint8_t)number;
    } else if (KEY_IS("can_remote_node_id")) {
        value_ok = parse_u32(value, &number) && number <= UINT8_MAX;
        if (value_ok) config->can_remote_node_id = (uint8_t)number;
    } else if (KEY_IS("can_serial_id")) {
        value_ok = parse_u32(value, &number) && number <= INT8_MAX;
        if (value_ok) config->can_serial_id = (int8_t)number;
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
    } else if (!value_ok || !airlink_config_validate(&config) ||
               bridge_conflicts_with_mesh(&config)) {
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
        else if (!airlink_config_validate(&s_staged_config) ||
                 bridge_conflicts_with_mesh(&s_staged_config)) {
            airlink_usb_write_cli("ERR invalid or Mesh-conflicting staged configuration\r\n");
        }
        else airlink_usb_write_cli("OK valid\r\n");
    } else if (strcmp(line, "config commit") == 0) {
        if (!s_config_transaction_active) {
            airlink_usb_write_cli("ERR no active transaction\r\n");
        } else if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
        } else if (!airlink_config_validate(&s_staged_config) ||
                   bridge_conflicts_with_mesh(&s_staged_config)) {
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

static size_t cobs_encode(const uint8_t *input, size_t length,
                          uint8_t *output, size_t capacity)
{
    if (capacity == 0) return 0;
    size_t code_index = 0, write = 1;
    uint8_t code = 1;
    for (size_t read = 0; read < length; ++read) {
        if (input[read] == 0) {
            if (code_index >= capacity) return 0;
            output[code_index] = code; code_index = write++;
            if (write > capacity) return 0;
            code = 1;
        } else {
            if (write >= capacity) return 0;
            output[write++] = input[read];
            if (++code == 0xff) {
                output[code_index] = code; code_index = write++;
                if (write > capacity) return 0;
                code = 1;
            }
        }
    }
    if (code_index >= capacity) return 0;
    output[code_index] = code;
    return write;
}

static size_t cobs_decode(const uint8_t *input, size_t length,
                          uint8_t *output, size_t capacity)
{
    size_t read = 0, write = 0;
    while (read < length) {
        const uint8_t code = input[read++];
        if (code == 0 || read + (size_t)code - 1U > length) return 0;
        for (uint8_t i = 1; i < code; ++i) {
            if (write >= capacity) return 0;
            output[write++] = input[read++];
        }
        if (code != 0xff && read < length) {
            if (write >= capacity) return 0;
            output[write++] = 0;
        }
    }
    return write;
}

static void rpc_send_response(const usb_rpc_header_t *request, uint16_t status,
                              const void *payload, size_t payload_length)
{
    if (payload_length > USB_RPC_MAX_PAYLOAD) { status = USB_RPC_ERR_INVALID; payload = NULL; payload_length = 0; }
    uint8_t raw[USB_RPC_FRAME_MAX];
    usb_rpc_header_t response = {
        .version = 1, .kind = USB_RPC_KIND_RESPONSE, .method = request->method,
        .status = status, .request_id = request->request_id,
        .payload_length = (uint32_t)payload_length,
    };
    memcpy(raw, &response, sizeof(response));
    if (payload_length != 0) memcpy(raw + sizeof(response), payload, payload_length);
    const uint32_t crc = airlink_crc32(raw, sizeof(response) + payload_length);
    memcpy(raw + sizeof(response) + payload_length, &crc, sizeof(crc));
    uint8_t encoded[USB_RPC_FRAME_MAX + 32U];
    const size_t encoded_length = cobs_encode(raw, sizeof(response) + payload_length + sizeof(crc),
                                               encoded, sizeof(encoded) - 1U);
    if (encoded_length == 0) return;
    encoded[encoded_length] = 0;
    (void)usb_serial_jtag_write_bytes(encoded, encoded_length + 1U, pdMS_TO_TICKS(100));
}

static bool parse_mac_text(const char *text, uint8_t mac[6])
{
    if (text == NULL) return false;
    unsigned values[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) return false;
    for (size_t i = 0; i < 6; ++i) mac[i] = (uint8_t)values[i];
    return true;
}

static bool json_identity(const char *json, char serial[AIRLINK_MESH_SERIAL_SIZE + 1U],
                          uint8_t mac[6])
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;
    const cJSON *serial_json = cJSON_GetObjectItemCaseSensitive(root, "serial");
    const cJSON *mac_json = cJSON_GetObjectItemCaseSensitive(root, "mac");
    const bool valid = cJSON_IsString(serial_json) && cJSON_IsString(mac_json) &&
        strlen(serial_json->valuestring) <= AIRLINK_MESH_SERIAL_SIZE &&
        parse_mac_text(mac_json->valuestring, mac);
    if (valid) strlcpy(serial, serial_json->valuestring, AIRLINK_MESH_SERIAL_SIZE + 1U);
    cJSON_Delete(root);
    return valid;
}

static int config_json(char *output, size_t capacity, const airlink_config_t *config,
                       const char *node_serial, const uint8_t *node_mac)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char identity[128] = "";
    if (node_mac != NULL) {
        snprintf(identity, sizeof(identity),
                 "\"node_serial\":\"%s\",\"node_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",",
                 node_serial, node_mac[0], node_mac[1], node_mac[2], node_mac[3], node_mac[4], node_mac[5]);
    }
    return snprintf(output, capacity,
        "{\"firmware\":\"%s\",\"serial_number\":\"%s\",%s"
        "\"config\":{\"schema_version\":%u,\"generation\":%" PRIu32
        ",\"uart_baud\":%" PRIu32 ",\"route_mode\":%u,\"fc_transport\":%u,"
        "\"wifi_mode\":%u,\"wifi_band\":%u,\"ap_ssid\":\"%s\",\"sta_ssid\":\"%s\","
        "\"udp_port\":%u,\"tcp_port\":%u,\"usb_mode\":%u,\"bridge_role\":%u,"
        "\"can_bitrate\":%" PRIu32 ",\"can_node_id\":%u,\"can_remote_node_id\":%u,"
        "\"can_serial_id\":%d,\"led_brightness\":%u,\"serial_number\":\"%s\"}}",
        app->version, config->serial_number, identity, config->schema_version,
        airlink_config_generation(), config->uart_baud,
        config->route_mode, config->fc_transport, config->wifi_mode, config->wifi_band,
        config->ap_ssid, config->sta_ssid, config->udp_port, config->tcp_port,
        config->usb_mode, config->bridge_role, config->can_bitrate, config->can_node_id,
        config->can_remote_node_id, config->can_serial_id, config->led_brightness,
        config->serial_number);
}

static bool json_copy_optional(const cJSON *object, const char *name,
                               char *destination, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (value == NULL) return true;
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity) return false;
    strlcpy(destination, value->valuestring, capacity); return true;
}

static bool json_u32_optional(const cJSON *object, const char *name, uint32_t *value,
                              uint32_t maximum)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > maximum ||
        item->valuedouble != (double)item->valueint) return false;
    *value = (uint32_t)item->valuedouble; return true;
}

static bool apply_config_json(const cJSON *object, airlink_config_t *config)
{
    uint32_t value;
#define APPLY_U32(name, field, maximum) do { \
    value = (uint32_t)config->field; \
    if (!json_u32_optional(object, name, &value, maximum)) return false; \
    config->field = value; \
} while (0)
    APPLY_U32("uart_baud", uart_baud, UINT32_MAX);
    APPLY_U32("route_mode", route_mode, AIRLINK_ROUTE_TRANSPARENT);
    APPLY_U32("fc_transport", fc_transport, AIRLINK_FC_TRANSPORT_DRONECAN);
    APPLY_U32("wifi_mode", wifi_mode, AIRLINK_WIFI_APSTA);
    APPLY_U32("wifi_band", wifi_band, AIRLINK_WIFI_BAND_5G);
    APPLY_U32("udp_port", udp_port, UINT16_MAX);
    APPLY_U32("tcp_port", tcp_port, UINT16_MAX);
    APPLY_U32("usb_mode", usb_mode, AIRLINK_USB_MAVLINK);
    APPLY_U32("can_bitrate", can_bitrate, UINT32_MAX);
    APPLY_U32("can_node_id", can_node_id, UINT8_MAX);
    APPLY_U32("can_remote_node_id", can_remote_node_id, UINT8_MAX);
    APPLY_U32("can_serial_id", can_serial_id, INT8_MAX);
    APPLY_U32("led_brightness", led_brightness, UINT8_MAX);
#undef APPLY_U32
    if (!json_copy_optional(object, "ap_ssid", config->ap_ssid, sizeof(config->ap_ssid)) ||
        !json_copy_optional(object, "ap_password", config->ap_password, sizeof(config->ap_password)) ||
        !json_copy_optional(object, "sta_ssid", config->sta_ssid, sizeof(config->sta_ssid)) ||
        !json_copy_optional(object, "sta_password", config->sta_password, sizeof(config->sta_password)) ||
        !json_copy_optional(object, "admin_password", config->admin_password,
                            sizeof(config->admin_password))) return false;
    config->bridge_role = AIRLINK_BRIDGE_OFF;
    config->bridge_enabled = false;
    return airlink_config_validate(config);
}

static void rpc_handle(const uint8_t *frame, size_t frame_length)
{
    if (frame_length < sizeof(usb_rpc_header_t) + sizeof(uint32_t)) return;
    usb_rpc_header_t request; memcpy(&request, frame, sizeof(request));
    if (request.version != 1 || request.kind != USB_RPC_KIND_REQUEST ||
        request.payload_length > USB_RPC_MAX_PAYLOAD ||
        frame_length != sizeof(request) + request.payload_length + sizeof(uint32_t)) {
        rpc_send_response(&request, USB_RPC_ERR_MALFORMED, NULL, 0); return;
    }
    uint32_t expected_crc; memcpy(&expected_crc, frame + sizeof(request) + request.payload_length, 4);
    if (expected_crc != airlink_crc32(frame, sizeof(request) + request.payload_length)) {
        rpc_send_response(&request, USB_RPC_ERR_MALFORMED, NULL, 0); return;
    }
    char text[USB_RPC_MAX_PAYLOAD + 1U];
    if (request.payload_length != 0) memcpy(text, frame + sizeof(request), request.payload_length);
    text[request.payload_length] = '\0';
    char response[USB_RPC_MAX_PAYLOAD + 1U];
    uint16_t status = USB_RPC_OK; size_t response_length = 0;
    bool restart_after_response = false;
    airlink_mesh_config_t mesh;
    switch (request.method) {
        case USB_RPC_NETWORK_READ: {
            airlink_mesh_config_get(&mesh);
            const bool include_secret = strstr(text, "\"include_secret\":true") != NULL;
            if (airlink_mesh_config_export_json(&mesh, include_secret, response,
                                                sizeof(response)) != ESP_OK) status = USB_RPC_ERR_NOT_FOUND;
            else response_length = strlen(response);
            break;
        }
        case USB_RPC_NETWORK_CREATE: {
            airlink_config_t base; airlink_config_get(&base);
            if (base.bridge_role != AIRLINK_BRIDGE_OFF || airlink_router_fc_armed()) {
                status = USB_RPC_ERR_ARMED_OR_UNKNOWN; break;
            }
            if (airlink_mesh_config_create(AIRLINK_MESH_ROLE_GROUND_ROOT, &mesh) != ESP_OK ||
                airlink_mesh_config_save(&mesh) != ESP_OK) status = USB_RPC_ERR_STORAGE;
            else if (airlink_mesh_config_export_json(&mesh, true, response, sizeof(response)) != ESP_OK) {
                status = USB_RPC_ERR_STORAGE;
            } else response_length = strlen(response);
            break;
        }
        case USB_RPC_NETWORK_UPDATE: {
            airlink_mesh_config_get(&mesh);
            const airlink_mesh_role_t role = (airlink_mesh_role_t)mesh.role;
            airlink_mesh_config_t candidate;
            if (!airlink_mesh_global_safe()) status = USB_RPC_ERR_ARMED_OR_UNKNOWN;
            else if (strstr(text, "\"reset\":true") != NULL) {
                if (airlink_mesh_config_reset() != ESP_OK) status = USB_RPC_ERR_STORAGE;
                else {
                    strlcpy(response, "{\"reset\":true,\"rebooting\":true}", sizeof(response));
                    response_length = strlen(response); restart_after_response = true;
                }
            }
            else if (airlink_mesh_config_import_json(text, role, &candidate) != ESP_OK) status = USB_RPC_ERR_INVALID;
            else if (airlink_mesh_update_network(&candidate, 10000) != ESP_OK) {
                status = USB_RPC_ERR_STORAGE;
            }
            else {
                strlcpy(response, "{\"committed\":true,\"rebooting\":true}", sizeof(response));
                response_length = strlen(response);
                restart_after_response = true;
            }
            break;
        }
        case USB_RPC_NODE_LIST:
        case USB_RPC_NODE_STATUS:
            response_length = airlink_mesh_nodes_json(response, sizeof(response));
            break;
        case USB_RPC_NODE_APPROVE:
        case USB_RPC_NODE_REMOVE: {
            char serial[AIRLINK_MESH_SERIAL_SIZE + 1U]; uint8_t mac[6];
            if (!json_identity(text, serial, mac)) status = USB_RPC_ERR_INVALID;
            else {
                const esp_err_t err = request.method == USB_RPC_NODE_APPROVE ?
                    airlink_mesh_approve_node(serial, mac) : airlink_mesh_remove_node(serial, mac);
                if (err == ESP_ERR_NOT_FOUND) status = USB_RPC_ERR_NOT_FOUND;
                else if (err != ESP_OK) status = USB_RPC_ERR_STORAGE;
            }
            break;
        }
        case USB_RPC_CONFIG_READ: {
            airlink_config_t config;
            char serial[AIRLINK_MESH_SERIAL_SIZE + 1U]; uint8_t mac[6];
            const bool remote = request.payload_length != 0 && json_identity(text, serial, mac);
            if (remote) {
                if (airlink_mesh_node_config_get(mac, &config, 3000) != ESP_OK ||
                    strcmp(config.serial_number, serial) != 0) {
                    status = USB_RPC_ERR_NOT_FOUND; break;
                }
            } else airlink_config_get(&config);
            const int length = config_json(response, sizeof(response), &config,
                                           remote ? serial : NULL, remote ? mac : NULL);
            response_length = length > 0 ? (size_t)length : 0;
            break;
        }
        case USB_RPC_CONFIG_WRITE: {
            cJSON *root = cJSON_Parse(text);
            const cJSON *config_json_value = root == NULL ? NULL :
                cJSON_GetObjectItemCaseSensitive(root, "config");
            const cJSON *serial_json = root == NULL ? NULL :
                cJSON_GetObjectItemCaseSensitive(root, "serial");
            const cJSON *mac_json = root == NULL ? NULL :
                cJSON_GetObjectItemCaseSensitive(root, "mac");
            uint8_t mac[6]; airlink_config_t config;
            if (!cJSON_IsObject(config_json_value) || !cJSON_IsString(serial_json) ||
                !cJSON_IsString(mac_json) || !parse_mac_text(mac_json->valuestring, mac)) {
                status = USB_RPC_ERR_INVALID;
            } else if (!airlink_mesh_global_safe()) {
                status = USB_RPC_ERR_ARMED_OR_UNKNOWN;
            } else if (airlink_mesh_node_config_get(mac, &config, 3000) != ESP_OK ||
                       strcmp(config.serial_number, serial_json->valuestring) != 0) {
                status = USB_RPC_ERR_NOT_FOUND;
            } else if (!apply_config_json(config_json_value, &config)) {
                status = USB_RPC_ERR_INVALID;
            } else if (airlink_mesh_node_config_set(mac, &config, 3000) != ESP_OK) {
                status = USB_RPC_ERR_STORAGE;
            } else {
                strlcpy(response, "{\"saved\":true,\"rebooting\":true}", sizeof(response));
                response_length = strlen(response);
            }
            cJSON_Delete(root);
            break;
        }
        case USB_RPC_OTA_BEGIN: {
            cJSON *root = cJSON_Parse(text);
            const cJSON *size = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "size");
            const cJSON *sha = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "sha256");
            const cJSON *version = root == NULL ? NULL :
                cJSON_GetObjectItemCaseSensitive(root, "version");
            const cJSON *targets_json = root == NULL ? NULL :
                cJSON_GetObjectItemCaseSensitive(root, "targets");
            const cJSON *include_root_json = root == NULL ? NULL :
                cJSON_GetObjectItemCaseSensitive(root, "include_root");
            uint8_t digest[32];
            uint8_t targets[AIRLINK_MESH_MAX_NODES][6];
            size_t target_count = 0;
            bool targets_valid = targets_json == NULL || cJSON_IsArray(targets_json);
            if (cJSON_IsArray(targets_json)) {
                const cJSON *target = NULL;
                cJSON_ArrayForEach(target, targets_json) {
                    if (target_count >= AIRLINK_MESH_MAX_NODES || !cJSON_IsString(target) ||
                        !parse_mac_text(target->valuestring, targets[target_count])) {
                        targets_valid = false; break;
                    }
                    target_count++;
                }
            }
            if (!airlink_mesh_global_safe()) status = USB_RPC_ERR_ARMED_OR_UNKNOWN;
            else if (!cJSON_IsNumber(size) || !cJSON_IsString(sha) || !cJSON_IsString(version) ||
                     !parse_sha256_text(sha->valuestring, digest) || size->valuedouble <= 0 ||
                     !targets_valid ||
                     airlink_mesh_ota_begin((size_t)size->valuedouble, digest, version->valuestring,
                         targets, target_count, cJSON_IsTrue(include_root_json)) != ESP_OK) {
                status = USB_RPC_ERR_OTA;
            }
            cJSON_Delete(root);
            break;
        }
        case USB_RPC_OTA_CHUNK:
            if (!airlink_mesh_ota_active() || request.payload_length > 1024U ||
                airlink_mesh_ota_write(frame + sizeof(request), request.payload_length) != ESP_OK) {
                status = USB_RPC_ERR_OTA;
            }
            break;
        case USB_RPC_OTA_COMMIT:
            if (!airlink_mesh_ota_active() || airlink_mesh_ota_finish() != ESP_OK) {
                status = USB_RPC_ERR_OTA;
            } else {
                strlcpy(response, "{\"verified\":true,\"activating\":true}", sizeof(response));
                response_length = strlen(response);
            }
            break;
        case USB_RPC_OTA_ABORT:
            airlink_mesh_ota_abort();
            break;
        case USB_RPC_REBOOT:
            if (!airlink_mesh_global_safe()) status = USB_RPC_ERR_ARMED_OR_UNKNOWN;
            else {
                char serial[AIRLINK_MESH_SERIAL_SIZE + 1U]; uint8_t mac[6];
                const bool remote = request.payload_length != 0 && json_identity(text, serial, mac);
                if (remote && airlink_mesh_reboot_node(mac) != ESP_OK) status = USB_RPC_ERR_NOT_FOUND;
                else {
                    strlcpy(response, remote ? "{\"node_rebooting\":true}" :
                            "{\"rebooting\":true}", sizeof(response));
                    response_length = strlen(response);
                    restart_after_response = !remote;
                }
            }
            break;
        default:
            status = USB_RPC_ERR_UNSUPPORTED;
            break;
    }
    rpc_send_response(&request, status, response_length == 0 ? NULL : response, response_length);
    if (restart_after_response && status == USB_RPC_OK) {
        (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
        vTaskDelay(pdMS_TO_TICKS(50)); airlink_usb_system_restart();
    }
}

static void restore_usb_mavlink(void)
{
    if (!s_binary_mode) return;
    if (airlink_mesh_ota_active()) airlink_mesh_ota_abort();
    s_binary_mode = false; s_binary_deadline_us = 0; s_mode = AIRLINK_USB_MAVLINK;
    const airlink_router_endpoint_t endpoint = {
        .id = AIRLINK_ENDPOINT_ID_USB, .type = AIRLINK_ENDPOINT_USB,
        .direction = AIRLINK_ENDPOINT_DIRECTION_GCS, .send = usb_router_send,
        .name = "usb-mavlink",
    };
    (void)airlink_router_register(&endpoint);
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
    airlink_mesh_config_t mesh;
    airlink_mesh_config_get(&mesh);
    if (mesh.configured && mesh.role == AIRLINK_MESH_ROLE_GROUND_ROOT) {
        s_mode = AIRLINK_USB_LOG_CLI;
        s_binary_mode = true;
        s_binary_deadline_us = esp_timer_get_time() + INT64_C(60000000);
        return true;
    }
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
        airlink_usb_write_cli("commands: help, status, config ..., mesh ..., wifi scan, ota begin <bytes> <sha256>, reboot, usb log, usb mavlink, usb download, factory ...\r\n");
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
    } else if (strcmp(line, "mesh show") == 0) {
        airlink_mesh_config_t mesh; airlink_mesh_config_get(&mesh);
        char json[512];
        if (!mesh.configured) airlink_usb_write_cli("OK mesh off unconfigured\r\n");
        else if (airlink_mesh_config_export_json(&mesh, false, json, sizeof(json)) != ESP_OK) {
            airlink_usb_write_cli("ERR mesh config unreadable\r\n");
        } else {
            airlink_usb_write_cli("OK mesh generation=");
            char generation[24]; snprintf(generation, sizeof(generation), "%" PRIu32 "\r\n", airlink_mesh_config_generation());
            airlink_usb_write_cli(generation); airlink_usb_write_cli(json); airlink_usb_write_cli("\r\n");
        }
    } else if (strncmp(line, "mesh create ", 12) == 0) {
        airlink_config_t base; airlink_config_get(&base);
        airlink_mesh_role_t role = strcmp(line + 12, "air") == 0 ? AIRLINK_MESH_ROLE_AIR :
            strcmp(line + 12, "ground_root") == 0 ? AIRLINK_MESH_ROLE_GROUND_ROOT : AIRLINK_MESH_ROLE_OFF;
        airlink_mesh_config_t mesh;
        if (role == AIRLINK_MESH_ROLE_OFF) airlink_usb_write_cli("ERR mesh role air|ground_root\r\n");
        else if (airlink_router_fc_armed() || base.bridge_role != AIRLINK_BRIDGE_OFF) {
            airlink_usb_write_cli("ERR armed or bridge_role must be off\r\n");
        } else if (airlink_mesh_config_create(role, &mesh) != ESP_OK ||
                   airlink_mesh_config_save(&mesh) != ESP_OK) {
            airlink_usb_write_cli("ERR could not create mesh network\r\n");
        } else {
            char json[512]; (void)airlink_mesh_config_export_json(&mesh, true, json, sizeof(json));
            airlink_usb_write_cli("OK created; protect this provisioning package; reboot required\r\n");
            airlink_usb_write_cli(json); airlink_usb_write_cli("\r\n");
        }
    } else if (strncmp(line, "mesh import ", 12) == 0) {
        const char *package = line + 12;
        airlink_mesh_role_t role = AIRLINK_MESH_ROLE_AIR;
        if (strncmp(package, "air ", 4) == 0) package += 4;
        else if (strncmp(package, "ground_root ", 12) == 0) {
            role = AIRLINK_MESH_ROLE_GROUND_ROOT; package += 12;
        } else if (strstr(package, "\"role\":\"ground_root\"") != NULL) {
            role = AIRLINK_MESH_ROLE_GROUND_ROOT;
        }
        airlink_mesh_config_t mesh;
        if (airlink_router_fc_armed()) airlink_usb_write_cli("ERR flight controller armed\r\n");
        else if (airlink_mesh_config_import_json(package, role, &mesh) != ESP_OK ||
                 airlink_mesh_config_save(&mesh) != ESP_OK) {
            airlink_usb_write_cli("ERR invalid mesh provisioning package\r\n");
        } else airlink_usb_write_cli("OK imported; reboot required\r\n");
    } else if (strncmp(line, "mesh role ", 10) == 0) {
        airlink_mesh_config_t mesh; airlink_mesh_config_get(&mesh);
        const char *role = line + 10;
        if (strcmp(role, "off") == 0) { mesh.configured = 0; mesh.role = AIRLINK_MESH_ROLE_OFF; }
        else if (!mesh.configured) { airlink_usb_write_cli("ERR create or import a network first\r\n"); return; }
        else if (strcmp(role, "air") == 0) mesh.role = AIRLINK_MESH_ROLE_AIR;
        else if (strcmp(role, "ground_root") == 0) mesh.role = AIRLINK_MESH_ROLE_GROUND_ROOT;
        else { airlink_usb_write_cli("ERR mesh role off|air|ground_root\r\n"); return; }
        if (airlink_router_fc_armed() || airlink_mesh_config_save(&mesh) != ESP_OK) {
            airlink_usb_write_cli("ERR could not change mesh role\r\n");
        } else airlink_usb_write_cli("OK mesh role saved; reboot required\r\n");
    } else if (strcmp(line, "mesh reset") == 0) {
        if (airlink_router_fc_armed() || airlink_mesh_config_reset() != ESP_OK) {
            airlink_usb_write_cli("ERR could not reset mesh configuration\r\n");
        } else airlink_usb_write_cli("OK mesh configuration reset; reboot required\r\n");
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
        (void)airlink_diag_mark_boot_stage("restarting");
        airlink_usb_write_cli("OK rebooting\r\n");
        vTaskDelay(pdMS_TO_TICKS(50));
        airlink_usb_system_restart();
    } else if (strcmp(line, "usb log") == 0 || strcmp(line, "usb mavlink") == 0) {
        if (airlink_router_fc_armed()) {
            airlink_usb_write_cli("ERR flight controller armed\r\n");
            return;
        }
        airlink_config_t config; airlink_config_get(&config);
        config.usb_mode = strcmp(line, "usb mavlink") == 0 ? AIRLINK_USB_MAVLINK : AIRLINK_USB_LOG_CLI;
        if (airlink_config_save(&config) == ESP_OK) {
            (void)airlink_diag_mark_boot_stage("restarting");
            airlink_usb_write_cli("OK mode saved; rebooting\r\n");
            vTaskDelay(pdMS_TO_TICKS(50));
            airlink_usb_system_restart();
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
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    uint8_t rx[USB_RX_CHUNK];
    char line[1024];
    size_t line_length = 0;
    uint8_t rpc_encoded[USB_RPC_FRAME_MAX + 32U];
    size_t rpc_encoded_length = 0;
    usb_packet_t packet;
    airlink_escape_matcher_t escape = {0};
    if (s_mode == AIRLINK_USB_LOG_CLI) airlink_usb_write_cli("\r\nAirLink LOG_CLI ready. Type help.\r\n> ");
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
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
                (void)airlink_diag_mark_boot_stage("ota-restarting");
                airlink_usb_write_cli("\r\nOK ota verified; rebooting\r\n");
                usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
                vTaskDelay(pdMS_TO_TICKS(100));
                airlink_usb_system_restart();
            }
            continue;
        }
        while (xQueueReceive(s_tx_queue, &packet, 0) == pdTRUE) {
            usb_serial_jtag_write_bytes(packet.data, packet.length, pdMS_TO_TICKS(5));
        }
        const int count = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(10));
        if (count <= 0) {
            if (s_binary_mode && (!usb_serial_jtag_is_connected() ||
                esp_timer_get_time() >= s_binary_deadline_us)) restore_usb_mavlink();
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
        if (s_binary_mode) {
            s_binary_deadline_us = esp_timer_get_time() + INT64_C(60000000);
            for (size_t i = cli_offset; i < (size_t)count; ++i) {
                if (rx[i] == 0) {
                    uint8_t decoded[USB_RPC_FRAME_MAX];
                    const size_t decoded_length = cobs_decode(rpc_encoded, rpc_encoded_length,
                                                               decoded, sizeof(decoded));
                    rpc_encoded_length = 0;
                    if (decoded_length >= sizeof(usb_rpc_header_t)) {
                        usb_rpc_header_t header; memcpy(&header, decoded, sizeof(header));
                        if (header.version == 1 && header.kind == USB_RPC_KIND_CONTROL &&
                            header.method == 0 && header.payload_length == 0 &&
                            decoded_length == sizeof(header) + sizeof(uint32_t)) {
                            uint32_t received_crc;
                            memcpy(&received_crc, decoded + sizeof(header), sizeof(received_crc));
                            if (received_crc == airlink_crc32(decoded, sizeof(header))) {
                                restore_usb_mavlink(); break;
                            }
                        }
                        rpc_handle(decoded, decoded_length);
                    }
                } else if (rpc_encoded_length < sizeof(rpc_encoded)) {
                    rpc_encoded[rpc_encoded_length++] = rx[i];
                } else {
                    rpc_encoded_length = 0;
                }
            }
            continue;
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
            .direction = AIRLINK_ENDPOINT_DIRECTION_GCS,
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
