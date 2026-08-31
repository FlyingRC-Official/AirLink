// SPDX-License-Identifier: Apache-2.0
#include "airlink_mesh.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "airlink_mesh_codec.h"
#include "airlink_ota.h"
#include "airlink_router.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#define MESH_RX_TASK_STACK 8192
#define MESH_PERIODIC_TASK_STACK 6144
#define MESH_TX_TASK_STACK 6144
#define MESH_HIGH_QUEUE_DEPTH 32
#define MESH_NORMAL_QUEUE_DEPTH 64
#define NODE_STATUS_UNKNOWN_US INT64_C(3000000)
#define NODE_OFFLINE_US INT64_C(5000000)
#define GROUP_DEDUP_US INT64_C(5000)
#define OTA_CHUNK_SIZE 1024U
#define OTA_WINDOW_CHUNKS 32U
#define OTA_WINDOW_BYTES (OTA_CHUNK_SIZE * OTA_WINDOW_CHUNKS)
#define OTA_RATE_DELAY_MS 16U
#define OTA_ACK_TIMEOUT_MS 2500U
#define OTA_REPAIR_ROUNDS 3U

typedef struct __attribute__((packed)) {
    char serial[AIRLINK_MESH_SERIAL_SIZE + 1U];
    char firmware[24];
} hello_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t approved;
    uint8_t reason;
} approval_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t parent[6];
    uint8_t layer;
    int8_t rssi;
    uint8_t system_id;
    uint8_t system_id_known;
    uint8_t armed;
    uint8_t armed_known;
    uint32_t uptime_s;
    uint32_t queue_drops;
    uint32_t mesh_generation;
    int32_t ota_image_state;
} node_status_payload_t;

enum {
    CONFIG_KIND_TRANSACTION = 1,
    CONFIG_KIND_READ_REQUEST = 2,
    CONFIG_KIND_READ_RESPONSE = 3,
    CONFIG_KIND_WRITE_REQUEST = 4,
    CONFIG_KIND_WRITE_RESPONSE = 5,
    CONFIG_PHASE_PREPARE = 1,
    CONFIG_PHASE_COMMIT = 2,
    CONFIG_RESULT_OK = 0,
    CONFIG_RESULT_UNSAFE = 1,
    CONFIG_RESULT_INVALID = 2,
    CONFIG_RESULT_STORAGE = 3,
};

typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint32_t generation;
    uint8_t digest[32];
    airlink_mesh_config_t config;
} config_prepare_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint32_t generation;
    uint8_t digest[32];
    uint8_t phase;
    uint8_t result;
} config_response_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint32_t token;
} node_config_read_request_t;

typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint32_t token;
    uint8_t result;
    airlink_config_t config;
} node_config_response_t;

typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint32_t token;
    airlink_config_t config;
} node_config_write_request_t;

typedef struct __attribute__((packed)) {
    uint32_t generation;
    uint8_t digest[32];
} config_control_payload_t;

enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_READY = 1,
    OTA_STATE_RECEIVING = 2,
    OTA_STATE_VERIFIED = 3,
    OTA_STATE_ACTIVATING = 4,
    OTA_STATE_ERROR = 255,
};

typedef struct __attribute__((packed)) {
    uint32_t session;
    uint32_t image_size;
    uint8_t sha256[32];
    uint16_t chunk_size;
    uint8_t window_chunks;
    char expected_version[24];
} ota_begin_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t session;
    uint32_t chunk_index;
    uint16_t data_length;
} ota_chunk_header_t;

typedef struct __attribute__((packed)) {
    uint32_t session;
    uint32_t window_base;
    uint32_t bitmap;
    uint8_t state;
    uint8_t progress;
} ota_ack_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t session;
    uint8_t activate;
} ota_commit_payload_t;

typedef struct {
    bool active;
    bool activating;
    bool include_root;
    uint32_t session;
    size_t image_size;
    size_t bytes_received;
    uint32_t total_chunks;
    uint32_t window_base;
    uint32_t window_bitmap;
    uint32_t target_mask;
    uint8_t *window;
    uint16_t lengths[OTA_WINDOW_CHUNKS];
    char expected_version[24];
} root_ota_state_t;

typedef struct {
    bool active;
    uint32_t session;
    size_t image_size;
    uint32_t total_chunks;
    uint32_t window_base;
    uint32_t bitmap;
    uint8_t *window;
    uint16_t lengths[OTA_WINDOW_CHUNKS];
    bool window_written;
} air_ota_state_t;

typedef struct {
    uint16_t length;
    uint8_t high_priority;
    uint8_t data[AIRLINK_MAX_FRAME_SIZE];
} mesh_mavlink_tx_item_t;

typedef struct {
    airlink_mesh_node_info_t info;
    airlink_mesh_replay_window_t replay;
    int64_t first_seen_us;
    int64_t last_seen_us;
    int64_t last_status_us;
    bool endpoint_registered;
    bool config_prepared;
    bool config_committed;
    bool config_failed;
    uint32_t ota_ack_base;
    uint32_t ota_ack_bitmap;
    uint8_t ota_state;
} node_slot_t;

static const char *TAG = "airlink_mesh";
static airlink_mesh_config_t s_config;
static airlink_config_t s_base_config;
static airlink_mesh_status_t s_status;
static airlink_mesh_crypto_context_t s_crypto;
static airlink_mesh_replay_window_t s_root_replay;
static node_slot_t s_nodes[AIRLINK_MESH_MAX_NODES];
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_tx_lock;
static QueueHandle_t s_high_tx_queue;
static QueueHandle_t s_normal_tx_queue;
static atomic_bool s_running;
static atomic_int_fast64_t s_rx_heartbeat_us;
static atomic_int_fast64_t s_tx_heartbeat_us;
static bool s_air_approved;
static uint8_t s_root_mac[6];
static uint32_t s_last_group_crc;
static size_t s_last_group_length;
static int64_t s_last_group_us;
static bool s_config_transaction_active;
static uint32_t s_config_transaction_generation;
static uint8_t s_config_transaction_digest[32];
static uint8_t s_air_pending_digest[32];
static uint32_t s_air_pending_generation;
static bool s_node_config_operation;
static bool s_node_config_done;
static uint32_t s_node_config_token;
static uint8_t s_node_config_result;
static uint8_t s_node_config_target[6];
static airlink_config_t s_node_config_value;
static root_ota_state_t s_root_ota;
static air_ota_state_t s_air_ota;
static atomic_bool s_ota_abort_requested;

static bool root_ota_send_one(size_t index, airlink_mesh_message_type_t type,
                              const void *payload, size_t payload_length);

static bool network_config_digest(const airlink_mesh_config_t *config, uint8_t digest[32])
{
    airlink_mesh_config_t canonical = *config;
    canonical.role = AIRLINK_MESH_ROLE_OFF;
    size_t digest_length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)&canonical,
                            sizeof(canonical), digest, 32, &digest_length) == PSA_SUCCESS &&
           digest_length == 32U;
}

static void delayed_restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1000));
    airlink_mesh_prepare_restart();
    esp_restart();
}

static void schedule_restart(void)
{
    (void)xTaskCreate(delayed_restart_task, "mesh_restart", 2048, NULL, 20, NULL);
}

static uint32_t increment_saturated(uint32_t value)
{
    return value == UINT32_MAX ? UINT32_MAX : value + 1U;
}

static bool address_is_broadcast(const uint8_t address[6])
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return memcmp(address, broadcast, sizeof(broadcast)) == 0;
}

static node_slot_t *find_node(const uint8_t mac[6])
{
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (s_nodes[i].info.present && memcmp(s_nodes[i].info.sta_mac, mac, 6) == 0) return &s_nodes[i];
    }
    return NULL;
}

static node_slot_t *find_or_allocate_node(const uint8_t mac[6])
{
    node_slot_t *slot = find_node(mac);
    if (slot != NULL) return slot;
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (s_nodes[i].info.present) continue;
        slot = &s_nodes[i];
        *slot = (node_slot_t){0};
        slot->info.present = true;
        slot->info.isolation_reason = AIRLINK_MESH_ISOLATION_NOT_APPROVED;
        memcpy(slot->info.sta_mac, mac, 6);
        slot->first_seen_us = esp_timer_get_time();
        s_status.discovered_nodes++;
        return slot;
    }
    return NULL;
}

static bool approval_matches(const char *serial, const uint8_t mac[6])
{
    airlink_mesh_approval_list_t approvals;
    return airlink_mesh_approval_get(&approvals) == ESP_OK &&
           airlink_mesh_approval_contains(&approvals, serial, mac);
}

static bool send_encoded(const mesh_addr_t *to, int mesh_flags,
                         airlink_mesh_message_type_t type, uint16_t flags,
                         const uint8_t destination[6], const void *payload,
                         size_t payload_length, const mesh_opt_t *option)
{
    if (s_tx_lock == NULL) return false;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint8_t packet[AIRLINK_MESH_MAX_PACKET];
    size_t packet_length = 0;
    if (!airlink_mesh_encode(&s_crypto, type, flags, destination, payload,
                             payload_length, packet, sizeof(packet), &packet_length)) {
        xSemaphoreGive(s_tx_lock); return false;
    }
    mesh_data_t data = {.data = packet, .size = (uint16_t)packet_length,
                        .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P};
    const esp_err_t err = esp_mesh_send(to, &data, mesh_flags, option, option == NULL ? 0 : 1);
    if (err == ESP_OK) s_status.tx_packets = increment_saturated(s_status.tx_packets);
    else s_status.queue_drops = increment_saturated(s_status.queue_drops);
    xSemaphoreGive(s_tx_lock);
    return err == ESP_OK;
}

