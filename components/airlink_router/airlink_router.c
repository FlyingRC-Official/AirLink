// SPDX-License-Identifier: Apache-2.0
#include "airlink_router.h"

#include <stdatomic.h>
#include <string.h>
#include "airlink_mavlink.h"
#include "airlink_stream.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_ENDPOINTS 16
#define DEDUP_ENTRIES 32
#define DEDUP_WINDOW_US 50000

typedef struct {
    bool used;
    airlink_router_endpoint_t endpoint;
    airlink_mavlink_parser_t parser;
    airlink_endpoint_stats_t stats;
} endpoint_slot_t;

typedef struct { uint32_t crc; int64_t time_us; } dedup_entry_t;

static const char *TAG = "router";
static endpoint_slot_t s_endpoints[MAX_ENDPOINTS];
static dedup_entry_t s_dedup[DEDUP_ENTRIES];
static size_t s_dedup_cursor;
static SemaphoreHandle_t s_lock;
static airlink_route_mode_t s_mode;
static atomic_bool s_fc_armed;
static atomic_int_fast64_t s_fc_last_seen_us;
static bool s_fc_identity_known;
static uint8_t s_fc_system_id;

static endpoint_slot_t *find_endpoint(uint8_t id)
{
    for (size_t i = 0; i < MAX_ENDPOINTS; ++i) {
        if (s_endpoints[i].used && s_endpoints[i].endpoint.id == id) return &s_endpoints[i];
    }
    return NULL;
}

