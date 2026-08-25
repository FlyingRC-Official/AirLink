// SPDX-License-Identifier: Apache-2.0
#include "airlink_board.h"
#include "airlink_can.h"
#include "airlink_config.h"
#include "airlink_diag.h"
#include "airlink_factory.h"
#include "airlink_led.h"
#include "airlink_ota.h"
#include "airlink_router.h"
#include "airlink_uart.h"
#include "airlink_usb.h"
#include "airlink_web.h"
#include "airlink_wifi.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "airlink";

typedef struct {
    bool normal_mode;
    bool usb_started;
    bool uart_required;
    bool uart_started;
    bool can_required;
    bool can_started;
    bool wifi_started;
    bool web_started;
    bool led_started;
} service_state_t;

static service_state_t s_services;

static bool service_started(const char *name, esp_err_t err)
{
    if (err == ESP_OK) return true;
    ESP_LOGE(TAG, "%s unavailable: %s; continuing in degraded mode",
             name, esp_err_to_name(err));
    return false;
}

static bool services_healthy(const airlink_wifi_status_t *wifi)
{
    if (!s_services.normal_mode || !s_services.usb_started ||
        !s_services.wifi_started || !s_services.web_started) return false;
    if (!airlink_usb_ready() || !airlink_web_ready()) return false;
    if (wifi == NULL || (!wifi->ap_started && !wifi->sta_connected)) return false;
    if (s_services.uart_required &&
        (!s_services.uart_started || !airlink_uart_ready())) return false;
    if (s_services.can_required &&
        (!s_services.can_started || !airlink_can_ready())) return false;
    return true;
}

static void status_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        airlink_wifi_status_t wifi = {0};
        airlink_wifi_get_status(&wifi);
        const bool healthy = services_healthy(&wifi);
        airlink_ota_services_ready(healthy);
        airlink_ota_health_heartbeat(healthy);
        if (s_services.led_started) {
            if (airlink_ota_in_progress()) airlink_led_set(AIRLINK_LED_OTA);
            else if (airlink_router_fc_seen()) airlink_led_set(AIRLINK_LED_MAVLINK);
            else if (wifi.ap_started || wifi.sta_connected) airlink_led_set(AIRLINK_LED_NO_FC);
            else airlink_led_set(AIRLINK_LED_WAIT_WIFI);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* Block host CDC control-line resets as soon as the application starts.
     * The USB CLI can open a bounded reset window for esptool when requested. */
    airlink_usb_reset_guard_enable();
    ESP_LOGI(TAG, "image target: %s", airlink_image_hardware_marker());
#if CONFIG_AIRLINK_BUILD_FACTORY_TEST
    const bool factory_test = true;
#else
    const bool factory_test = false;
#endif
    airlink_board_probe_t board;
    ESP_ERROR_CHECK(airlink_board_init(&board));
    airlink_config_snapshot_t snapshot;
    ESP_ERROR_CHECK(airlink_config_init(&snapshot));
    service_started("diagnostics", airlink_diag_init());
    ESP_ERROR_CHECK(airlink_router_init(snapshot.value.route_mode));
    s_services.led_started = service_started("status LED", airlink_led_start(snapshot.value.led_brightness));
    ESP_ERROR_CHECK(airlink_ota_init());

    const bool hardware_ok = board.chip_ok && board.flash_ok && board.psram_ok;
    const bool recovery = !hardware_ok || board.recovery_requested;
    const airlink_usb_mode_t usb_mode = (recovery || factory_test) ?
                                        AIRLINK_USB_LOG_CLI : snapshot.value.usb_mode;
    s_services.normal_mode = hardware_ok && !recovery;
    s_services.usb_started = service_started("USB", airlink_usb_start(usb_mode));

    /* Recovery keeps only USB LOG_CLI, Wi-Fi configuration access and the web
     * service alive. Telemetry UART/CAN endpoints never start in recovery. */
    const bool ground_bridge = snapshot.value.bridge_enabled &&
                               snapshot.value.bridge_role == AIRLINK_BRIDGE_GROUND;
    s_services.uart_required = hardware_ok && !recovery && !ground_bridge;
    if (s_services.uart_required) {
        s_services.uart_started = service_started("flight-controller UART",
                                                   airlink_uart_start(snapshot.value.uart_baud));
    }
    s_services.can_required = hardware_ok && !recovery;
    if (s_services.can_required) {
        s_services.can_started = service_started("CAN",
                                                  airlink_can_start(snapshot.value.can_bitrate, factory_test));
    }
    s_services.wifi_started = service_started("Wi-Fi", airlink_wifi_start(&snapshot.value));
    s_services.web_started = service_started("web API", airlink_web_start(recovery, !hardware_ok));
    service_started("factory service", airlink_factory_start(&board, factory_test));

    if (s_services.normal_mode) {
        ESP_LOGI(TAG, "normal telemetry services ready");
    } else {
        ESP_LOGW(TAG, "recovery mode: chip=%d flash=%d psram=%d boot=%d",
                 board.chip_ok, board.flash_ok, board.psram_ok, board.recovery_requested);
    }
    ESP_ERROR_CHECK(xTaskCreate(status_task, "status", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
