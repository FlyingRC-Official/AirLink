// SPDX-License-Identifier: Apache-2.0
#include "airlink_factory.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airlink_can.h"
#include "airlink_config.h"
#include "airlink_factory_ble.h"
#include "airlink_led.h"
#include "airlink_uart.h"
#include "airlink_usb.h"
#include "esp_wifi.h"
#include "esp_mac.h"

static airlink_board_probe_t s_probe;
static bool s_enabled;

static void reply(const char *format, ...)
{
    char output[768];
    va_list args;
    va_start(args, format);
    vsnprintf(output, sizeof(output), format, args);
    va_end(args);
    airlink_usb_write_cli(output);
}

static void info_command(void)
{
    airlink_can_status_t can; airlink_can_get_status(&can);
    airlink_config_t config; airlink_config_get(&config);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    reply("{\"ok\":true,\"test\":\"info\",\"chip\":%s,\"flash_bytes\":%" PRIu32
          ",\"psram_bytes\":%" PRIu32 ",\"usb\":%s,\"boot_pressed\":%s,"
          "\"can_rx\":%" PRIu32 ",\"can_tx\":%" PRIu32 ",\"serial\":\"%s\","
          "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ble_advertising\":%s}\r\n",
          s_probe.chip_ok ? "true" : "false", s_probe.flash_bytes, s_probe.psram_bytes,
          airlink_usb_connected() ? "true" : "false", airlink_board_boot_pressed() ? "true" : "false",
          can.rx_frames, can.tx_frames, config.serial_number,
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
          airlink_factory_ble_advertising() ? "true" : "false");
}

static void scan_command(void)
{
    uint16_t count = 0, count_2g = 0, count_5g = 0;
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t *records = count > 0 ? calloc(count, sizeof(*records)) : NULL;
    if (err == ESP_OK && count > 0 && records == NULL) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && count > 0) err = esp_wifi_scan_get_ap_records(&count, records);
    for (size_t i = 0; i < count; ++i) {
        if (records[i].primary <= 14) count_2g++; else count_5g++;
    }
    free(records);
    reply("{\"ok\":%s,\"test\":\"wifi_scan\",\"aps_2g\":%u,\"aps_5g\":%u,\"error\":\"%s\"}\r\n",
          err == ESP_OK ? "true" : "false", count_2g, count_5g, esp_err_to_name(err));
}

static void cli_handler(const char *line)
{
    if (!s_enabled || strncmp(line, "factory ", 8) != 0) {
        airlink_usb_write_cli("ERR factory commands disabled\r\n");
        return;
    }
    const char *command = line + 8;
    if (strcmp(command, "info") == 0) {
        info_command();
    } else if (strcmp(command, "nvs") == 0) {
        airlink_config_t config; airlink_config_get(&config);
        const esp_err_t err = airlink_config_save(&config);
        reply("{\"ok\":%s,\"test\":\"nvs\",\"generation\":%" PRIu32 "}\r\n",
              err == ESP_OK ? "true" : "false", airlink_config_generation());
    } else if (strcmp(command, "wifi-scan") == 0) {
        scan_command();
    } else if (strncmp(command, "identity ", 9) == 0) {
        char serial[AIRLINK_SERIAL_MAX + 1], password[AIRLINK_PASSWORD_MAX + 1];
        const int parsed = sscanf(command + 9, "%24s %64s", serial, password);
        const esp_err_t err = parsed == 2 ? airlink_config_set_identity(serial, password) : ESP_ERR_INVALID_ARG;
        reply("{\"ok\":%s,\"test\":\"identity\",\"serial\":\"%s\"}\r\n",
              err == ESP_OK ? "true" : "false", parsed > 0 ? serial : "");
    } else if (strncmp(command, "uart-prbs ", 10) == 0) {
        unsigned baud = 0, requested = 0;
        const int parsed = sscanf(command + 10, "%u %u", &baud, &requested);
        const size_t length = requested;
        uint32_t errors = 0;
        esp_err_t err = parsed == 2 ? airlink_uart_set_baud(baud) : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) err = airlink_uart_factory_loopback(length, &errors);
        reply("{\"ok\":%s,\"test\":\"uart_prbs\",\"baud\":%u,\"bytes\":%u,\"errors\":%" PRIu32 "}\r\n",
              err == ESP_OK && errors == 0 ? "true" : "false", baud, (unsigned)length, errors);
    } else if (strncmp(command, "can-send ", 9) == 0) {
        unsigned id = 0, value = 0;
        const int parsed = sscanf(command + 9, "%x %x", &id, &value);
        const uint8_t data = (uint8_t)value;
        const esp_err_t err = parsed == 2 ? airlink_can_factory_transmit(id, id > 0x7ffU, &data, 1) : ESP_ERR_INVALID_ARG;
        reply("{\"ok\":%s,\"test\":\"can_send\",\"id\":%u}\r\n", err == ESP_OK ? "true" : "false", id);
    } else if (strncmp(command, "can-bitrate ", 12) == 0) {
        const uint32_t bitrate = (uint32_t)strtoul(command + 12, NULL, 10);
        const esp_err_t err = airlink_can_factory_set_bitrate(bitrate);
        reply("{\"ok\":%s,\"test\":\"can_bitrate\",\"bitrate\":%" PRIu32 "}\r\n",
              err == ESP_OK ? "true" : "false", bitrate);
    } else if (strncmp(command, "led ", 4) == 0) {
        airlink_led_clear_error();
        airlink_led_state_t state = AIRLINK_LED_WAIT_WIFI;
        if (strcmp(command + 4, "red") == 0) state = AIRLINK_LED_ERROR;
        else if (strcmp(command + 4, "green") == 0) state = AIRLINK_LED_MAVLINK;
        else if (strcmp(command + 4, "blue") == 0) state = AIRLINK_LED_CONNECTED;
        else if (strcmp(command + 4, "white") == 0) state = AIRLINK_LED_OTA;
        airlink_led_set(state);
        reply("{\"ok\":true,\"test\":\"led\",\"state\":\"%s\"}\r\n", command + 4);
    } else if (strcmp(command, "boot") == 0) {
        reply("{\"ok\":true,\"test\":\"boot\",\"pressed\":%s}\r\n",
              airlink_board_boot_pressed() ? "true" : "false");
    } else if (strcmp(command, "act") == 0) {
        airlink_board_act_pulse();
        reply("{\"ok\":true,\"test\":\"act_led\"}\r\n");
    } else if (strcmp(command, "ble") == 0) {
        reply("{\"ok\":%s,\"test\":\"ble_advertising\"}\r\n",
              airlink_factory_ble_advertising() ? "true" : "false");
    } else {
        airlink_usb_write_cli("ERR factory commands: info, identity, nvs, wifi-scan, uart-prbs BAUD BYTES, can-bitrate, can-send, led, act, boot, ble\r\n");
    }
}

esp_err_t airlink_factory_start(const airlink_board_probe_t *probe, bool enabled)
{
    if (probe == NULL) return ESP_ERR_INVALID_ARG;
    s_probe = *probe;
    s_enabled = enabled;
    airlink_usb_set_cli_handler(cli_handler);
    if (enabled) {
        const esp_err_t err = airlink_factory_ble_start();
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
