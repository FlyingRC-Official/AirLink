// SPDX-License-Identifier: Apache-2.0
#include "airlink_uart.h"

#include <string.h>
#include "airlink_board.h"
#include "airlink_core.h"
#include "airlink_router.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define FC_UART UART_NUM_1
#define RX_BUFFER_SIZE (16 * 1024)
#define TX_BUFFER_SIZE (8 * 1024)
#define UART_EVENT_DEPTH 24
#define HIGH_QUEUE_DEPTH 32
#define NORMAL_QUEUE_DEPTH 64
#define HIGH_BURST_LIMIT 8U

typedef struct {
    uint16_t length;
    uint8_t data[AIRLINK_MAX_FRAME_SIZE];
} uart_packet_t;

static const char *TAG = "fc_uart";
static QueueHandle_t s_events;
static QueueHandle_t s_high_queue;
static QueueHandle_t s_normal_queue;
static airlink_uart_health_t s_health;
static TaskHandle_t s_rx_task;

static esp_err_t uart_router_send(const uint8_t *data, size_t length,
                                  bool high_priority, void *context)
{
    (void)context;
    if (length > AIRLINK_MAX_FRAME_SIZE) return ESP_ERR_INVALID_SIZE;
    uart_packet_t packet = {.length = (uint16_t)length};
    memcpy(packet.data, data, length);
    QueueHandle_t queue = high_priority ? s_high_queue : s_normal_queue;
    if (xQueueSend(queue, &packet, 0) == pdTRUE) return ESP_OK;
    if (!high_priority) {
        uart_packet_t discarded;
        if (xQueueReceive(s_normal_queue, &discarded, 0) == pdTRUE &&
            xQueueSend(s_normal_queue, &packet, 0) == pdTRUE) {
            s_health.normal_queue_drops++;
            return ESP_OK;
        }
        s_health.normal_queue_drops++;
    } else {
        s_health.high_queue_drops++;
    }
    return ESP_ERR_NO_MEM;
}

static void uart_tx_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    uart_packet_t packet;
    unsigned high_burst = 0;
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        bool received = false;
        if (high_burst >= HIGH_BURST_LIMIT &&
            xQueueReceive(s_normal_queue, &packet, 0) == pdTRUE) {
            high_burst = 0;
            received = true;
        } else if (xQueueReceive(s_high_queue, &packet, 0) == pdTRUE) {
            high_burst++;
            received = true;
        } else if (xQueueReceive(s_normal_queue, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
            high_burst = 0;
            received = true;
        }
        if (!received) continue;
        const int written = uart_write_bytes(FC_UART, packet.data, packet.length);
        if (written != packet.length) ESP_LOGW(TAG, "short UART write %d/%u", written, packet.length);
    }
}

static void uart_rx_task(void *argument)
{
    (void)argument;
    uint8_t buffer[512];
    uart_event_t event;
    while (true) {
        if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.type == UART_DATA) {
            size_t remaining = event.size;
            while (remaining > 0) {
                const size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
                const int received = uart_read_bytes(FC_UART, buffer, request, pdMS_TO_TICKS(20));
                if (received <= 0) break;
                airlink_router_ingest(AIRLINK_ENDPOINT_ID_FC_UART, buffer, (size_t)received);
                airlink_board_act_pulse();
                remaining -= (size_t)received;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            s_health.rx_overflow++;
            uart_flush_input(FC_UART);
            xQueueReset(s_events);
            ESP_LOGW(TAG, "RX overflow recovered");
        } else if (event.type == UART_BREAK || event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
            ESP_LOGW(TAG, "UART error event=%d", event.type);
        }
    }
}

esp_err_t airlink_uart_start(uint32_t baud)
{
    const uart_config_t config = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(FC_UART, &config), TAG, "UART config");
    ESP_RETURN_ON_ERROR(uart_set_pin(FC_UART, AIRLINK_GPIO_FC_TX, AIRLINK_GPIO_FC_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "UART pins");
    ESP_RETURN_ON_ERROR(uart_driver_install(FC_UART, RX_BUFFER_SIZE, TX_BUFFER_SIZE,
                                            UART_EVENT_DEPTH, &s_events, 0), TAG, "UART driver");
    s_high_queue = xQueueCreate(HIGH_QUEUE_DEPTH, sizeof(uart_packet_t));
    s_normal_queue = xQueueCreate(NORMAL_QUEUE_DEPTH, sizeof(uart_packet_t));
    if (s_high_queue == NULL || s_normal_queue == NULL) return ESP_ERR_NO_MEM;
    const airlink_router_endpoint_t endpoint = {
        .id = AIRLINK_ENDPOINT_ID_FC_UART,
        .type = AIRLINK_ENDPOINT_UART,
        .send = uart_router_send,
        .name = "flight-controller-uart",
    };
    ESP_RETURN_ON_ERROR(airlink_router_register(&endpoint), TAG, "register UART endpoint");
    if (xTaskCreate(uart_rx_task, "fc_uart_rx", 4096, NULL, 18, &s_rx_task) != pdPASS ||
        xTaskCreate(uart_tx_task, "fc_uart_tx", 4096, NULL, 19, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t airlink_uart_set_baud(uint32_t baud)
{
    return uart_set_baudrate(FC_UART, baud);
}

void airlink_uart_get_health(airlink_uart_health_t *health)
{
    if (health != NULL) *health = s_health;
}

esp_err_t airlink_uart_factory_loopback(size_t length, uint32_t *errors)
{
    if (length == 0 || length > 65536 || errors == NULL) return ESP_ERR_INVALID_ARG;
    *errors = 0;
    if (s_rx_task == NULL) return ESP_ERR_INVALID_STATE;
    vTaskSuspend(s_rx_task);
    uart_flush_input(FC_UART);
    xQueueReset(s_events);
    uint32_t state = UINT32_C(0x1aceb00c);
    uint8_t tx[128], rx[128];
    size_t complete = 0;
    while (complete < length) {
        const size_t chunk = length - complete < sizeof(tx) ? length - complete : sizeof(tx);
        for (size_t i = 0; i < chunk; ++i) {
            state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
            tx[i] = (uint8_t)state;
        }
        if (uart_write_bytes(FC_UART, tx, chunk) != (int)chunk ||
            uart_wait_tx_done(FC_UART, pdMS_TO_TICKS(100)) != ESP_OK) {
            uart_flush_input(FC_UART);
            xQueueReset(s_events);
            vTaskResume(s_rx_task);
            return ESP_FAIL;
        }
        size_t received_total = 0;
        while (received_total < chunk) {
            const int received = uart_read_bytes(FC_UART, rx + received_total,
                                                 chunk - received_total, pdMS_TO_TICKS(100));
            if (received <= 0) { *errors += (uint32_t)(chunk - received_total); break; }
            received_total += (size_t)received;
        }
        for (size_t i = 0; i < received_total; ++i) if (rx[i] != tx[i]) (*errors)++;
        complete += chunk;
    }
    uart_flush_input(FC_UART);
    xQueueReset(s_events);
    vTaskResume(s_rx_task);
    return ESP_OK;
}
