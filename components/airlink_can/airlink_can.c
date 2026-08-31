// SPDX-License-Identifier: Apache-2.0
#include "airlink_can.h"
#include "airlink_dronecan.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "airlink_board.h"
#include "airlink_core.h"
#include "airlink_led.h"
#include "airlink_router.h"
#include "canard.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "uavcan.protocol.GetNodeInfo.h"
#include "uavcan.protocol.NodeStatus.h"
#include "uavcan.tunnel.Targetted.h"

#define CANARD_ARENA_SIZE (16U * 1024U)
#define RX_QUEUE_DEPTH 128U
#define HIGH_QUEUE_DEPTH 16U
#define NORMAL_QUEUE_DEPTH 32U
#define HIGH_BURST_LIMIT 8U
#define NODE_STATUS_INTERVAL_US INT64_C(1000000)
#define PEER_TIMEOUT_US INT64_C(3000000)
#define NODE_NAME "com.flyingrc.airlink"

typedef struct { twai_frame_header_t header; uint8_t data[8]; } rx_item_t;
typedef struct {
    bool seen;
    uint8_t health;
    uint8_t mode;
    uint32_t uptime;
    int64_t last_seen_us;
    uint32_t frames;
} dronecan_node_t;
typedef struct {
    uint16_t length;
    bool high_priority;
    uint8_t data[AIRLINK_MAX_FRAME_SIZE];
} tunnel_packet_t;

static const char *TAG = "can";
static twai_node_handle_t s_node;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_high_queue;
static QueueHandle_t s_normal_queue;
static airlink_can_status_t s_status;
static dronecan_node_t s_nodes[128];
static atomic_bool s_recover_requested;
static bool s_factory_mode;
static bool s_tunnel_enabled;
static TaskHandle_t s_can_task;
static CanardInstance s_canard;
static _Alignas(max_align_t) uint8_t s_canard_arena[CANARD_ARENA_SIZE];
static uint8_t s_local_node_id;
static uint8_t s_remote_node_id;
static int8_t s_serial_id;
static uint32_t s_virtual_baud;
static int64_t s_peer_last_seen_us;
static uint8_t s_targetted_transfer_id;
static uint8_t s_node_status_transfer_id;
/* ESP-IDF's TWAI v2 driver queues frame pointers rather than copying their
 * contents. Keep one frame and its payload alive until on_tx_done releases
 * the slot; stack-backed frames can otherwise be dereferenced by the ISR
 * after their caller has returned. */
static twai_frame_t s_tx_frame;
static uint8_t s_tx_data[8];
static atomic_bool s_tx_busy;

static inline void increment_saturated_u32(uint32_t *value)
{
    if (*value != UINT32_MAX) (*value)++;
}

static inline void add_saturated_u64(uint64_t *value, size_t increment)
{
    *value = UINT64_MAX - *value < increment ? UINT64_MAX : *value + increment;
}

static bool should_accept(const CanardInstance *instance, uint64_t *signature,
                          uint16_t data_type_id, CanardTransferType transfer_type,
                          uint8_t source_node_id)
{
    (void)instance;
    (void)source_node_id;
    if (transfer_type == CanardTransferTypeBroadcast) {
        if (data_type_id == UAVCAN_PROTOCOL_NODESTATUS_ID) {
            *signature = UAVCAN_PROTOCOL_NODESTATUS_SIGNATURE;
            return true;
        }
        if (s_tunnel_enabled && data_type_id == UAVCAN_TUNNEL_TARGETTED_ID) {
            *signature = UAVCAN_TUNNEL_TARGETTED_SIGNATURE;
            return true;
        }
    }
    if (transfer_type == CanardTransferTypeRequest &&
        data_type_id == UAVCAN_PROTOCOL_GETNODEINFO_ID) {
        *signature = UAVCAN_PROTOCOL_GETNODEINFO_SIGNATURE;
        return true;
    }
    return false;
}