static esp_err_t queue_mavlink(const uint8_t *data, size_t length,
                               bool high_priority, void *context)
{
    (void)context;
    if (length == 0 || length > AIRLINK_MAX_FRAME_SIZE) return ESP_ERR_INVALID_SIZE;
    if (s_config.role == AIRLINK_MESH_ROLE_AIR && (!s_air_approved || !s_status.connected)) {
        return ESP_ERR_INVALID_STATE;
    }
    mesh_mavlink_tx_item_t item = {.length = (uint16_t)length,
                                   .high_priority = high_priority};
    memcpy(item.data, data, length);
    QueueHandle_t queue = high_priority ? s_high_tx_queue : s_normal_tx_queue;
    if (queue == NULL || xQueueSend(queue, &item, 0) != pdTRUE) {
        s_status.queue_drops = increment_saturated(s_status.queue_drops);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static size_t approved_group(uint8_t addresses[AIRLINK_MESH_MAX_NODES * 6U])
{
    size_t count = 0;
    const int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        node_slot_t *node = &s_nodes[i];
        if (!node->info.present || !node->info.approved ||
            node->info.isolation_reason != AIRLINK_MESH_ISOLATION_NONE ||
            now - node->last_seen_us > NODE_OFFLINE_US) continue;
        memcpy(addresses + count * 6U, node->info.sta_mac, 6); count++;
    }
    return count;
}

static bool transmit_ground_mavlink(const uint8_t *data, size_t length,
                                    bool high_priority)
{
    uint8_t addresses[AIRLINK_MESH_MAX_NODES * 6U];
    const size_t count = approved_group(addresses);
    if (count == 0) return false;
    mesh_opt_t option = {.type = MESH_OPT_SEND_GROUP, .len = (uint16_t)(count * 6U),
                         .val = addresses};
    static const uint8_t group_destination[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint16_t flags = AIRLINK_MESH_FLAG_GROUP |
                           (high_priority ? AIRLINK_MESH_FLAG_HIGH_PRIORITY : 0);
    return send_encoded(NULL, MESH_DATA_FROMDS | MESH_DATA_GROUP,
                        AIRLINK_MESH_MSG_MAVLINK_DOWN, flags, group_destination,
                        data, length, &option);
}

static esp_err_t ground_mesh_send(const uint8_t *data, size_t length,
                                  bool high_priority, void *context)
{
    (void)context;
    const int64_t now = esp_timer_get_time();
    const uint32_t crc = airlink_crc32(data, length);
    if (crc == s_last_group_crc && length == s_last_group_length &&
        now - s_last_group_us < GROUP_DEDUP_US) return ESP_OK;
    const esp_err_t err = queue_mavlink(data, length, high_priority, NULL);
    if (err == ESP_OK) {
        s_last_group_crc = crc; s_last_group_length = length; s_last_group_us = now;
    }
    return err;
}

static void tx_task(void *argument)
{
    (void)argument;
    mesh_mavlink_tx_item_t item;
    while (atomic_load(&s_running)) {
        atomic_store(&s_tx_heartbeat_us, esp_timer_get_time());
        BaseType_t received = xQueueReceive(s_high_tx_queue, &item, 0);
        if (received != pdTRUE) received = xQueueReceive(s_normal_tx_queue, &item, pdMS_TO_TICKS(10));
        if (received != pdTRUE) continue;
        if (s_config.role == AIRLINK_MESH_ROLE_AIR) {
            const uint16_t flags = item.high_priority ? AIRLINK_MESH_FLAG_HIGH_PRIORITY : 0;
            (void)send_encoded(NULL, MESH_DATA_P2P | MESH_DATA_TODS,
                               AIRLINK_MESH_MSG_MAVLINK_UP, flags, s_root_mac,
                               item.data, item.length, NULL);
        } else {
            (void)transmit_ground_mavlink(item.data, item.length, item.high_priority != 0);
        }
    }
    vTaskDelete(NULL);
}

static void unregister_node_endpoint(node_slot_t *node)
{
    if (!node->endpoint_registered) return;
    const uint8_t index = (uint8_t)(node - s_nodes);
    airlink_router_unregister((uint8_t)(AIRLINK_ENDPOINT_ID_MESH_VEHICLE_BASE + index));
    node->endpoint_registered = false;
}

static void register_node_endpoint(node_slot_t *node)
{
    if (node->endpoint_registered || !node->info.approved ||
        node->info.isolation_reason != AIRLINK_MESH_ISOLATION_NONE) return;
    const uint8_t index = (uint8_t)(node - s_nodes);
    const airlink_router_endpoint_t endpoint = {
        .id = (uint8_t)(AIRLINK_ENDPOINT_ID_MESH_VEHICLE_BASE + index),
        .type = AIRLINK_ENDPOINT_MESH,
        .direction = AIRLINK_ENDPOINT_DIRECTION_VEHICLE,
        .send = ground_mesh_send,
        .context = node,
        .name = "mesh-vehicle",
    };
    node->endpoint_registered = airlink_router_register(&endpoint) == ESP_OK;
}

static void refresh_duplicate_isolation(node_slot_t *candidate)
{
    if (!candidate->info.system_id_known || !candidate->info.approved) return;
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        node_slot_t *other = &s_nodes[i];
        if (other == candidate || !other->info.present || !other->info.approved ||
            !other->info.online || !other->info.system_id_known ||
            other->info.system_id != candidate->info.system_id) continue;
        node_slot_t *later = other->first_seen_us <= candidate->first_seen_us ? candidate : other;
        later->info.isolation_reason = AIRLINK_MESH_ISOLATION_DUPLICATE_SYSTEM_ID;
        unregister_node_endpoint(later);
        ESP_LOGE(TAG, "isolated duplicate MAVLink system id %u from " MACSTR,
                 later->info.system_id, MAC2STR(later->info.sta_mac));
    }
}

static void process_root_packet(node_slot_t *node,
                                const airlink_mesh_decoded_packet_t *packet)
{
    const int64_t now = esp_timer_get_time();
    node->last_seen_us = now; node->info.online = true;
    if (packet->header.type == AIRLINK_MESH_MSG_HELLO &&
        packet->payload_length == sizeof(hello_payload_t)) {
        hello_payload_t hello; memcpy(&hello, packet->payload, sizeof(hello));
        hello.serial[sizeof(hello.serial) - 1U] = '\0';
        hello.firmware[sizeof(hello.firmware) - 1U] = '\0';
        const bool identity_changed = node->info.serial[0] != '\0' &&
                                      strcmp(node->info.serial, hello.serial) != 0;
        strlcpy(node->info.serial, hello.serial, sizeof(node->info.serial));
        strlcpy(node->info.firmware, hello.firmware, sizeof(node->info.firmware));
        node->info.approved = !identity_changed && approval_matches(hello.serial, node->info.sta_mac);
        node->info.isolation_reason = identity_changed ? AIRLINK_MESH_ISOLATION_IDENTITY_CHANGED :
            node->info.approved ? AIRLINK_MESH_ISOLATION_NONE : AIRLINK_MESH_ISOLATION_NOT_APPROVED;
        if (node->info.approved) register_node_endpoint(node); else unregister_node_endpoint(node);
        approval_payload_t approval = {.approved = node->info.approved,
                                       .reason = (uint8_t)node->info.isolation_reason};
        mesh_addr_t to; memcpy(to.addr, node->info.sta_mac, 6);
        (void)send_encoded(&to, MESH_DATA_FROMDS | MESH_DATA_P2P,
                           AIRLINK_MESH_MSG_APPROVAL, 0, node->info.sta_mac,
                           &approval, sizeof(approval), NULL);
    } else if (packet->header.type == AIRLINK_MESH_MSG_NODE_STATUS &&
               packet->payload_length == sizeof(node_status_payload_t)) {
        node_status_payload_t status; memcpy(&status, packet->payload, sizeof(status));
        memcpy(node->info.parent, status.parent, 6); node->info.layer = status.layer;
        node->info.rssi = status.rssi; node->info.system_id = status.system_id;
        node->info.system_id_known = status.system_id_known != 0;
        node->info.armed = status.armed != 0; node->info.armed_known = status.armed_known != 0;
        node->info.uptime_s = status.uptime_s; node->info.queue_drops = status.queue_drops;
        node->info.mesh_generation = status.mesh_generation;
        node->info.ota_image_state = status.ota_image_state;
        node->info.status_known = true; node->last_status_us = now;
        refresh_duplicate_isolation(node);
    } else if (packet->header.type == AIRLINK_MESH_MSG_MAVLINK_UP &&
               node->info.approved && node->info.isolation_reason == AIRLINK_MESH_ISOLATION_NONE &&
               node->endpoint_registered) {
        const uint8_t index = (uint8_t)(node - s_nodes);
        (void)airlink_router_ingest((uint8_t)(AIRLINK_ENDPOINT_ID_MESH_VEHICLE_BASE + index),
                                    packet->payload, packet->payload_length);
    } else if (packet->header.type == AIRLINK_MESH_MSG_CONFIG_RESPONSE &&
               packet->payload_length == sizeof(node_config_response_t) &&
               packet->payload[0] >= CONFIG_KIND_READ_RESPONSE) {
        node_config_response_t response;
        memcpy(&response, packet->payload, sizeof(response));
        if (s_node_config_operation && response.token == s_node_config_token &&
            memcmp(node->info.sta_mac, s_node_config_target, 6) == 0 &&
            (response.kind == CONFIG_KIND_READ_RESPONSE ||
             response.kind == CONFIG_KIND_WRITE_RESPONSE)) {
            s_node_config_result = response.result;
            if (response.kind == CONFIG_KIND_READ_RESPONSE && response.result == CONFIG_RESULT_OK) {
                s_node_config_value = response.config;
            }
            s_node_config_done = true;
        }
    } else if (packet->header.type == AIRLINK_MESH_MSG_CONFIG_RESPONSE &&
               packet->payload_length == sizeof(config_response_payload_t)) {
        config_response_payload_t response;
        memcpy(&response, packet->payload, sizeof(response));
        if (response.kind == CONFIG_KIND_TRANSACTION && s_config_transaction_active &&
            response.generation == s_config_transaction_generation &&
            memcmp(response.digest, s_config_transaction_digest, sizeof(response.digest)) == 0) {
            if (response.result != CONFIG_RESULT_OK) node->config_failed = true;
            else if (response.phase == CONFIG_PHASE_PREPARE) node->config_prepared = true;
            else if (response.phase == CONFIG_PHASE_COMMIT) node->config_committed = true;
        }
    } else if (packet->header.type == AIRLINK_MESH_MSG_OTA_ACK &&
               packet->payload_length == sizeof(ota_ack_payload_t)) {
        ota_ack_payload_t ack;
        memcpy(&ack, packet->payload, sizeof(ack));
        if (s_root_ota.active && ack.session == s_root_ota.session) {
            node->ota_ack_base = ack.window_base;
            node->ota_ack_bitmap = ack.bitmap;
            node->ota_state = ack.state;
            node->info.ota_state = ack.state;
            node->info.ota_progress = ack.progress;
        }
    }
}

static void send_config_response(uint8_t phase, uint8_t result, uint32_t generation,
                                 const uint8_t digest[32])
{
    config_response_payload_t response = {
        .kind = CONFIG_KIND_TRANSACTION, .generation = generation,
        .phase = phase, .result = result,
    };
    memcpy(response.digest, digest, sizeof(response.digest));
    (void)send_encoded(NULL, MESH_DATA_P2P | MESH_DATA_TODS,
                       AIRLINK_MESH_MSG_CONFIG_RESPONSE, AIRLINK_MESH_FLAG_HIGH_PRIORITY,
                       s_root_mac, &response, sizeof(response), NULL);
}

static uint32_t ota_window_expected(uint32_t base, uint32_t total_chunks)
{
    if (base >= total_chunks) return 0;
    const uint32_t remaining = total_chunks - base;
    return remaining >= OTA_WINDOW_CHUNKS ? UINT32_MAX :
           (uint32_t)((UINT64_C(1) << remaining) - 1U);
}

static void send_air_ota_ack(uint32_t base, uint32_t bitmap, uint8_t state)
{
    ota_ack_payload_t ack = {
        .session = s_air_ota.session, .window_base = base, .bitmap = bitmap,
        .state = state,
        .progress = s_air_ota.total_chunks == 0 ? 0 :
            (uint8_t)(((uint64_t)(base + (uint32_t)__builtin_popcount(bitmap)) * 100U) /
                      s_air_ota.total_chunks),
    };
    (void)send_encoded(NULL, MESH_DATA_P2P | MESH_DATA_TODS,
                       AIRLINK_MESH_MSG_OTA_ACK, 0, s_root_mac,
                       &ack, sizeof(ack), NULL);
}

static void air_ota_reset(bool abort_stream)
{
    if (abort_stream && airlink_ota_stream_active()) airlink_ota_stream_abort();
    free(s_air_ota.window);
    s_air_ota = (air_ota_state_t){0};
}

static void process_air_ota_begin(const airlink_mesh_decoded_packet_t *packet)
{
    if (packet->payload_length != sizeof(ota_begin_payload_t)) return;
    ota_begin_payload_t begin;
    memcpy(&begin, packet->payload, sizeof(begin));
    if (!s_air_approved || !airlink_mesh_global_safe() || begin.image_size == 0 ||
        begin.chunk_size != OTA_CHUNK_SIZE || begin.window_chunks != OTA_WINDOW_CHUNKS) {
        s_air_ota.session = begin.session;
        send_air_ota_ack(0, 0, OTA_STATE_ERROR);
        return;
    }
    if (s_air_ota.active && s_air_ota.session != begin.session) air_ota_reset(true);
    if (!s_air_ota.active) {
        uint8_t *window = heap_caps_malloc(OTA_WINDOW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        begin.expected_version[sizeof(begin.expected_version) - 1U] = '\0';
        if (window == NULL || airlink_ota_stream_begin_versioned(
            begin.image_size, begin.sha256, begin.expected_version) != ESP_OK) {
            free(window); s_air_ota.session = begin.session;
            send_air_ota_ack(0, 0, OTA_STATE_ERROR); return;
        }
        s_air_ota = (air_ota_state_t){
            .active = true, .session = begin.session, .image_size = begin.image_size,
            .total_chunks = (uint32_t)((begin.image_size + OTA_CHUNK_SIZE - 1U) / OTA_CHUNK_SIZE),
            .window = window,
        };
    }
    send_air_ota_ack(s_air_ota.window_base, s_air_ota.bitmap, OTA_STATE_READY);
}

static void process_air_ota_chunk(const airlink_mesh_decoded_packet_t *packet)
{
    if (!s_air_ota.active || packet->payload_length < sizeof(ota_chunk_header_t)) return;
    ota_chunk_header_t chunk;
    memcpy(&chunk, packet->payload, sizeof(chunk));
    if (chunk.session != s_air_ota.session || chunk.data_length == 0 ||
        chunk.data_length > OTA_CHUNK_SIZE ||
        packet->payload_length != sizeof(chunk) + chunk.data_length ||
        chunk.chunk_index >= s_air_ota.total_chunks) return;
    const uint32_t base = (chunk.chunk_index / OTA_WINDOW_CHUNKS) * OTA_WINDOW_CHUNKS;
    if (base != s_air_ota.window_base) {
        if (!s_air_ota.window_written || base != s_air_ota.window_base + OTA_WINDOW_CHUNKS) {
            send_air_ota_ack(s_air_ota.window_base, s_air_ota.bitmap, OTA_STATE_RECEIVING); return;
        }
        s_air_ota.window_base = base; s_air_ota.bitmap = 0; s_air_ota.window_written = false;
        memset(s_air_ota.lengths, 0, sizeof(s_air_ota.lengths));
    }
    const uint32_t slot = chunk.chunk_index - base;
    const uint32_t bit = UINT32_C(1) << slot;
    if ((s_air_ota.bitmap & bit) == 0) {
        memcpy(s_air_ota.window + slot * OTA_CHUNK_SIZE,
               packet->payload + sizeof(chunk), chunk.data_length);
        s_air_ota.lengths[slot] = chunk.data_length;
        s_air_ota.bitmap |= bit;
    }
    const uint32_t expected = ota_window_expected(base, s_air_ota.total_chunks);
    if (s_air_ota.bitmap == expected && !s_air_ota.window_written) {
        bool written = true;
        const uint32_t count = (uint32_t)__builtin_popcount(expected);
        for (uint32_t i = 0; i < count; ++i) {
            if (s_air_ota.lengths[i] == 0 ||
                airlink_ota_stream_write(s_air_ota.window + i * OTA_CHUNK_SIZE,
                                         s_air_ota.lengths[i]) != ESP_OK) {
                written = false; break;
            }
        }
        if (!written) {
            send_air_ota_ack(base, s_air_ota.bitmap, OTA_STATE_ERROR);
            air_ota_reset(true); return;
        }
        s_air_ota.window_written = true;
    }
    send_air_ota_ack(base, s_air_ota.bitmap, OTA_STATE_RECEIVING);
}

static void process_air_ota_commit(const airlink_mesh_decoded_packet_t *packet)
{
    if (!s_air_ota.active || packet->payload_length != sizeof(ota_commit_payload_t)) return;
    ota_commit_payload_t commit;
    memcpy(&commit, packet->payload, sizeof(commit));
    if (commit.session != s_air_ota.session) return;
    uint8_t state = OTA_STATE_ERROR;
    if (!commit.activate) {
        if (s_air_ota.window_written && airlink_ota_stream_remaining() == 0 &&
            airlink_ota_stream_verify() == ESP_OK) state = OTA_STATE_VERIFIED;
    } else if (airlink_ota_stream_staged() && airlink_ota_stream_activate() == ESP_OK) {
        state = OTA_STATE_ACTIVATING;
    }
    send_air_ota_ack(s_air_ota.window_base, s_air_ota.bitmap, state);
    if (state == OTA_STATE_ACTIVATING) schedule_restart();
    else if (state == OTA_STATE_ERROR) air_ota_reset(true);
}

static void process_air_packet(const airlink_mesh_decoded_packet_t *packet)
{
    if (packet->header.type == AIRLINK_MESH_MSG_APPROVAL &&
        packet->payload_length == sizeof(approval_payload_t)) {
        approval_payload_t approval; memcpy(&approval, packet->payload, sizeof(approval));
        s_air_approved = approval.approved != 0;
    } else if (packet->header.type == AIRLINK_MESH_MSG_MAVLINK_DOWN && s_air_approved) {
        (void)airlink_router_ingest(AIRLINK_ENDPOINT_ID_MESH_GCS,
                                    packet->payload, packet->payload_length);
    } else if (packet->header.type == AIRLINK_MESH_MSG_CONFIG_GET && s_air_approved &&
               packet->payload_length == sizeof(node_config_read_request_t)) {
        node_config_read_request_t request;
        memcpy(&request, packet->payload, sizeof(request));
        if (request.kind == CONFIG_KIND_READ_REQUEST) {
            node_config_response_t response = {
                .kind = CONFIG_KIND_READ_RESPONSE, .token = request.token,
                .result = CONFIG_RESULT_OK,
            };
            airlink_config_t current;
            airlink_config_get(&current);
            memcpy(&response.config, &current, sizeof(current));
            (void)send_encoded(NULL, MESH_DATA_P2P | MESH_DATA_TODS,
                               AIRLINK_MESH_MSG_CONFIG_RESPONSE, 0, s_root_mac,
                               &response, sizeof(response), NULL);
        }
    } else if (packet->header.type == AIRLINK_MESH_MSG_CONFIG_GET && s_air_approved &&
               packet->payload_length == sizeof(node_config_write_request_t)) {
        node_config_write_request_t request;
        memcpy(&request, packet->payload, sizeof(request));
        if (request.kind == CONFIG_KIND_WRITE_REQUEST) {
            airlink_config_t candidate;
            memcpy(&candidate, &request.config, sizeof(candidate));
            candidate.bridge_enabled = false;
            candidate.bridge_role = AIRLINK_BRIDGE_OFF;
            uint8_t result = CONFIG_RESULT_OK;
            if (!airlink_mesh_global_safe()) result = CONFIG_RESULT_UNSAFE;
            else if (!airlink_config_validate(&candidate)) result = CONFIG_RESULT_INVALID;
            else if (airlink_config_save(&candidate) != ESP_OK) result = CONFIG_RESULT_STORAGE;
            node_config_response_t response = {
                .kind = CONFIG_KIND_WRITE_RESPONSE, .token = request.token, .result = result,
            };
            (void)send_encoded(NULL, MESH_DATA_P2P | MESH_DATA_TODS,
                               AIRLINK_MESH_MSG_CONFIG_RESPONSE,
                               AIRLINK_MESH_FLAG_HIGH_PRIORITY, s_root_mac,
                               &response, sizeof(response), NULL);
            if (result == CONFIG_RESULT_OK) schedule_restart();
        }
    } else if (packet->header.type == AIRLINK_MESH_MSG_CONFIG_PREPARE && s_air_approved &&
               packet->payload_length == sizeof(config_prepare_payload_t)) {
        config_prepare_payload_t prepare;
        memcpy(&prepare, packet->payload, sizeof(prepare));
        airlink_mesh_config_t candidate;
        memcpy(&candidate, &prepare.config, sizeof(candidate));
        candidate.role = AIRLINK_MESH_ROLE_AIR;
        uint8_t digest[32];
        uint8_t result = CONFIG_RESULT_OK;
        if (prepare.kind != CONFIG_KIND_TRANSACTION) result = CONFIG_RESULT_INVALID;
        else if (!airlink_mesh_global_safe()) result = CONFIG_RESULT_UNSAFE;
        else if (!network_config_digest(&candidate, digest) ||
                 memcmp(digest, prepare.digest, sizeof(digest)) != 0) result = CONFIG_RESULT_INVALID;
        else if (airlink_mesh_config_stage(&candidate, prepare.generation) != ESP_OK) {
            result = CONFIG_RESULT_STORAGE;
        }
        if (result == CONFIG_RESULT_OK) {
            s_air_pending_generation = prepare.generation;
            memcpy(s_air_pending_digest, prepare.digest, sizeof(s_air_pending_digest));
        }
        send_config_response(CONFIG_PHASE_PREPARE, result, prepare.generation, prepare.digest);
    } else if (packet->header.type == AIRLINK_MESH_MSG_CONFIG_ABORT &&
               packet->payload_length == sizeof(config_control_payload_t)) {
        config_control_payload_t control;
        memcpy(&control, packet->payload, sizeof(control));
        if (control.generation == s_air_pending_generation &&
            memcmp(control.digest, s_air_pending_digest, sizeof(control.digest)) == 0) {
            (void)airlink_mesh_config_abort_staged();
            s_air_pending_generation = 0;
            memset(s_air_pending_digest, 0, sizeof(s_air_pending_digest));
        }
    } else if (packet->header.type == AIRLINK_MESH_MSG_CONFIG_COMMIT &&
               packet->payload_length == sizeof(config_control_payload_t)) {
        config_control_payload_t control;
        memcpy(&control, packet->payload, sizeof(control));
        uint8_t result = CONFIG_RESULT_INVALID;
        if (control.generation == s_air_pending_generation &&
            memcmp(control.digest, s_air_pending_digest, sizeof(control.digest)) == 0) {
            result = airlink_mesh_config_commit_staged() == ESP_OK ?
                     CONFIG_RESULT_OK : CONFIG_RESULT_STORAGE;
        }
        send_config_response(CONFIG_PHASE_COMMIT, result, control.generation, control.digest);
        if (result == CONFIG_RESULT_OK) schedule_restart();
    } else if (packet->header.type == AIRLINK_MESH_MSG_OTA_BEGIN) {
        process_air_ota_begin(packet);
    } else if (packet->header.type == AIRLINK_MESH_MSG_OTA_CHUNK) {
        process_air_ota_chunk(packet);
    } else if (packet->header.type == AIRLINK_MESH_MSG_OTA_COMMIT) {
        process_air_ota_commit(packet);
    } else if (packet->header.type == AIRLINK_MESH_MSG_OTA_ABORT) {
        air_ota_reset(true);
    } else if (packet->header.type == AIRLINK_MESH_MSG_REBOOT && s_air_approved &&
               airlink_mesh_global_safe()) {
        schedule_restart();
    }
}

static void rx_task(void *argument)
{
    (void)argument;
    uint8_t buffer[AIRLINK_MESH_MAX_PACKET];
    while (atomic_load(&s_running)) {
        atomic_store(&s_rx_heartbeat_us, esp_timer_get_time());
        mesh_addr_t from = {0}; int flags = 0;
        mesh_data_t data = {.data = buffer, .size = sizeof(buffer), .proto = MESH_PROTO_BIN};
        const esp_err_t err = esp_mesh_recv(&from, &data, 1000, &flags, NULL, 0);
        if (err == ESP_ERR_MESH_TIMEOUT) continue;
        if (err != ESP_OK || data.size < AIRLINK_MESH_HEADER_SIZE) continue;
        airlink_mesh_wire_header_t clear_header; memcpy(&clear_header, buffer, sizeof(clear_header));
        airlink_mesh_decoded_packet_t decoded;
        bool ok = false;
        airlink_mesh_decode_result_t decode_result = AIRLINK_MESH_DECODE_MALFORMED;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_config.role == AIRLINK_MESH_ROLE_GROUND_ROOT) {
            node_slot_t *node = find_node(from.addr);
            airlink_mesh_replay_window_t first_replay = {0};
            airlink_mesh_replay_window_t *replay = node == NULL ? &first_replay : &node->replay;
            if (memcmp(clear_header.source, from.addr, 6) == 0 &&
                (memcmp(clear_header.destination, s_crypto.local_mac, 6) == 0 ||
                 address_is_broadcast(clear_header.destination)) &&
                (clear_header.flags & AIRLINK_MESH_FLAG_FROM_ROOT) == 0) {
                decode_result = airlink_mesh_decode_ex(s_config.fleet_key, s_config.network_id,
                                                       buffer, data.size, replay, &decoded);
                ok = decode_result == AIRLINK_MESH_DECODE_OK;
            }
            if (ok && node == NULL) {
                node = find_or_allocate_node(from.addr);
                if (node != NULL) node->replay = first_replay;
                else ok = false;
            }
            if (ok) process_root_packet(node, &decoded);
        } else {
            if ((memcmp(clear_header.destination, s_crypto.local_mac, 6) == 0 ||
                 address_is_broadcast(clear_header.destination)) &&
                (clear_header.flags & AIRLINK_MESH_FLAG_FROM_ROOT) != 0) {
                decode_result = airlink_mesh_decode_ex(s_config.fleet_key, s_config.network_id,
                                                       buffer, data.size, &s_root_replay, &decoded);
                ok = decode_result == AIRLINK_MESH_DECODE_OK;
            }
            if (ok) process_air_packet(&decoded);
        }
        if (ok) s_status.rx_packets = increment_saturated(s_status.rx_packets);
        else if (decode_result == AIRLINK_MESH_DECODE_REPLAY) {
            s_status.replay_drops = increment_saturated(s_status.replay_drops);
        } else {
            s_status.auth_failures = increment_saturated(s_status.auth_failures);
        }
        xSemaphoreGive(s_lock);
    }
    vTaskDelete(NULL);
}

static void send_air_presence(void)
{
    wifi_ap_record_t parent;
    if (esp_wifi_sta_get_ap_info(&parent) == ESP_OK) s_status.rssi = parent.rssi;
    const esp_app_desc_t *app = esp_app_get_description();
    hello_payload_t hello = {0};
    strlcpy(hello.serial, s_base_config.serial_number, sizeof(hello.serial));
    strlcpy(hello.firmware, app->version, sizeof(hello.firmware));
    (void)send_encoded(NULL, MESH_DATA_P2P | MESH_DATA_TODS,
                       AIRLINK_MESH_MSG_HELLO, 0, s_root_mac,
                       &hello, sizeof(hello), NULL);

    node_status_payload_t status = {.layer = s_status.layer, .rssi = s_status.rssi,
        .armed = airlink_router_fc_armed(), .armed_known = airlink_router_fc_seen(),
        .uptime_s = (uint32_t)(esp_timer_get_time() / INT64_C(1000000)),
        .queue_drops = s_status.queue_drops};
    status.mesh_generation = airlink_mesh_config_generation();
    status.ota_image_state = airlink_ota_image_state();
    memcpy(status.parent, s_status.parent, 6);
    status.system_id_known = airlink_router_fc_system_id(&status.system_id);
    (void)send_encoded(NULL, MESH_DATA_P2P | MESH_DATA_TODS,
                       AIRLINK_MESH_MSG_NODE_STATUS, 0, s_root_mac,
                       &status, sizeof(status), NULL);
}

static void periodic_task(void *argument)
{
    (void)argument;
    while (atomic_load(&s_running)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_config.role == AIRLINK_MESH_ROLE_AIR && s_status.connected) send_air_presence();
        if (s_config.role == AIRLINK_MESH_ROLE_GROUND_ROOT) {
            const int64_t now = esp_timer_get_time();
            uint8_t approved_online = 0;
            for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
                node_slot_t *node = &s_nodes[i];
                if (!node->info.present) continue;
                node->info.online = now - node->last_seen_us <= NODE_OFFLINE_US;
                node->info.status_known = now - node->last_status_us <= NODE_STATUS_UNKNOWN_US;
                if (!node->info.online || !node->info.status_known) node->info.armed_known = false;
                if (node->info.approved && node->info.online) approved_online++;
            }
            s_status.approved_online_nodes = approved_online;
        }
        xSemaphoreGive(s_lock);
        if (s_config.role == AIRLINK_MESH_ROLE_GROUND_ROOT && s_root_ota.active &&
            !s_root_ota.activating && !airlink_mesh_global_safe()) {
            ESP_LOGE(TAG, "aborting Mesh OTA after fleet safety state changed");
            atomic_store(&s_ota_abort_requested, true);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

static void mesh_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument; (void)base;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (id == MESH_EVENT_STARTED) {
        s_status.started = true;
        s_status.connected = s_config.role == AIRLINK_MESH_ROLE_GROUND_ROOT;
        s_status.rootless = false;
        s_status.layer = (uint8_t)esp_mesh_get_layer();
    } else if (id == MESH_EVENT_PARENT_CONNECTED) {
        const mesh_event_connected_t *event = data;
        s_status.connected = true; s_status.rootless = false;
        s_status.layer = (uint8_t)event->self_layer;
        memcpy(s_status.parent, event->connected.bssid, 6);
        memcpy(s_root_mac, event->connected.bssid, 6);
    } else if (id == MESH_EVENT_PARENT_DISCONNECTED) {
        s_status.connected = false; s_air_approved = false;
        s_status.reorganizations = increment_saturated(s_status.reorganizations);
    } else if (id == MESH_EVENT_LAYER_CHANGE) {
        const mesh_event_layer_change_t *event = data;
        s_status.layer = (uint8_t)event->new_layer;
        s_status.reorganizations = increment_saturated(s_status.reorganizations);
    } else if (id == MESH_EVENT_NETWORK_STATE) {
        s_status.rootless = ((const mesh_event_network_state_t *)data)->is_rootless;
    } else if (id == MESH_EVENT_ROOT_ADDRESS) {
        memcpy(s_root_mac, ((const mesh_event_root_address_t *)data)->addr, 6);
    }
    xSemaphoreGive(s_lock);
}

esp_err_t airlink_mesh_start(const airlink_mesh_config_t *mesh_config,
                             const airlink_config_t *base_config)
{
    if (mesh_config == NULL || base_config == NULL ||
        !airlink_mesh_config_validate(mesh_config, base_config) ||
        mesh_config->role == AIRLINK_MESH_ROLE_OFF) return ESP_ERR_INVALID_ARG;
    s_config = *mesh_config; s_base_config = *base_config;
    s_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    s_high_tx_queue = xQueueCreate(MESH_HIGH_QUEUE_DEPTH, sizeof(mesh_mavlink_tx_item_t));
    s_normal_tx_queue = xQueueCreate(MESH_NORMAL_QUEUE_DEPTH, sizeof(mesh_mavlink_tx_item_t));
    if (s_lock == NULL || s_tx_lock == NULL || s_high_tx_queue == NULL ||
        s_normal_tx_queue == NULL) return ESP_ERR_NO_MEM;
    s_status = (airlink_mesh_status_t){.is_root = mesh_config->role == AIRLINK_MESH_ROLE_GROUND_ROOT};
    esp_read_mac(s_crypto.local_mac, ESP_MAC_WIFI_STA);
    memcpy(s_crypto.fleet_key, mesh_config->fleet_key, 32);
    memcpy(s_crypto.network_id, mesh_config->network_id, 6);
    esp_fill_random(s_crypto.session_id, sizeof(s_crypto.session_id));
    s_crypto.next_sequence = 1;
    s_crypto.from_root = mesh_config->role == AIRLINK_MESH_ROLE_GROUND_ROOT;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return loop;
    esp_netif_t *mesh_sta = NULL;
    esp_netif_t *mesh_ap = NULL;
    esp_netif_create_default_wifi_mesh_netifs(&mesh_sta, &mesh_ap);
    if (mesh_sta == NULL || mesh_ap == NULL) return ESP_ERR_NO_MEM;
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY), TAG, "2.4 GHz only");
    ESP_RETURN_ON_ERROR(esp_wifi_set_country_code(mesh_config->country, false), TAG, "country");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "power save");
    ESP_RETURN_ON_ERROR(esp_mesh_init(), TAG, "mesh init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                                    mesh_event, NULL), TAG, "mesh events");
    ESP_RETURN_ON_ERROR(esp_mesh_set_topology(MESH_TOPO_TREE), TAG, "topology");
    ESP_RETURN_ON_ERROR(esp_mesh_set_max_layer((int)mesh_config->max_hops + 1), TAG, "max layer");
    ESP_RETURN_ON_ERROR(esp_mesh_set_capacity_num((int)mesh_config->max_nodes + 1),
                        TAG, "max capacity");
    ESP_RETURN_ON_ERROR(esp_mesh_fix_root(true), TAG, "fixed root");
    ESP_RETURN_ON_ERROR(esp_mesh_allow_root_conflicts(false), TAG, "single root");
    ESP_RETURN_ON_ERROR(esp_mesh_disable_ps(), TAG, "mesh power save");
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    cfg.channel = mesh_config->channel; cfg.allow_channel_switch = false;
    cfg.router.allow_router_switch = false;
    memcpy(cfg.mesh_id.addr, mesh_config->network_id, 6);
    cfg.mesh_ap.max_connection = mesh_config->max_nodes;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    char password[44];
    if (!airlink_mesh_derive_softap_password(mesh_config->fleet_key,
                                             mesh_config->network_id, password)) return ESP_FAIL;
    strlcpy((char *)cfg.mesh_ap.password, password, sizeof(cfg.mesh_ap.password));
    ESP_RETURN_ON_ERROR(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK), TAG, "mesh auth");
    ESP_RETURN_ON_ERROR(esp_mesh_set_config(&cfg), TAG, "mesh config");
    if (mesh_config->role == AIRLINK_MESH_ROLE_GROUND_ROOT) {
        ESP_RETURN_ON_ERROR(esp_mesh_set_type(MESH_ROOT), TAG, "root role");
    }
    atomic_store(&s_running, true);
    ESP_RETURN_ON_ERROR(esp_mesh_start(), TAG, "mesh start");
    if (mesh_config->role == AIRLINK_MESH_ROLE_AIR) {
        const airlink_router_endpoint_t endpoint = {
            .id = AIRLINK_ENDPOINT_ID_MESH_GCS, .type = AIRLINK_ENDPOINT_MESH,
            .direction = AIRLINK_ENDPOINT_DIRECTION_GCS, .send = queue_mavlink,
            .name = "mesh-gcs",
        };
        ESP_RETURN_ON_ERROR(airlink_router_register(&endpoint), TAG, "mesh router endpoint");
    }
    if (xTaskCreate(tx_task, "mesh_tx", MESH_TX_TASK_STACK, NULL, 17, NULL) != pdPASS ||
        xTaskCreate(rx_task, "mesh_rx", MESH_RX_TASK_STACK, NULL, 18, NULL) != pdPASS ||
        xTaskCreate(periodic_task, "mesh_status", MESH_PERIODIC_TASK_STACK, NULL, 8, NULL) != pdPASS) {
        atomic_store(&s_running, false); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool airlink_mesh_ready(void)
{
    const int64_t now = esp_timer_get_time();
    return s_status.started && s_status.connected && !s_status.rootless &&
           now - atomic_load(&s_rx_heartbeat_us) < INT64_C(3000000) &&
           now - atomic_load(&s_tx_heartbeat_us) < INT64_C(3000000);
}

bool airlink_mesh_global_safe(void)
{
    if (s_config.role != AIRLINK_MESH_ROLE_GROUND_ROOT) {
        return airlink_router_fc_seen() && !airlink_router_fc_armed();
    }
    airlink_mesh_approval_list_t approvals;
    if (airlink_mesh_approval_get(&approvals) != ESP_OK || approvals.count == 0) return false;
    const int64_t now = esp_timer_get_time();
    bool safe = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < approvals.count; ++i) {
        node_slot_t *node = find_node(approvals.entries[i].sta_mac);
        if (node == NULL || strcmp(node->info.serial, approvals.entries[i].serial) != 0 ||
            now - node->last_seen_us > NODE_OFFLINE_US ||
            now - node->last_status_us > NODE_STATUS_UNKNOWN_US ||
            !node->info.armed_known || node->info.armed ||
            node->info.isolation_reason != AIRLINK_MESH_ISOLATION_NONE) {
            safe = false; break;
        }
    }
    xSemaphoreGive(s_lock);
    return safe;
}

