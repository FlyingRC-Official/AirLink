// SPDX-License-Identifier: Apache-2.0
#include "airlink_led.h"

#include "airlink_board.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static led_strip_handle_t s_strip;
static volatile airlink_led_state_t s_state = AIRLINK_LED_WAIT_WIFI;
static uint8_t s_scale;
static volatile bool s_error_latched;

static void set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    red = (uint8_t)((uint16_t)red * s_scale / 100U);
    green = (uint8_t)((uint16_t)green * s_scale / 100U);
    blue = (uint8_t)((uint16_t)blue * s_scale / 100U);
    led_strip_set_pixel(s_strip, 0, red, green, blue);
    led_strip_refresh(s_strip);
}

static void led_task(void *argument)
{
    (void)argument;
    bool phase = false;
    while (true) {
        phase = !phase;
        switch (s_state) {
            case AIRLINK_LED_ERROR: set_rgb(phase ? 255 : 0, 0, 0); break;
            case AIRLINK_LED_OTA: set_rgb(phase ? 255 : 0, phase ? 255 : 0, phase ? 255 : 0); break;
            case AIRLINK_LED_MESH: set_rgb(150, 0, 255); break;
            case AIRLINK_LED_MAVLINK: set_rgb(0, phase ? 255 : 40, 0); break;
            case AIRLINK_LED_CONNECTED: set_rgb(0, 0, 255); break;
            case AIRLINK_LED_NO_FC: set_rgb(255, 110, 0); break;
            default: set_rgb(0, 0, phase ? 180 : 10); break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t airlink_led_start(uint8_t brightness)
{
    s_scale = brightness;
    const led_strip_config_t strip = {
        .strip_gpio_num = AIRLINK_GPIO_WS2812,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10U * 1000U * 1000U,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip, &rmt, &s_strip);
    if (err != ESP_OK) return err;
    return xTaskCreate(led_task, "status_led", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void airlink_led_set(airlink_led_state_t state)
{
    if (state == AIRLINK_LED_ERROR) s_error_latched = true;
    if (!s_error_latched || state == AIRLINK_LED_ERROR) s_state = state;
}

void airlink_led_clear_error(void) { s_error_latched = false; }