static struct uavcan_protocol_NodeStatus local_node_status(void)
{
    return (struct uavcan_protocol_NodeStatus){
        .uptime_sec = (uint32_t)(esp_timer_get_time() / INT64_C(1000000)),
        .health = UAVCAN_PROTOCOL_NODESTATUS_HEALTH_OK,
        .mode = UAVCAN_PROTOCOL_NODESTATUS_MODE_OPERATIONAL,
        .sub_mode = 0,
        .vendor_specific_status_code = s_tunnel_enabled ? 1U : 0U,
    };
}

static void send_get_node_info(CanardRxTransfer *transfer)
{
    uint8_t buffer[UAVCAN_PROTOCOL_GETNODEINFO_RESPONSE_MAX_SIZE];
    struct uavcan_protocol_GetNodeInfoResponse response = {0};
    response.status = local_node_status();
    const esp_app_desc_t *app = esp_app_get_description();
    unsigned major = 0, minor = 3;
    if (app != NULL) (void)sscanf(app->version, "%u.%u", &major, &minor);
    response.software_version.major = (uint8_t)major;
    response.software_version.minor = (uint8_t)minor;
    response.hardware_version.major = 1;
    response.hardware_version.minor = 0;
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        memcpy(response.hardware_version.unique_id, mac, sizeof(mac));
        memcpy(response.hardware_version.unique_id + 8, mac, sizeof(mac));
    }
    response.name.len = (uint8_t)strlen(NODE_NAME);
    memcpy(response.name.data, NODE_NAME, response.name.len);
    const uint16_t size = (uint16_t)uavcan_protocol_GetNodeInfoResponse_encode(
        &response, buffer);
    const int16_t result = canardRequestOrRespond(
        &s_canard, transfer->source_node_id, UAVCAN_PROTOCOL_GETNODEINFO_SIGNATURE,
        UAVCAN_PROTOCOL_GETNODEINFO_ID, &transfer->transfer_id, transfer->priority,
        CanardResponse, buffer, size);
    if (result < 0) increment_saturated_u32(&s_status.tunnel_drops);
}

static void observe_node_status(CanardRxTransfer *transfer)
{
    if (transfer->source_node_id == 0 || transfer->source_node_id > CANARD_MAX_NODE_ID) return;
    struct uavcan_protocol_NodeStatus status = {0};
    if (uavcan_protocol_NodeStatus_decode(transfer, &status)) return;
    dronecan_node_t *node = &s_nodes[transfer->source_node_id];
    node->uptime = status.uptime_sec;
    node->health = status.health;
    node->mode = status.mode;
    node->seen = true;
    node->last_seen_us = esp_timer_get_time();
    increment_saturated_u32(&node->frames);
}

static void receive_targetted(CanardRxTransfer *transfer)
{
    struct uavcan_tunnel_Targetted message = {0};
    if (uavcan_tunnel_Targetted_decode(transfer, &message) ||
        !airlink_dronecan_targetted_matches(transfer->source_node_id,
                                            message.protocol.protocol,
                                            message.target_node,
                                            message.serial_id,
                                            s_local_node_id,
                                            s_remote_node_id,
                                            s_serial_id)) {
        increment_saturated_u32(&s_status.tunnel_drops);
        return;
    }
    s_peer_last_seen_us = esp_timer_get_time();
    increment_saturated_u32(&s_status.tunnel_rx_transfers);
    add_saturated_u64(&s_status.tunnel_rx_bytes, message.buffer.len);
    if (message.buffer.len > 0U) {
        const esp_err_t err = airlink_router_ingest(AIRLINK_ENDPOINT_ID_FC_CAN,
                                                     message.buffer.data,
                                                     message.buffer.len);
        if (err != ESP_OK) increment_saturated_u32(&s_status.tunnel_drops);
        else airlink_board_act_pulse();
    }
}