void airlink_mesh_get_status(airlink_mesh_status_t *status)
{
    if (status == NULL) return;
    if (s_lock == NULL) { *status = (airlink_mesh_status_t){0}; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY); *status = s_status; xSemaphoreGive(s_lock);
}

size_t airlink_mesh_get_nodes(airlink_mesh_node_info_t *nodes, size_t capacity)
{
    if (nodes == NULL && capacity != 0) return 0;
    if (s_lock == NULL) return 0;
    size_t count = 0; xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES && count < capacity; ++i) {
        if (s_nodes[i].info.present) nodes[count++] = s_nodes[i].info;
    }
    xSemaphoreGive(s_lock); return count;
}

size_t airlink_mesh_nodes_json(char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return 0;
    if (s_lock == NULL) return (size_t)snprintf(output, capacity, "{\"nodes\":[]}");
    size_t used = (size_t)snprintf(output, capacity, "{\"nodes\":[");
    xSemaphoreTake(s_lock, portMAX_DELAY); bool first = true;
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES && used < capacity; ++i) {
        const airlink_mesh_node_info_t *n = &s_nodes[i].info; if (!n->present) continue;
        used += (size_t)snprintf(output + used, capacity - used,
            "%s{\"serial\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
            "\"approved\":%s,\"online\":%s,\"layer\":%u,\"rssi\":%d,"
            "\"parent\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
            "\"system_id\":%u,\"system_id_known\":%s,\"armed\":%s,"
            "\"armed_known\":%s,\"isolation_reason\":%u,\"firmware\":\"%s\","
            "\"uptime_s\":%" PRIu32 ",\"queue_drops\":%" PRIu32 ","
            "\"mesh_generation\":%" PRIu32 ","
            "\"ota_state\":%u,\"ota_progress\":%u,\"ota_image_state\":%" PRId32 "}",
            first ? "" : ",", n->serial, n->sta_mac[0], n->sta_mac[1], n->sta_mac[2],
            n->sta_mac[3], n->sta_mac[4], n->sta_mac[5], n->approved ? "true" : "false",
            n->online ? "true" : "false", n->layer, n->rssi,
            n->parent[0], n->parent[1], n->parent[2], n->parent[3], n->parent[4], n->parent[5],
            n->system_id,
            n->system_id_known ? "true" : "false", n->armed ? "true" : "false",
            n->armed_known ? "true" : "false", n->isolation_reason, n->firmware,
            n->uptime_s, n->queue_drops, n->mesh_generation,
            n->ota_state, n->ota_progress, n->ota_image_state);
        first = false;
    }
    xSemaphoreGive(s_lock);
    if (used < capacity) used += (size_t)snprintf(output + used, capacity - used, "]}");
    return used < capacity ? used : capacity - 1U;
}

