// SPDX-License-Identifier: Apache-2.0
#include "airlink_board.h"
#include "airlink_can.h"
#include "airlink_config.h"
#include "airlink_diag.h"
#include "airlink_led.h"
#include "airlink_mesh.h"
#include "airlink_mesh_config.h"
#include "airlink_ota.h"
#include "airlink_router.h"
#include "airlink_uart.h"
#include "airlink_usb.h"
#include "airlink_api.h"
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
    bool mesh_mode;
    bool mesh_started;
    bool api_started;
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
    if (!s_services.normal_mode || !s_services.usb_started || !airlink_usb_ready()) return false;
    if (s_services.mesh_mode) {
        if (!s_services.mesh_started || !airlink_mesh_ready()) return false;
    } else {
        if (!s_services.wifi_started || !s_services.api_started || !airlink_api_ready()) return false;
        if (wifi == NULL || (!wifi->ap_started && !wifi->sta_connected)) return false;
    }
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
    bool healthy_stage_recorded = false;
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        airlink_wifi_status_t wifi = {0};
        if (!s_services.mesh_mode) airlink_wifi_get_status(&wifi);
        const bool healthy = services_healthy(&wifi);
        if (healthy && !healthy_stage_recorded) {
            if (airlink_diag_mark_boot_stage("healthy") == ESP_OK) healthy_stage_recorded = true;
        }
        airlink_ota_services_ready(healthy);
        airlink_ota_health_heartbeat(healthy);
        if (s_services.led_started) {
            if (airlink_ota_in_progress()) airlink_led_set(AIRLINK_LED_OTA);
            else if (s_services.mesh_mode && !airlink_mesh_ready()) airlink_led_set(AIRLINK_LED_MESH);
            else if (airlink_router_fc_seen()) airlink_led_set(AIRLINK_LED_MAVLINK);
            else if ((s_services.mesh_mode && airlink_mesh_ready()) || wifi.ap_started || wifi.sta_connected) airlink_led_set(AIRLINK_LED_NO_FC);
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
    airlink_board_probe_t board;
    ESP_ERROR_CHECK(airlink_board_init(&board));
    airlink_config_snapshot_t snapshot;
    ESP_ERROR_CHECK(airlink_config_init(&snapshot));
    airlink_mesh_config_snapshot_t mesh_snapshot;
    ESP_ERROR_CHECK(airlink_mesh_config_init(&mesh_snapshot));
    service_started("diagnostics", airlink_diag_init());
    const bool configured_mesh = mesh_snapshot.value.configured &&
                                 mesh_snapshot.value.role != AIRLINK_MESH_ROLE_OFF &&
                                 airlink_mesh_config_validate(&mesh_snapshot.value, &snapshot.value);
    if (mesh_snapshot.value.configured && mesh_snapshot.value.role != AIRLINK_MESH_ROLE_OFF &&
        !configured_mesh) {
        ESP_LOGE(TAG, "invalid or bridge-conflicting Mesh configuration ignored; use USB recovery");
    }
    ESP_ERROR_CHECK(airlink_router_init(configured_mesh ? AIRLINK_ROUTE_MAVLINK :
                                        snapshot.value.route_mode));
    s_services.led_started = service_started("status LED", airlink_led_start(snapshot.value.led_brightness));
    ESP_ERROR_CHECK(airlink_ota_init());
    (void)airlink_diag_mark_boot_stage("core-ready");

    const bool hardware_ok = board.chip_ok && board.flash_ok && board.psram_ok;
    const bool recovery = !hardware_ok || board.recovery_requested;
    s_services.mesh_mode = configured_mesh && hardware_ok && !recovery;
    const airlink_usb_mode_t usb_mode = recovery ? AIRLINK_USB_LOG_CLI :
        s_services.mesh_mode && mesh_snapshot.value.role == AIRLINK_MESH_ROLE_GROUND_ROOT ?
        AIRLINK_USB_MAVLINK :
        s_services.mesh_mode ? AIRLINK_USB_LOG_CLI : snapshot.value.usb_mode;
    s_services.normal_mode = hardware_ok && !recovery;
    s_services.usb_started = service_started("USB", airlink_usb_start(usb_mode));
    (void)airlink_diag_mark_boot_stage("usb-ready");

    /* Recovery keeps only USB LOG_CLI, Wi-Fi configuration access and the web
     * service alive. Telemetry UART/CAN endpoints never start in recovery. */
    const bool ground_bridge = snapshot.value.bridge_enabled &&
                               snapshot.value.bridge_role == AIRLINK_BRIDGE_GROUND;
    const bool can_fc = snapshot.value.fc_transport == AIRLINK_FC_TRANSPORT_DRONECAN;
    const bool mesh_air = s_services.mesh_mode &&
                          mesh_snapshot.value.role == AIRLINK_MESH_ROLE_AIR;
    const bool mesh_ground = s_services.mesh_mode &&
                             mesh_snapshot.value.role == AIRLINK_MESH_ROLE_GROUND_ROOT;
    s_services.uart_required = hardware_ok && !recovery &&
                               (mesh_air || (!mesh_ground && !ground_bridge && !can_fc));
    if (s_services.uart_required) {
        s_services.uart_started = service_started("flight-controller UART",
                                                   airlink_uart_start(snapshot.value.uart_baud));
    }
    s_services.can_required = hardware_ok && !recovery;
    if (s_services.can_required) {
        const airlink_can_options_t can_options = {
            .bitrate = snapshot.value.can_bitrate,
            .virtual_baud = snapshot.value.uart_baud,
            .local_node_id = snapshot.value.can_node_id,
            .remote_node_id = snapshot.value.can_remote_node_id,
            .serial_id = snapshot.value.can_serial_id,
            .tunnel_enabled = !s_services.mesh_mode && can_fc && !ground_bridge,
        };
        s_services.can_started = service_started("CAN",
                                                  airlink_can_start(&can_options));
    }
    (void)airlink_diag_mark_boot_stage("io-ready");
    if (s_services.mesh_mode) {
        s_services.mesh_started = service_started("Wi-Fi Mesh",
            airlink_mesh_start(&mesh_snapshot.value, &snapshot.value));
    } else {
        s_services.wifi_started = service_started("Wi-Fi", airlink_wifi_start(&snapshot.value));
    }
    (void)airlink_diag_mark_boot_stage("wifi-started");
    if (!s_services.mesh_mode) {
        s_services.api_started = service_started("management API", airlink_api_start(recovery, !hardware_ok));
    }
    (void)airlink_diag_mark_boot_stage("services-started");

    if (s_services.normal_mode) {
        ESP_LOGI(TAG, "normal telemetry services ready");
    } else {
        ESP_LOGW(TAG, "recovery mode: chip=%d flash=%d psram=%d boot=%d",
                 board.chip_ok, board.flash_ok, board.psram_ok, board.recovery_requested);
    }
    ESP_ERROR_CHECK(xTaskCreate(status_task, "status", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