static void on_transfer(CanardInstance *instance, CanardRxTransfer *transfer)
{
    (void)instance;
    if (transfer->transfer_type == CanardTransferTypeBroadcast &&
        transfer->data_type_id == UAVCAN_PROTOCOL_NODESTATUS_ID) {
        observe_node_status(transfer);
    } else if (transfer->transfer_type == CanardTransferTypeBroadcast &&
               transfer->data_type_id == UAVCAN_TUNNEL_TARGETTED_ID) {
        receive_targetted(transfer);
    } else if (transfer->transfer_type == CanardTransferTypeRequest &&
               transfer->data_type_id == UAVCAN_PROTOCOL_GETNODEINFO_ID) {
        send_get_node_info(transfer);
    }
}

static bool IRAM_ATTR on_rx(twai_node_handle_t handle,
                            const twai_rx_done_event_data_t *event, void *context)
{
    (void)event;
    (void)context;
    rx_item_t item = {0};
    twai_frame_t frame = {.buffer = item.data, .buffer_len = sizeof(item.data)};
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK || frame.header.fdf ||
        frame.header.dlc > 8) return false;
    item.header = frame.header;
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_rx_queue, &item, &woken) != pdTRUE) {
        increment_saturated_u32(&s_status.tunnel_drops);
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_tx_done(twai_node_handle_t handle,
                                 const twai_tx_done_event_data_t *event,
                                 void *context)
{
    (void)handle;
    (void)context;
    if (event->done_tx_frame == &s_tx_frame) {
        atomic_store_explicit(&s_tx_busy, false, memory_order_release);
    }
    return false;
}

static bool IRAM_ATTR on_state(twai_node_handle_t handle,
                               const twai_state_change_event_data_t *event, void *context)
{
    (void)handle;
    (void)context;
    if (event->new_sta == TWAI_ERROR_BUS_OFF) {
        increment_saturated_u32(&s_status.bus_off_count);
        atomic_store(&s_recover_requested, true);
    }
    return false;
}

static bool IRAM_ATTR on_error(twai_node_handle_t handle,
                               const twai_error_event_data_t *event, void *context)
{
    (void)handle;
    (void)context;
    if (event->err_flags.arb_lost) increment_saturated_u32(&s_status.arbitration_lost);
    /* Losing arbitration is expected when multiple healthy CAN nodes begin a
     * frame together. Report only actual bit/form/stuff/ACK faults as bus
     * errors so the two counters remain operationally meaningful. */
    if (event->err_flags.bit_err || event->err_flags.form_err ||
        event->err_flags.stuff_err || event->err_flags.ack_err) {
        increment_saturated_u32(&s_status.bus_errors);
    }
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
    const int16_t result = canardHandleRxFrame(&s_canard, &frame,
                                               (uint64_t)esp_timer_get_time());
    if (result < 0 && result != -CANARD_ERROR_RX_NOT_WANTED) {
        increment_saturated_u32(&s_status.dronecan_errors);
    }
}

static esp_err_t submit_frame(uint32_t id, bool extended,
                              const uint8_t *data, size_t length)
{
    if (data == NULL || length > sizeof(s_tx_data)) return ESP_ERR_INVALID_ARG;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_tx_busy, &expected, true,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_tx_data, data, length);
    s_tx_frame = (twai_frame_t){
        .header = {
            .id = id,
            .dlc = (uint8_t)length,
            .ide = extended,
            .fdf = false,
        },
        .buffer = s_tx_data,
        .buffer_len = length,
    };
    const esp_err_t err = twai_node_transmit(s_node, &s_tx_frame, 0);
    if (err != ESP_OK) {
        atomic_store_explicit(&s_tx_busy, false, memory_order_release);
    }
    return err;
}

static bool drain_canard_tx(void)
{
    const CanardCANFrame *pending = canardPeekTxQueue(&s_canard);
    if (pending == NULL ||
        submit_frame(pending->id & CANARD_CAN_EXT_ID_MASK, true,
                     pending->data, pending->data_len) != ESP_OK) return false;
    canardPopTxQueue(&s_canard);
    increment_saturated_u32(&s_status.tx_frames);
    return true;
}

