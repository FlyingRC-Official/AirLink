// SPDX-License-Identifier: Apache-2.0
#include "airlink_usb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "airlink_config.h"
#include "airlink_router.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define USB_QUEUE_DEPTH 32
typedef struct { uint16_t length; uint8_t data[AIRLINK_MAX_FRAME_SIZE]; } usb_packet_t;

static airlink_usb_mode_t s_mode;
static QueueHandle_t s_tx_queue;
static airlink_usb_cli_handler_t s_cli_handler;
static vprintf_like_t s_console_vprintf;

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
        airlink_usb_write_cli("commands: help, reboot, usb log, usb mavlink, factory ...\r\n");
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
            airlink_router_ingest(AIRLINK_ENDPOINT_ID_USB, rx, (size_t)count);
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
    return xTaskCreate(usb_task, "usb_mux", 4096, NULL, 12, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void airlink_usb_set_cli_handler(airlink_usb_cli_handler_t handler) { s_cli_handler = handler; }
bool airlink_usb_connected(void) { return usb_serial_jtag_is_connected(); }
