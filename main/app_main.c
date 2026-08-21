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

static void status_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        airlink_wifi_status_t wifi; airlink_wifi_get_status(&wifi);
        airlink_ota_health_heartbeat(true);
        if (airlink_ota_in_progress()) airlink_led_set(AIRLINK_LED_OTA);
        else if (airlink_router_fc_seen()) airlink_led_set(AIRLINK_LED_MAVLINK);
        else if (wifi.ap_started || wifi.sta_connected) airlink_led_set(AIRLINK_LED_NO_FC);
        else airlink_led_set(AIRLINK_LED_WAIT_WIFI);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
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
    ESP_ERROR_CHECK(airlink_diag_init());
    ESP_ERROR_CHECK(airlink_router_init(snapshot.value.route_mode));
    ESP_ERROR_CHECK(airlink_led_start(snapshot.value.led_brightness));
    ESP_ERROR_CHECK(airlink_ota_init());

    const bool hardware_ok = board.chip_ok && board.flash_ok && board.psram_ok;
    const bool recovery = !hardware_ok || board.recovery_requested;
    const airlink_usb_mode_t usb_mode = (recovery || factory_test) ?
                                        AIRLINK_USB_LOG_CLI : snapshot.value.usb_mode;
    ESP_ERROR_CHECK(airlink_usb_start(usb_mode));

    /* Recovery keeps only USB LOG_CLI, Wi-Fi configuration access and the web
     * service alive. Telemetry UART/CAN endpoints never start in recovery. */
    if (hardware_ok && !recovery) ESP_ERROR_CHECK(airlink_uart_start(snapshot.value.uart_baud));
    if (hardware_ok && !recovery) ESP_ERROR_CHECK(airlink_can_start(snapshot.value.can_bitrate, factory_test));
    ESP_ERROR_CHECK(airlink_wifi_start(&snapshot.value));
    ESP_ERROR_CHECK(airlink_web_start(recovery, !hardware_ok));
    ESP_ERROR_CHECK(airlink_factory_start(&board, factory_test));

    if (hardware_ok && !recovery) {
        airlink_ota_services_ready(true);
        ESP_LOGI(TAG, "normal telemetry services ready");
    } else {
        ESP_LOGW(TAG, "recovery mode: chip=%d flash=%d psram=%d boot=%d",
                 board.chip_ok, board.flash_ok, board.psram_ok, board.recovery_requested);
    }
    ESP_ERROR_CHECK(xTaskCreate(status_task, "status", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