static bool canard_tx_idle(void)
{
    return canardPeekTxQueue(&s_canard) == NULL;
}

static bool broadcast_targetted(const uint8_t *data, uint8_t length, uint8_t priority)
{
    struct uavcan_tunnel_Targetted message = {0};
    message.protocol.protocol = UAVCAN_TUNNEL_PROTOCOL_MAVLINK2;
    message.target_node = s_remote_node_id;
    message.serial_id = s_serial_id;
    message.options = 0;
    message.baudrate = s_virtual_baud;
    message.buffer.len = length;
    if (length > 0U) memcpy(message.buffer.data, data, length);
    uint8_t payload[UAVCAN_TUNNEL_TARGETTED_MAX_SIZE];
    const uint16_t payload_size = (uint16_t)uavcan_tunnel_Targetted_encode(
        &message, payload);
    const int16_t result = canardBroadcast(
        &s_canard, UAVCAN_TUNNEL_TARGETTED_SIGNATURE, UAVCAN_TUNNEL_TARGETTED_ID,
        &s_targetted_transfer_id, priority,
        payload, payload_size);
    if (result < 0) {
        increment_saturated_u32(&s_status.tunnel_drops);
        return false;
    }
    increment_saturated_u32(&s_status.tunnel_tx_transfers);
    add_saturated_u64(&s_status.tunnel_tx_bytes, length);
    if (length == 0U) increment_saturated_u32(&s_status.keepalives);
    return true;
}

static void broadcast_node_status(void)
{
    struct uavcan_protocol_NodeStatus status = local_node_status();
    uint8_t payload[UAVCAN_PROTOCOL_NODESTATUS_MAX_SIZE];
    const uint16_t size = (uint16_t)uavcan_protocol_NodeStatus_encode(&status, payload);
    if (canardBroadcast(&s_canard, UAVCAN_PROTOCOL_NODESTATUS_SIGNATURE,
                        UAVCAN_PROTOCOL_NODESTATUS_ID, &s_node_status_transfer_id,
                        CANARD_TRANSFER_PRIORITY_LOW, payload, size) < 0) {
        increment_saturated_u32(&s_status.tunnel_drops);
    }
}

static esp_err_t can_router_send(const uint8_t *data, size_t length,
                                 bool high_priority, void *context)
{
    (void)context;
    if (!s_tunnel_enabled || data == NULL || length == 0 ||
        length > AIRLINK_MAX_FRAME_SIZE) return ESP_ERR_INVALID_ARG;
    tunnel_packet_t packet = {
        .length = (uint16_t)length,
        .high_priority = high_priority,
    };
    memcpy(packet.data, data, length);
    QueueHandle_t queue = high_priority ? s_high_queue : s_normal_queue;
    if (xQueueSend(queue, &packet, 0) == pdTRUE) return ESP_OK;
    if (!high_priority) {
        tunnel_packet_t discarded;
        if (xQueueReceive(s_normal_queue, &discarded, 0) == pdTRUE &&
            xQueueSend(s_normal_queue, &packet, 0) == pdTRUE) {
            increment_saturated_u32(&s_status.normal_queue_drops);
            return ESP_OK;
        }
        increment_saturated_u32(&s_status.normal_queue_drops);
    } else {
        increment_saturated_u32(&s_status.high_queue_drops);
    }
    return ESP_ERR_NO_MEM;
}

static bool dequeue_tunnel_packet(tunnel_packet_t *packet, unsigned *high_burst)
{
    if (*high_burst >= HIGH_BURST_LIMIT &&
        xQueueReceive(s_normal_queue, packet, 0) == pdTRUE) {
        *high_burst = 0;
        return true;
    }
    if (xQueueReceive(s_high_queue, packet, 0) == pdTRUE) {
        (*high_burst)++;
        return true;
    }
    if (xQueueReceive(s_normal_queue, packet, 0) == pdTRUE) {
        *high_burst = 0;
        return true;
    }
    return false;
}