esp_err_t airlink_mesh_approve_node(const char *serial, const uint8_t sta_mac[6])
{
    if (serial == NULL || sta_mac == NULL || s_lock == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    node_slot_t *node = find_node(sta_mac);
    if (node != NULL && strcmp(node->info.serial, serial) != 0) {
        xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t save = airlink_mesh_approval_add(serial, sta_mac);
    if (save != ESP_OK) { xSemaphoreGive(s_lock); return save; }
    if (node != NULL) {
        node->info.approved = true; node->info.isolation_reason = AIRLINK_MESH_ISOLATION_NONE;
        refresh_duplicate_isolation(node);
        if (node->info.isolation_reason == AIRLINK_MESH_ISOLATION_NONE) register_node_endpoint(node);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t airlink_mesh_remove_node(const char *serial, const uint8_t sta_mac[6])
{
    if (serial == NULL || sta_mac == NULL || s_lock == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t remove = airlink_mesh_approval_remove(serial, sta_mac);
    if (remove != ESP_OK) { xSemaphoreGive(s_lock); return remove; }
    node_slot_t *node = find_node(sta_mac);
    if (node != NULL) {
        node->info.approved = false; node->info.isolation_reason = AIRLINK_MESH_ISOLATION_REMOVED;
        unregister_node_endpoint(node);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static bool transaction_nodes_ready(bool committed, bool *failed)
{
    bool ready = true;
    *failed = false;
    const int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        node_slot_t *node = &s_nodes[i];
        if (!node->info.present || !node->info.approved) continue;
        if (!node->info.online || now - node->last_seen_us > NODE_OFFLINE_US) {
            *failed = true; return false;
        }
        if (node->config_failed) *failed = true;
        if (committed ? !node->config_committed : !node->config_prepared) ready = false;
    }
    return ready;
}

static void send_config_control_to_approved(airlink_mesh_message_type_t type,
                                            const void *payload, size_t payload_length)
{
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        node_slot_t *node = &s_nodes[i];
        if (!node->info.present || !node->info.approved || !node->info.online) continue;
        mesh_addr_t to;
        memcpy(to.addr, node->info.sta_mac, sizeof(to.addr));
        (void)send_encoded(&to, MESH_DATA_FROMDS | MESH_DATA_P2P, type,
                           AIRLINK_MESH_FLAG_HIGH_PRIORITY, node->info.sta_mac,
                           payload, payload_length, NULL);
    }
}

esp_err_t airlink_mesh_update_network(const airlink_mesh_config_t *config,
                                      uint32_t timeout_ms)
{
    if (config == NULL || timeout_ms == 0 || s_config.role != AIRLINK_MESH_ROLE_GROUND_ROOT ||
        config->role != AIRLINK_MESH_ROLE_GROUND_ROOT || !airlink_mesh_global_safe()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = airlink_mesh_config_generation();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (s_nodes[i].info.approved && s_nodes[i].info.mesh_generation > generation) {
            generation = s_nodes[i].info.mesh_generation;
        }
    }
    xSemaphoreGive(s_lock);
    if (generation == UINT32_MAX) return ESP_ERR_INVALID_STATE;
    generation++;
    config_prepare_payload_t prepare = {
        .kind = CONFIG_KIND_TRANSACTION,
        .generation = generation,
    };
    memcpy(&prepare.config, config, sizeof(*config));
    if (!network_config_digest(config, prepare.digest)) return ESP_FAIL;
    ESP_RETURN_ON_ERROR(airlink_mesh_config_stage(config, generation), TAG, "stage root config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_config_transaction_active) {
        xSemaphoreGive(s_lock); (void)airlink_mesh_config_abort_staged();
        return ESP_ERR_INVALID_STATE;
    }
    s_config_transaction_active = true;
    s_config_transaction_generation = generation;
    memcpy(s_config_transaction_digest, prepare.digest, sizeof(prepare.digest));
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        s_nodes[i].config_prepared = false;
        s_nodes[i].config_committed = false;
        s_nodes[i].config_failed = false;
    }
    send_config_control_to_approved(AIRLINK_MESH_MSG_CONFIG_PREPARE, &prepare, sizeof(prepare));
    xSemaphoreGive(s_lock);

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool failed = false, ready = false;
    while (esp_timer_get_time() < deadline) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ready = transaction_nodes_ready(false, &failed);
        xSemaphoreGive(s_lock);
        if (ready || failed || !airlink_mesh_global_safe()) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!ready || failed || !airlink_mesh_global_safe()) {
        config_control_payload_t abort = {.generation = generation};
        memcpy(abort.digest, prepare.digest, sizeof(abort.digest));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        send_config_control_to_approved(AIRLINK_MESH_MSG_CONFIG_ABORT, &abort, sizeof(abort));
        s_config_transaction_active = false;
        xSemaphoreGive(s_lock);
        (void)airlink_mesh_config_abort_staged();
        return ESP_ERR_TIMEOUT;
    }

    config_control_payload_t commit = {.generation = generation};
    memcpy(commit.digest, prepare.digest, sizeof(commit.digest));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    send_config_control_to_approved(AIRLINK_MESH_MSG_CONFIG_COMMIT, &commit, sizeof(commit));
    xSemaphoreGive(s_lock);
    ready = false; failed = false;
    while (esp_timer_get_time() < deadline) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ready = transaction_nodes_ready(true, &failed);
        xSemaphoreGive(s_lock);
        if (ready || failed) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config_transaction_active = false;
    xSemaphoreGive(s_lock);
    /* COMMIT is irrevocable once delivered to any node.  Missing commit ACKs
     * are reported through the post-reboot node state, but must not make the
     * root keep the old network and split a successfully prepared fleet. */
    if (!ready || failed) {
        ESP_LOGW(TAG, "commit acknowledgements incomplete; activating prepared root config");
    }
    /* Air nodes acknowledge before their one-second delayed restart.  Keep the
     * old root available through that delay, then switch and restart it last. */
    vTaskDelay(pdMS_TO_TICKS(1200));
    return airlink_mesh_config_commit_staged();
}

static esp_err_t node_config_exchange(const uint8_t sta_mac[6], const void *request,
                                      size_t request_length, airlink_config_t *response,
                                      uint32_t timeout_ms)
{
    if (sta_mac == NULL || request == NULL || timeout_ms == 0 ||
        s_config.role != AIRLINK_MESH_ROLE_GROUND_ROOT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    node_slot_t *node = find_node(sta_mac);
    if (node == NULL || !node->info.approved || !node->info.online ||
        node->info.isolation_reason != AIRLINK_MESH_ISOLATION_NONE || s_node_config_operation) {
        xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE;
    }
    s_node_config_operation = true; s_node_config_done = false;
    s_node_config_result = CONFIG_RESULT_STORAGE;
    esp_fill_random(&s_node_config_token, sizeof(s_node_config_token));
    if (s_node_config_token == 0) s_node_config_token = 1;
    memcpy(s_node_config_target, sta_mac, 6);
    uint8_t wire[sizeof(node_config_write_request_t)];
    memcpy(wire, request, request_length);
    memcpy(wire + offsetof(node_config_read_request_t, token),
           &s_node_config_token, sizeof(s_node_config_token));
    const bool sent = root_ota_send_one((size_t)(node - s_nodes),
                                        AIRLINK_MESH_MSG_CONFIG_GET, wire, request_length);
    xSemaphoreGive(s_lock);
    if (!sent) { s_node_config_operation = false; return ESP_FAIL; }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool done = s_node_config_done;
        xSemaphoreGive(s_lock);
        if (done) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool done = s_node_config_done;
    const uint8_t result = s_node_config_result;
    if (done && result == CONFIG_RESULT_OK && response != NULL) *response = s_node_config_value;
    s_node_config_operation = false; s_node_config_done = false;
    xSemaphoreGive(s_lock);
    if (!done) return ESP_ERR_TIMEOUT;
    return result == CONFIG_RESULT_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t airlink_mesh_node_config_get(const uint8_t sta_mac[6],
                                       airlink_config_t *config, uint32_t timeout_ms)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    node_config_read_request_t request = {.kind = CONFIG_KIND_READ_REQUEST};
    return node_config_exchange(sta_mac, &request, sizeof(request), config, timeout_ms);
}

esp_err_t airlink_mesh_node_config_set(const uint8_t sta_mac[6],
                                       const airlink_config_t *config,
                                       uint32_t timeout_ms)
{
    if (config == NULL || !airlink_config_validate(config) ||
        config->bridge_role != AIRLINK_BRIDGE_OFF || config->bridge_enabled ||
        !airlink_mesh_global_safe()) return ESP_ERR_INVALID_STATE;
    node_config_write_request_t request = {.kind = CONFIG_KIND_WRITE_REQUEST};
    memcpy(&request.config, config, sizeof(*config));
    return node_config_exchange(sta_mac, &request, sizeof(request), NULL, timeout_ms);
}

esp_err_t airlink_mesh_reboot_node(const uint8_t sta_mac[6])
{
    if (sta_mac == NULL || s_config.role != AIRLINK_MESH_ROLE_GROUND_ROOT ||
        !airlink_mesh_global_safe()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    node_slot_t *node = find_node(sta_mac);
    const bool valid = node != NULL && node->info.approved && node->info.online &&
                       node->info.isolation_reason == AIRLINK_MESH_ISOLATION_NONE;
    const size_t index = valid ? (size_t)(node - s_nodes) : 0;
    const bool sent = valid && root_ota_send_one(index, AIRLINK_MESH_MSG_REBOOT, NULL, 0);
    xSemaphoreGive(s_lock);
    return sent ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool root_ota_node_target(size_t index)
{
    return index < AIRLINK_MESH_MAX_NODES &&
           (s_root_ota.target_mask & (UINT32_C(1) << index)) != 0;
}

static bool root_ota_send_group(airlink_mesh_message_type_t type,
                                const void *payload, size_t payload_length)
{
    uint8_t addresses[AIRLINK_MESH_MAX_NODES * 6U];
    size_t count = 0;
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (!root_ota_node_target(i)) continue;
        memcpy(addresses + count * 6U, s_nodes[i].info.sta_mac, 6); count++;
    }
    if (count == 0) return true;
    mesh_opt_t option = {.type = MESH_OPT_SEND_GROUP, .len = (uint16_t)(count * 6U),
                         .val = addresses};
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return send_encoded(NULL, MESH_DATA_FROMDS | MESH_DATA_GROUP, type,
                        AIRLINK_MESH_FLAG_GROUP, broadcast, payload, payload_length, &option);
}

static bool root_ota_send_one(size_t index, airlink_mesh_message_type_t type,
                              const void *payload, size_t payload_length)
{
    mesh_addr_t to;
    memcpy(to.addr, s_nodes[index].info.sta_mac, sizeof(to.addr));
    return send_encoded(&to, MESH_DATA_FROMDS | MESH_DATA_P2P, type, 0,
                        s_nodes[index].info.sta_mac, payload, payload_length, NULL);
}

static bool root_ota_all_state(uint8_t state)
{
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (root_ota_node_target(i) && s_nodes[i].ota_state != state) return false;
    }
    return true;
}

static bool root_ota_wait_state(uint8_t state, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        bool ready;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ready = root_ota_all_state(state);
        xSemaphoreGive(s_lock);
        if (ready) return true;
        if (!airlink_mesh_global_safe()) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool root_ota_wait_node_state(size_t index, uint8_t state, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool ready = s_nodes[index].ota_state == state;
        xSemaphoreGive(s_lock);
        if (ready) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool root_ota_activation_safe(void)
{
    const int64_t now = esp_timer_get_time();
    bool safe = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        node_slot_t *node = &s_nodes[i];
        if (!node->info.present || !node->info.approved) continue;
        if (root_ota_node_target(i) && node->ota_state == OTA_STATE_ACTIVATING) {
            if (node->info.armed_known && node->info.armed) safe = false;
            continue;
        }
        if (now - node->last_seen_us > NODE_OFFLINE_US ||
            now - node->last_status_us > NODE_STATUS_UNKNOWN_US ||
            !node->info.armed_known || node->info.armed) safe = false;
    }
    xSemaphoreGive(s_lock);
    return safe;
}

static bool root_ota_flush_window(void)
{
    if (s_root_ota.window_bitmap == 0) return true;
    const uint32_t expected = ota_window_expected(s_root_ota.window_base,
                                                   s_root_ota.total_chunks);
    if (s_root_ota.window_bitmap != expected) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (!root_ota_node_target(i)) continue;
        s_nodes[i].ota_ack_base = UINT32_MAX;
        s_nodes[i].ota_ack_bitmap = 0;
        s_nodes[i].ota_state = OTA_STATE_RECEIVING;
    }
    xSemaphoreGive(s_lock);
    uint8_t payload[sizeof(ota_chunk_header_t) + OTA_CHUNK_SIZE];
    const uint32_t count = (uint32_t)__builtin_popcount(expected);
    for (uint32_t slot = 0; slot < count; ++slot) {
        ota_chunk_header_t chunk = {
            .session = s_root_ota.session,
            .chunk_index = s_root_ota.window_base + slot,
            .data_length = s_root_ota.lengths[slot],
        };
        memcpy(payload, &chunk, sizeof(chunk));
        memcpy(payload + sizeof(chunk), s_root_ota.window + slot * OTA_CHUNK_SIZE,
               chunk.data_length);
        if (!root_ota_send_group(AIRLINK_MESH_MSG_OTA_CHUNK, payload,
                                 sizeof(chunk) + chunk.data_length)) return false;
        vTaskDelay(pdMS_TO_TICKS(OTA_RATE_DELAY_MS));
    }
    for (uint32_t round = 0; round <= OTA_REPAIR_ROUNDS; ++round) {
        const int64_t deadline = esp_timer_get_time() + (int64_t)OTA_ACK_TIMEOUT_MS * 1000;
        while (esp_timer_get_time() < deadline) {
            bool complete = true, failed = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
                if (!root_ota_node_target(i)) continue;
                if (s_nodes[i].ota_state == OTA_STATE_ERROR) failed = true;
                if (s_nodes[i].ota_ack_base != s_root_ota.window_base ||
                    s_nodes[i].ota_ack_bitmap != expected) complete = false;
            }
            xSemaphoreGive(s_lock);
            if (failed) return false;
            if (complete) goto window_complete;
            if (!airlink_mesh_global_safe()) return false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (round == OTA_REPAIR_ROUNDS) return false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
            if (!root_ota_node_target(i)) continue;
            const uint32_t missing = expected & ~s_nodes[i].ota_ack_bitmap;
            for (uint32_t slot = 0; slot < count; ++slot) {
                if ((missing & (UINT32_C(1) << slot)) == 0) continue;
                ota_chunk_header_t chunk = {
                    .session = s_root_ota.session,
                    .chunk_index = s_root_ota.window_base + slot,
                    .data_length = s_root_ota.lengths[slot],
                };
                memcpy(payload, &chunk, sizeof(chunk));
                memcpy(payload + sizeof(chunk), s_root_ota.window + slot * OTA_CHUNK_SIZE,
                       chunk.data_length);
                (void)root_ota_send_one(i, AIRLINK_MESH_MSG_OTA_CHUNK, payload,
                                        sizeof(chunk) + chunk.data_length);
            }
        }
        xSemaphoreGive(s_lock);
    }
window_complete:
    s_root_ota.window_base += OTA_WINDOW_CHUNKS;
    s_root_ota.window_bitmap = 0;
    memset(s_root_ota.lengths, 0, sizeof(s_root_ota.lengths));
    return true;
}

static void root_ota_cleanup(bool abort_local)
{
    if (abort_local && airlink_ota_stream_active()) airlink_ota_stream_abort();
    free(s_root_ota.window);
    s_root_ota = (root_ota_state_t){0};
}

void airlink_mesh_ota_abort(void)
{
    if (!s_root_ota.active || s_root_ota.activating) return;
    const uint32_t session = s_root_ota.session;
    (void)root_ota_send_group(AIRLINK_MESH_MSG_OTA_ABORT, &session, sizeof(session));
    root_ota_cleanup(true);
}

bool airlink_mesh_ota_active(void) { return s_root_ota.active; }

esp_err_t airlink_mesh_ota_begin(size_t image_size, const uint8_t sha256[32],
                                 const char *expected_version,
                                 const uint8_t (*targets)[6], size_t target_count,
                                 bool include_root)
{
    if (s_config.role != AIRLINK_MESH_ROLE_GROUND_ROOT || sha256 == NULL ||
        expected_version == NULL || expected_version[0] == '\0' ||
        strlen(expected_version) >= sizeof(s_root_ota.expected_version) || image_size == 0 ||
        image_size > UINT32_MAX || target_count > AIRLINK_MESH_MAX_NODES ||
        s_root_ota.active || !airlink_mesh_global_safe()) return ESP_ERR_INVALID_STATE;
    root_ota_state_t ota = {
        .active = true, .include_root = include_root, .image_size = image_size,
        .total_chunks = (uint32_t)((image_size + OTA_CHUNK_SIZE - 1U) / OTA_CHUNK_SIZE),
    };
    esp_fill_random(&ota.session, sizeof(ota.session));
    if (ota.session == 0) ota.session = 1;
    strlcpy(ota.expected_version, expected_version, sizeof(ota.expected_version));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        node_slot_t *node = &s_nodes[i];
        bool selected = targets == NULL && target_count == 0 && node->info.approved;
        for (size_t target = 0; target < target_count; ++target) {
            if (memcmp(node->info.sta_mac, targets[target], 6) == 0) selected = true;
        }
        if (!selected) continue;
        if (!node->info.approved || !node->info.online || node->info.layer < 2U ||
            node->info.layer > 4U ||
            node->info.isolation_reason != AIRLINK_MESH_ISOLATION_NONE) {
            xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE;
        }
        ota.target_mask |= UINT32_C(1) << i;
    }
    xSemaphoreGive(s_lock);
    if (ota.target_mask == 0 && !include_root) return ESP_ERR_NOT_FOUND;
    if (target_count != 0 && (uint32_t)__builtin_popcount(ota.target_mask) != target_count) {
        return ESP_ERR_NOT_FOUND;
    }
    ota.window = heap_caps_malloc(OTA_WINDOW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ota.window == NULL) return ESP_ERR_NO_MEM;
    s_root_ota = ota;
    atomic_store(&s_ota_abort_requested, false);
    if (include_root && airlink_ota_stream_begin_versioned(
        image_size, sha256, expected_version) != ESP_OK) {
        root_ota_cleanup(false); return ESP_FAIL;
    }
    ota_begin_payload_t begin = {
        .session = ota.session, .image_size = (uint32_t)image_size,
        .chunk_size = OTA_CHUNK_SIZE, .window_chunks = OTA_WINDOW_CHUNKS,
    };
    memcpy(begin.sha256, sha256, sizeof(begin.sha256));
    strlcpy(begin.expected_version, expected_version, sizeof(begin.expected_version));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (root_ota_node_target(i)) s_nodes[i].ota_state = OTA_STATE_IDLE;
    }
    xSemaphoreGive(s_lock);
    if (!root_ota_send_group(AIRLINK_MESH_MSG_OTA_BEGIN, &begin, sizeof(begin)) ||
        !root_ota_wait_state(OTA_STATE_READY, OTA_ACK_TIMEOUT_MS)) {
        airlink_mesh_ota_abort(); return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t airlink_mesh_ota_write(const void *data, size_t length)
{
    if (!s_root_ota.active || s_root_ota.activating || data == NULL || length == 0 ||
        length > OTA_CHUNK_SIZE || s_root_ota.bytes_received + length > s_root_ota.image_size ||
        (s_root_ota.bytes_received + length < s_root_ota.image_size && length != OTA_CHUNK_SIZE) ||
        (!airlink_mesh_global_safe() || atomic_exchange(&s_ota_abort_requested, false))) {
        if (s_root_ota.active) airlink_mesh_ota_abort();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t chunk_index = (uint32_t)(s_root_ota.bytes_received / OTA_CHUNK_SIZE);
    const uint32_t slot = chunk_index - s_root_ota.window_base;
    if (slot >= OTA_WINDOW_CHUNKS) { airlink_mesh_ota_abort(); return ESP_ERR_INVALID_SIZE; }
    memcpy(s_root_ota.window + slot * OTA_CHUNK_SIZE, data, length);
    s_root_ota.lengths[slot] = (uint16_t)length;
    s_root_ota.window_bitmap |= UINT32_C(1) << slot;
    s_root_ota.bytes_received += length;
    if (s_root_ota.include_root && airlink_ota_stream_write(data, length) != ESP_OK) {
        airlink_mesh_ota_abort(); return ESP_FAIL;
    }
    if (slot == OTA_WINDOW_CHUNKS - 1U || s_root_ota.bytes_received == s_root_ota.image_size) {
        if (!root_ota_flush_window()) { airlink_mesh_ota_abort(); return ESP_ERR_TIMEOUT; }
    }
    return ESP_OK;
}

static void root_ota_activation_task(void *argument)
{
    (void)argument;
    for (int layer = 4; layer >= 1; --layer) {
        for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            const bool selected = root_ota_node_target(i) && s_nodes[i].info.layer == layer;
            const int64_t previous_seen = s_nodes[i].last_seen_us;
            if (selected) {
                s_nodes[i].ota_state = OTA_STATE_VERIFIED;
                s_nodes[i].info.ota_image_state = -1;
            }
            xSemaphoreGive(s_lock);
            if (!selected) continue;
            if (!root_ota_activation_safe()) {
                ESP_LOGE(TAG, "OTA activation stopped by fleet safety gate");
                root_ota_cleanup(s_root_ota.include_root); vTaskDelete(NULL); return;
            }
            ota_commit_payload_t activate = {.session = s_root_ota.session, .activate = 1};
            if (!root_ota_send_one(i, AIRLINK_MESH_MSG_OTA_COMMIT, &activate, sizeof(activate)) ||
                !root_ota_wait_node_state(i, OTA_STATE_ACTIVATING, 5000)) {
                ESP_LOGE(TAG, "OTA activation command failed for node %u", (unsigned)i);
                continue;
            }
            const int64_t deadline = esp_timer_get_time() + INT64_C(60000000);
            bool healthy = false;
            while (esp_timer_get_time() < deadline) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                healthy = s_nodes[i].info.online && s_nodes[i].info.status_known &&
                          s_nodes[i].last_seen_us > previous_seen + INT64_C(2000000) &&
                          s_nodes[i].info.ota_image_state == 2 &&
                          strcmp(s_nodes[i].info.firmware, s_root_ota.expected_version) == 0;
                xSemaphoreGive(s_lock);
                if (healthy) break;
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            if (!healthy) ESP_LOGE(TAG, "OTA node %u did not rejoin healthy; continuing", (unsigned)i);
        }
    }
    if (s_root_ota.include_root) {
        if (airlink_ota_stream_staged() && airlink_ota_stream_activate() == ESP_OK) {
            free(s_root_ota.window); s_root_ota.window = NULL;
            schedule_restart();
        } else {
            ESP_LOGE(TAG, "root OTA activation failed"); root_ota_cleanup(true);
        }
    } else {
        root_ota_cleanup(false);
    }
    vTaskDelete(NULL);
}

esp_err_t airlink_mesh_ota_finish(void)
{
    if (!s_root_ota.active || s_root_ota.activating ||
        s_root_ota.bytes_received != s_root_ota.image_size ||
        s_root_ota.window_bitmap != 0 || !airlink_mesh_global_safe() ||
        atomic_exchange(&s_ota_abort_requested, false)) {
        if (s_root_ota.active) airlink_mesh_ota_abort();
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < AIRLINK_MESH_MAX_NODES; ++i) {
        if (root_ota_node_target(i)) s_nodes[i].ota_state = OTA_STATE_RECEIVING;
    }
    xSemaphoreGive(s_lock);
    ota_commit_payload_t verify = {.session = s_root_ota.session, .activate = 0};
    if (!root_ota_send_group(AIRLINK_MESH_MSG_OTA_COMMIT, &verify, sizeof(verify)) ||
        !root_ota_wait_state(OTA_STATE_VERIFIED, 15000) ||
        (s_root_ota.include_root && airlink_ota_stream_verify() != ESP_OK)) {
        airlink_mesh_ota_abort(); return ESP_FAIL;
    }
    s_root_ota.activating = true;
    if (xTaskCreate(root_ota_activation_task, "mesh_ota_activate", 6144,
                    NULL, 7, NULL) != pdPASS) {
        s_root_ota.activating = false; airlink_mesh_ota_abort(); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void airlink_mesh_prepare_restart(void)
{
    atomic_store(&s_running, false);
    if (s_status.started) (void)esp_mesh_stop();
}
