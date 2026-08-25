// SPDX-License-Identifier: Apache-2.0
#include "airlink_can.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "airlink_board.h"
#include "airlink_led.h"
#include "canard.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct { twai_frame_header_t header; uint8_t data[8]; } rx_item_t;
typedef struct {
    bool seen;
    uint8_t health;
    uint8_t mode;
    uint32_t uptime;
    int64_t last_seen_us;
    uint32_t frames;
} dronecan_node_t;

static const char *TAG = "can";
static twai_node_handle_t s_node;
static QueueHandle_t s_rx_queue;
static airlink_can_status_t s_status;
static dronecan_node_t s_nodes[128];
static atomic_bool s_recover_requested;
static bool s_factory_mode;
static TaskHandle_t s_can_task;
static CanardInstance s_canard;
static uint8_t s_canard_arena[2048];

static inline void increment_saturated_u32(uint32_t *value)
{
    if (*value != UINT32_MAX) (*value)++;
}

#define DRONECAN_NODE_STATUS_ID 341U
#define DRONECAN_NODE_STATUS_SIGNATURE UINT64_C(0x0f0868d0c1a7c6f1)

static bool should_accept(const CanardInstance *instance, uint64_t *signature,
                          uint16_t data_type_id, CanardTransferType transfer_type,
                          uint8_t source_node_id)
{
    (void)instance;
    (void)source_node_id;
    if (transfer_type == CanardTransferTypeBroadcast && data_type_id == DRONECAN_NODE_STATUS_ID) {
        *signature = DRONECAN_NODE_STATUS_SIGNATURE;
        return true;
    }
    return false;
}

static void on_transfer(CanardInstance *instance, CanardRxTransfer *transfer)
{
    (void)instance;
    if (transfer->data_type_id != DRONECAN_NODE_STATUS_ID || transfer->payload_len < 7 ||
        transfer->source_node_id == 0 || transfer->source_node_id > CANARD_MAX_NODE_ID) return;
    dronecan_node_t *node = &s_nodes[transfer->source_node_id];
    if (canardDecodeScalar(transfer, 0, 32, false, &node->uptime) != 32 ||
        canardDecodeScalar(transfer, 32, 2, false, &node->health) != 2 ||
        canardDecodeScalar(transfer, 34, 3, false, &node->mode) != 3) return;
    node->seen = true;
    node->last_seen_us = esp_timer_get_time();
    increment_saturated_u32(&node->frames);
}

static bool IRAM_ATTR on_rx(twai_node_handle_t handle, const twai_rx_done_event_data_t *event, void *context)
{
    (void)event; (void)context;
    rx_item_t item = {0};
    twai_frame_t frame = {.buffer = item.data, .buffer_len = sizeof(item.data)};
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK || frame.header.fdf || frame.header.dlc > 8) return false;
    item.header = frame.header;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_rx_queue, &item, &woken);
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_state(twai_node_handle_t handle, const twai_state_change_event_data_t *event, void *context)
{
    (void)handle; (void)context;
    if (event->new_sta == TWAI_ERROR_BUS_OFF) {
        increment_saturated_u32(&s_status.bus_off_count);
        atomic_store(&s_recover_requested, true);
    }
    return false;
}

static bool IRAM_ATTR on_error(twai_node_handle_t handle, const twai_error_event_data_t *event, void *context)
{
    (void)handle; (void)context;
    if (event->err_flags.arb_lost) increment_saturated_u32(&s_status.arbitration_lost);
    return false;
}

static void parse_dronecan(const rx_item_t *item)
{
    if (!item->header.ide || item->header.dlc == 0 || item->header.dlc > 8) return;
    CanardCANFrame frame = {
        .id = item->header.id | CANARD_CAN_FRAME_EFF,
        .data_len = item->header.dlc,
    };
    memcpy(frame.data, item->data, item->header.dlc);
    const int16_t result = canardHandleRxFrame(&s_canard, &frame, (uint64_t)esp_timer_get_time());
    if (result < 0 && result != -CANARD_ERROR_RX_NOT_WANTED) {
        increment_saturated_u32(&s_status.dronecan_errors);
    }
}

static void can_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    rx_item_t item;
    int64_t last_cleanup_us = 0;
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        if (xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            increment_saturated_u32(&s_status.rx_frames);
            parse_dronecan(&item);
            airlink_board_act_pulse();
        }
        if (atomic_exchange(&s_recover_requested, false)) {
            airlink_led_set(AIRLINK_LED_ERROR);
            vTaskDelay(pdMS_TO_TICKS(100));
            const esp_err_t err = twai_node_recover(s_node);
            if (err != ESP_OK) ESP_LOGW(TAG, "bus-off recovery: %s", esp_err_to_name(err));
        }
        twai_node_status_t status;
        twai_node_record_t record;
        if (twai_node_get_info(s_node, &status, &record) == ESP_OK) {
            s_status.bus_errors = record.bus_err_num;
            s_status.tx_error_count = status.tx_error_count;
            s_status.rx_error_count = status.rx_error_count;
        }
        uint8_t count = 0;
        const int64_t now = esp_timer_get_time();
        if (now - last_cleanup_us >= CANARD_RECOMMENDED_STALE_TRANSFER_CLEANUP_INTERVAL_USEC) {
            canardCleanupStaleTransfers(&s_canard, (uint64_t)now);
            last_cleanup_us = now;
        }
        for (size_t i = 1; i < 128; ++i) if (s_nodes[i].seen && now - s_nodes[i].last_seen_us < INT64_C(5000000)) count++;
        s_status.dronecan_nodes = count;
    }
}