static void can_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    rx_item_t item;
    tunnel_packet_t packet = {0};
    size_t packet_offset = 0;
    bool packet_active = false;
    unsigned high_burst = 0;
    int64_t last_cleanup_us = 0;
    int64_t last_status_us = 0;
    int64_t last_keepalive_us = 0;
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        if (xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(2)) == pdTRUE) {
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
        (void)drain_canard_tx();
        const int64_t now = esp_timer_get_time();
        if (canard_tx_idle() && now - last_status_us >= NODE_STATUS_INTERVAL_US) {
            broadcast_node_status();
            last_status_us = now;
            (void)drain_canard_tx();
        }
        if (s_tunnel_enabled && canard_tx_idle() &&
            airlink_dronecan_keepalive_due((uint64_t)now,
                                           (uint64_t)last_keepalive_us)) {
            if (broadcast_targetted(NULL, 0, CANARD_TRANSFER_PRIORITY_MEDIUM)) {
                last_keepalive_us = now;
            }
            (void)drain_canard_tx();
        }
        if (s_tunnel_enabled && canard_tx_idle()) {
            if (!packet_active) {
                packet_active = dequeue_tunnel_packet(&packet, &high_burst);
                packet_offset = 0;
            }
            if (packet_active) {
                const size_t remaining = packet.length - packet_offset;
                const uint8_t chunk = (uint8_t)airlink_dronecan_chunk_size(remaining);
                const uint8_t priority = packet.high_priority ?
                    CANARD_TRANSFER_PRIORITY_HIGH : CANARD_TRANSFER_PRIORITY_MEDIUM;
                if (broadcast_targetted(packet.data + packet_offset, chunk, priority)) {
                    packet_offset += chunk;
                    packet_active = packet_offset < packet.length;
                } else {
                    packet_active = false;
                }
                (void)drain_canard_tx();
            }
        }
        twai_node_status_t status;
        if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
            s_status.tx_error_count = status.tx_error_count;
            s_status.rx_error_count = status.rx_error_count;
        }
        if (now - last_cleanup_us >= CANARD_RECOMMENDED_STALE_TRANSFER_CLEANUP_INTERVAL_USEC) {
            canardCleanupStaleTransfers(&s_canard, (uint64_t)now);
            last_cleanup_us = now;
        }
        uint8_t count = 0;
        for (size_t i = 1; i < 128; ++i) {
            if (s_nodes[i].seen && now - s_nodes[i].last_seen_us < INT64_C(5000000)) count++;
        }
        s_status.dronecan_nodes = count;
        s_status.peer_online = s_peer_last_seen_us > 0 &&
                               now - s_peer_last_seen_us < PEER_TIMEOUT_US;
    }
}

static esp_err_t create_node(uint32_t bitrate)
{
    const twai_onchip_node_config_t config = {
        .io_cfg = {.tx = AIRLINK_GPIO_CAN_TX, .rx = AIRLINK_GPIO_CAN_RX,
                   .quanta_clk_out = -1, .bus_off_indicator = -1},
        .bit_timing = {.bitrate = bitrate, .sp_permill = 800},
        .fail_retry_cnt = -1,
        .tx_queue_depth = 32,
    };
    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&config, &s_node), TAG, "create TWAI node");
    const twai_event_callbacks_t callbacks = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx,
        .on_state_change = on_state,
        .on_error = on_error,
    };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(s_node, &callbacks, NULL),
                        TAG, "TWAI callbacks");
    return twai_node_enable(s_node);
}

