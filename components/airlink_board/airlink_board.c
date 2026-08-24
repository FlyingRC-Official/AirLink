// SPDX-License-Identifier: Apache-2.0
#include "airlink_board.h"

#include <inttypes.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "board";
static esp_timer_handle_t s_act_timer;
static int64_t s_act_last_us;

static void act_off(void *argument)
{
    (void)argument;
    gpio_set_level(AIRLINK_GPIO_ACT_LED, 0);
}

bool airlink_board_boot_pressed(void)
{
    return gpio_get_level(AIRLINK_GPIO_BOOT) == 0;
}

void airlink_board_act_pulse(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_act_last_us < INT64_C(50000)) return;
    s_act_last_us = now;
    gpio_set_level(AIRLINK_GPIO_ACT_LED, 1);
    if (esp_timer_is_active(s_act_timer)) esp_timer_stop(s_act_timer);
    esp_timer_start_once(s_act_timer, 20000);
}

esp_err_t airlink_board_init(airlink_board_probe_t *probe)
{
    if (probe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *probe = (airlink_board_probe_t){0};

    const gpio_config_t inputs = {
        .pin_bit_mask = UINT64_C(1) << AIRLINK_GPIO_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const gpio_config_t outputs = {
        .pin_bit_mask = UINT64_C(1) << AIRLINK_GPIO_ACT_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&inputs), TAG, "BOOT GPIO setup failed");
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "ACT GPIO setup failed");
    const esp_timer_create_args_t timer_args = {.callback = act_off, .name = "act_led"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_act_timer), TAG, "ACT timer setup failed");

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    probe->chip_ok = chip.model == CHIP_ESP32C5;
    uint32_t flash_bytes = 0;
    ESP_RETURN_ON_ERROR(esp_flash_get_size(NULL, &flash_bytes), TAG, "flash probe failed");
    probe->flash_bytes = flash_bytes;
    probe->flash_ok = flash_bytes == 8U * 1024U * 1024U;
    probe->psram_bytes = esp_psram_get_size();
    probe->psram_ok = esp_psram_is_initialized() &&
                      probe->psram_bytes == 8U * 1024U * 1024U;
    probe->recovery_requested = airlink_board_boot_pressed();
#if CONFIG_AIRLINK_FORCE_RECOVERY
    probe->recovery_requested = true;
#endif

    ESP_LOGI(TAG, "chip=%s flash=%" PRIu32 " psram=%" PRIu32 " recovery=%d",
             probe->chip_ok ? "esp32c5" : "unexpected", probe->flash_bytes,
             probe->psram_bytes, probe->recovery_requested);
    return ESP_OK;
}