esp_err_t airlink_router_init(airlink_route_mode_t mode)
{
    memset(s_endpoints, 0, sizeof(s_endpoints));
    memset(s_dedup, 0, sizeof(s_dedup));
    s_dedup_cursor = 0;
    atomic_store(&s_fc_armed, false);
    s_fc_identity_known = false;
    s_fc_system_id = 0;
    atomic_store(&s_fc_last_seen_us, 0);
    s_mode = mode;
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t airlink_router_register(const airlink_router_endpoint_t *endpoint)
{
    if (endpoint == NULL || endpoint->send == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (find_endpoint(endpoint->id) != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < MAX_ENDPOINTS; ++i) {
        if (!s_endpoints[i].used) {
            s_endpoints[i].used = true;
            s_endpoints[i].endpoint = *endpoint;
            airlink_mavlink_parser_reset(&s_endpoints[i].parser);
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "registered %s id=%u", endpoint->name, endpoint->id);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NO_MEM;
}

void airlink_router_unregister(uint8_t endpoint_id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    endpoint_slot_t *slot = find_endpoint(endpoint_id);
    if (slot != NULL) memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(s_lock);
}

static bool duplicate_network_frame(const uint8_t *data, size_t length)
{
    const int64_t now = esp_timer_get_time();
    const uint32_t crc = airlink_crc32(data, length);
    for (size_t i = 0; i < DEDUP_ENTRIES; ++i) {
        if (s_dedup[i].crc == crc && now - s_dedup[i].time_us < DEDUP_WINDOW_US) return true;
    }
    s_dedup[s_dedup_cursor] = (dedup_entry_t){crc, now};
    s_dedup_cursor = (s_dedup_cursor + 1U) % DEDUP_ENTRIES;
    return false;
}

static bool vehicle_side(airlink_endpoint_type_t type)
{
    return type == AIRLINK_ENDPOINT_UART || type == AIRLINK_ENDPOINT_BRIDGE;
}

static uint32_t add_u32_saturated(uint32_t value, uint32_t increment)
{
    return UINT32_MAX - value < increment ? UINT32_MAX : value + increment;
}

static uint64_t add_u64_saturated(uint64_t value, size_t increment)
{
    return UINT64_MAX - value < increment ? UINT64_MAX : value + increment;
}

static void observe_vehicle_frame(const endpoint_slot_t *source,
                                  const airlink_mavlink_frame_t *frame)
{
    if (!vehicle_side(source->endpoint.type)) return;
    atomic_store(&s_fc_last_seen_us, esp_timer_get_time());
    if (!airlink_mavlink_heartbeat_is_autopilot(frame)) return;
    if (!s_fc_identity_known) {
        s_fc_identity_known = true;
        s_fc_system_id = frame->system_id;
        ESP_LOGI(TAG, "pinned FC heartbeat sysid=%u compid=%u",
                 frame->system_id, frame->component_id);
    }
    if (frame->system_id == s_fc_system_id) {
        atomic_store(&s_fc_armed, frame->heartbeat_armed);
    }
}

/* Transparent mode preserves every input byte, but still passively observes
 * valid MAVLink heartbeats on vehicle-side streams. This parser never changes
 * or delays the forwarded bytes; it only maintains the armed safety latch. */
static void observe_vehicle_safety(endpoint_slot_t *source,
                                   const uint8_t *data, size_t length)
{
    if (!vehicle_side(source->endpoint.type)) return;
    for (size_t i = 0; i < length; ++i) {
        airlink_mavlink_frame_t frame;
        if (!airlink_mavlink_parse_byte(&source->parser, data[i], &frame)) continue;
        if (frame.crc_known && !frame.crc_valid) continue;
        observe_vehicle_frame(source, &frame);
    }
    source->stats.parse_errors = source->parser.errors;
}

static void route(const endpoint_slot_t *source, const uint8_t *data, size_t length,
                  bool high_priority)
{
    for (size_t i = 0; i < MAX_ENDPOINTS; ++i) {
        endpoint_slot_t *destination = &s_endpoints[i];
        if (!destination->used || destination == source) continue;
        const bool from_vehicle = vehicle_side(source->endpoint.type);
        const bool to_vehicle = vehicle_side(destination->endpoint.type);
        if (from_vehicle == to_vehicle) continue;
        const esp_err_t err = destination->endpoint.send(data, length, high_priority,
                                                          destination->endpoint.context);
        if (err == ESP_OK) {
            destination->stats.bytes_out = add_u64_saturated(destination->stats.bytes_out, length);
            destination->stats.frames_out = add_u32_saturated(destination->stats.frames_out, 1U);
        } else {
            destination->stats.queue_drops = add_u32_saturated(destination->stats.queue_drops, 1U);
        }
    }
}

esp_err_t airlink_router_ingest(uint8_t endpoint_id, const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    endpoint_slot_t *source = find_endpoint(endpoint_id);
    if (source == NULL) { xSemaphoreGive(s_lock); return ESP_ERR_NOT_FOUND; }
    source->stats.bytes_in = add_u64_saturated(source->stats.bytes_in, length);
    source->stats.last_activity_us = esp_timer_get_time();

    if (s_mode == AIRLINK_ROUTE_TRANSPARENT) {
        /* Endpoint queues intentionally remain MAVLink-frame sized. Transparent
         * input is a byte stream, so split large UART/TCP reads into ordered
         * queue-safe chunks instead of rejecting the entire read block. */
        observe_vehicle_safety(source, data, length);
        size_t offset = 0;
        while (offset < length) {
            const size_t chunk = airlink_stream_chunk_size(length - offset,
                                                           AIRLINK_MAX_FRAME_SIZE);
            route(source, data + offset, chunk, false);
            source->stats.frames_in = add_u32_saturated(source->stats.frames_in, 1U);
            offset += chunk;
        }
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    for (size_t i = 0; i < length; ++i) {
        airlink_mavlink_frame_t frame;
        if (!airlink_mavlink_parse_byte(&source->parser, data[i], &frame)) continue;
        source->stats.frames_in = add_u32_saturated(source->stats.frames_in, 1U);
        if (frame.crc_known && !frame.crc_valid) {
            source->stats.parse_errors = add_u32_saturated(source->stats.parse_errors, 1U);
            continue;
        }
        if (vehicle_side(source->endpoint.type)) {
            observe_vehicle_frame(source, &frame);
        } else if (duplicate_network_frame(frame.bytes, frame.length)) {
            continue;
        }
        route(source, frame.bytes, frame.length, frame.high_priority);
    }
    source->stats.parse_errors = source->parser.errors;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void airlink_router_set_mode(airlink_route_mode_t mode)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = mode;
    for (size_t i = 0; i < MAX_ENDPOINTS; ++i) airlink_mavlink_parser_reset(&s_endpoints[i].parser);
    xSemaphoreGive(s_lock);
}

bool airlink_router_fc_seen(void)
{
    /* This runs in the watchdog-supervised status task.  Never wait on the
     * routing mutex here: a saturated output queue used to let the high
     * priority UART task starve status until the task watchdog reset the C5. */
    const int64_t last_seen_us = atomic_load(&s_fc_last_seen_us);
    return esp_timer_get_time() - last_seen_us < INT64_C(3000000);
}
/* Once an armed heartbeat has been observed, loss of telemetry is not evidence
 * that the aircraft disarmed.  Keep destructive operations locked until a
 * valid disarmed heartbeat is received or the AirLink itself is restarted. */
bool airlink_router_fc_armed(void) { return atomic_load(&s_fc_armed); }

void airlink_router_get_stats(uint8_t endpoint_id, airlink_endpoint_stats_t *stats)
{
    if (stats == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    endpoint_slot_t *slot = find_endpoint(endpoint_id);
    *stats = slot != NULL ? slot->stats : (airlink_endpoint_stats_t){0};
    xSemaphoreGive(s_lock);
}