esp_err_t airlink_can_start(const airlink_can_options_t *options)
{
    if (options == NULL || options->local_node_id < 1U || options->local_node_id > 127U ||
        options->remote_node_id < 1U || options->remote_node_id > 127U ||
        options->local_node_id == options->remote_node_id || options->serial_id < 0 ||
        options->serial_id > 15) return ESP_ERR_INVALID_ARG;
    s_factory_mode = options->factory_mode;
    s_tunnel_enabled = options->tunnel_enabled && !options->factory_mode;
    s_local_node_id = options->local_node_id;
    s_remote_node_id = options->remote_node_id;
    s_serial_id = options->serial_id;
    s_virtual_baud = options->virtual_baud;
    canardInit(&s_canard, s_canard_arena, sizeof(s_canard_arena),
               on_transfer, should_accept, NULL);
    canardSetLocalNodeID(&s_canard, s_local_node_id);
    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
    if (s_rx_queue == NULL) return ESP_ERR_NO_MEM;
    if (s_tunnel_enabled) {
        s_high_queue = xQueueCreate(HIGH_QUEUE_DEPTH, sizeof(tunnel_packet_t));
        s_normal_queue = xQueueCreate(NORMAL_QUEUE_DEPTH, sizeof(tunnel_packet_t));
        if (s_high_queue == NULL || s_normal_queue == NULL) return ESP_ERR_NO_MEM;
        const airlink_router_endpoint_t endpoint = {
            .id = AIRLINK_ENDPOINT_ID_FC_CAN,
            .type = AIRLINK_ENDPOINT_CAN,
            .direction = AIRLINK_ENDPOINT_DIRECTION_VEHICLE,
            .send = can_router_send,
            .name = "flight-controller-dronecan",
        };
        ESP_RETURN_ON_ERROR(airlink_router_register(&endpoint), TAG, "register CAN endpoint");
    }
    ESP_RETURN_ON_ERROR(create_node(options->bitrate), TAG, "start TWAI node");
    return xTaskCreate(can_task, "dronecan", 6144, NULL, 18, &s_can_task) == pdPASS ?
           ESP_OK : ESP_ERR_NO_MEM;
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
    const int64_t deadline = esp_timer_get_time() + INT64_C(20000);
    esp_err_t err;
    do {
        err = submit_frame(id, extended, data, length);
        if (err != ESP_ERR_TIMEOUT) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() < deadline);
    if (err == ESP_OK) increment_saturated_u32(&s_status.tx_frames);
    return err;
}

void airlink_can_get_status(airlink_can_status_t *status)
{
    if (status != NULL) *status = s_status;
}

size_t airlink_can_json(char *output, size_t capacity)
{
    if (output == NULL || capacity < 3) return 0;
    char header[768];
    const int header_length = snprintf(
        header, sizeof(header),
        "{\"rx_frames\":%" PRIu32 ",\"tx_frames\":%" PRIu32
        ",\"bus_errors\":%" PRIu32 ",\"dronecan_errors\":%" PRIu32
        ",\"arbitration_lost\":%" PRIu32 ",\"tx_error_count\":%" PRIu16
        ",\"rx_error_count\":%" PRIu16 ",\"bus_off\":%" PRIu32
        ",\"tunnel_rx_bytes\":%" PRIu64 ",\"tunnel_tx_bytes\":%" PRIu64
        ",\"tunnel_rx_transfers\":%" PRIu32 ",\"tunnel_tx_transfers\":%" PRIu32
        ",\"tunnel_drops\":%" PRIu32 ",\"high_queue_drops\":%" PRIu32
        ",\"normal_queue_drops\":%" PRIu32 ",\"keepalives\":%" PRIu32
        ",\"peer_online\":%s,\"nodes\":[",
        s_status.rx_frames, s_status.tx_frames, s_status.bus_errors,
        s_status.dronecan_errors, s_status.arbitration_lost,
        s_status.tx_error_count, s_status.rx_error_count, s_status.bus_off_count,
        s_status.tunnel_rx_bytes, s_status.tunnel_tx_bytes,
        s_status.tunnel_rx_transfers, s_status.tunnel_tx_transfers,
        s_status.tunnel_drops, s_status.high_queue_drops,
        s_status.normal_queue_drops, s_status.keepalives,
        s_status.peer_online ? "true" : "false");
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
        const int entry_length = snprintf(
            entry, sizeof(entry),
            "%s{\"id\":%u,\"health\":%u,\"mode\":%u,\"uptime\":%" PRIu32
            ",\"frames\":%" PRIu32 "}",
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