static esp_err_t create_node(uint32_t bitrate)
{
    const twai_onchip_node_config_t config = {
        .io_cfg = {.tx = AIRLINK_GPIO_CAN_TX, .rx = AIRLINK_GPIO_CAN_RX,
                   .quanta_clk_out = -1, .bus_off_indicator = -1},
        .bit_timing = {.bitrate = bitrate, .sp_permill = 800},
        .fail_retry_cnt = -1,
        .tx_queue_depth = 16,
    };
    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&config, &s_node), TAG, "create TWAI node");
    const twai_event_callbacks_t callbacks = {
        .on_rx_done = on_rx,
        .on_state_change = on_state,
        .on_error = on_error,
    };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(s_node, &callbacks, NULL), TAG, "TWAI callbacks");
    return twai_node_enable(s_node);
}

esp_err_t airlink_can_start(uint32_t bitrate, bool factory_mode)
{
    s_factory_mode = factory_mode;
    canardInit(&s_canard, s_canard_arena, sizeof(s_canard_arena), on_transfer, should_accept, NULL);
    s_rx_queue = xQueueCreate(64, sizeof(rx_item_t));
    if (s_rx_queue == NULL) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(create_node(bitrate), TAG, "start TWAI node");
    return xTaskCreate(can_task, "can_diag", 4096, NULL, 10, &s_can_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool airlink_can_ready(void) { return s_node != NULL && s_can_task != NULL; }

esp_err_t airlink_can_factory_set_bitrate(uint32_t bitrate)
{
    if (!s_factory_mode) return ESP_ERR_NOT_ALLOWED;
    if (bitrate != 125000 && bitrate != 250000 && bitrate != 500000 && bitrate != 1000000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_can_task == NULL || s_node == NULL) return ESP_ERR_INVALID_STATE;
    vTaskSuspend(s_can_task);
    esp_err_t err = twai_node_disable(s_node);
    if (err == ESP_OK) {
        err = twai_node_delete(s_node);
        if (err == ESP_OK) s_node = NULL;
    }
    if (err == ESP_OK) err = create_node(bitrate);
    xQueueReset(s_rx_queue);
    vTaskResume(s_can_task);
    return err;
}

esp_err_t airlink_can_factory_transmit(uint32_t id, bool extended,
                                       const uint8_t *data, size_t length)
{
    if (!s_factory_mode) return ESP_ERR_NOT_ALLOWED;
    if (data == NULL || length > 8) return ESP_ERR_INVALID_ARG;
    uint8_t copy[8] = {0};
    memcpy(copy, data, length);
    twai_frame_t frame = {
        .header = {.id = id, .dlc = length, .ide = extended},
        .buffer = copy, .buffer_len = length,
    };
    const esp_err_t err = twai_node_transmit(s_node, &frame, 20);
    if (err == ESP_OK) increment_saturated_u32(&s_status.tx_frames);
    return err;
}

void airlink_can_get_status(airlink_can_status_t *status) { if (status != NULL) *status = s_status; }

size_t airlink_can_json(char *output, size_t capacity)
{
    if (output == NULL || capacity < 3) return 0;
    char header[256];
    const int header_length = snprintf(header, sizeof(header),
        "{\"rx_frames\":%" PRIu32 ",\"tx_frames\":%" PRIu32 ",\"bus_errors\":%" PRIu32
        ",\"dronecan_errors\":%" PRIu32 ",\"arbitration_lost\":%" PRIu32
        ",\"bus_off\":%" PRIu32 ",\"nodes\":[",
        s_status.rx_frames, s_status.tx_frames, s_status.bus_errors,
        s_status.dronecan_errors, s_status.arbitration_lost, s_status.bus_off_count);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        (size_t)header_length + 3U > capacity) {
        memcpy(output, "{}", 3);
        return 2;
    }
    size_t used = (size_t)header_length;
    memcpy(output, header, used + 1U);
    bool first = true;
    const int64_t now = esp_timer_get_time();
    for (size_t i = 1; i < 128; ++i) {
        if (!s_nodes[i].seen || now - s_nodes[i].last_seen_us >= INT64_C(5000000)) continue;
        char entry[128];
        const int entry_length = snprintf(entry, sizeof(entry),
            "%s{\"id\":%u,\"health\":%u,\"mode\":%u,\"uptime\":%" PRIu32 ",\"frames\":%" PRIu32 "}",
            first ? "" : ",", (unsigned)i, s_nodes[i].health, s_nodes[i].mode,
            s_nodes[i].uptime, s_nodes[i].frames);
        if (entry_length < 0 || (size_t)entry_length >= sizeof(entry) ||
            used + (size_t)entry_length + 3U > capacity) break;
        memcpy(output + used, entry, (size_t)entry_length);
        used += (size_t)entry_length;
        output[used] = '\0';
        first = false;
    }
    memcpy(output + used, "]}", 3);
    return used + 2U;
}
