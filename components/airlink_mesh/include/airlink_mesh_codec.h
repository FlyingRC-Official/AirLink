// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRLINK_MESH_PROTOCOL_VERSION 1U
#define AIRLINK_MESH_PROTOCOL_MAGIC UINT32_C(0x314d4c41) /* ALM1 */
#define AIRLINK_MESH_TAG_SIZE 16U
#define AIRLINK_MESH_SESSION_ID_SIZE 16U
#define AIRLINK_MESH_HEADER_SIZE 48U
#define AIRLINK_MESH_MAX_PACKET 1456U
#define AIRLINK_MESH_MAX_PAYLOAD \
    (AIRLINK_MESH_MAX_PACKET - AIRLINK_MESH_HEADER_SIZE - AIRLINK_MESH_TAG_SIZE)

typedef enum {
    AIRLINK_MESH_MSG_HELLO = 1,
    AIRLINK_MESH_MSG_APPROVAL = 2,
    AIRLINK_MESH_MSG_MAVLINK_UP = 3,
    AIRLINK_MESH_MSG_MAVLINK_DOWN = 4,
    AIRLINK_MESH_MSG_NODE_STATUS = 5,
    AIRLINK_MESH_MSG_CONFIG_GET = 16,
    AIRLINK_MESH_MSG_CONFIG_RESPONSE = 17,
    AIRLINK_MESH_MSG_CONFIG_PREPARE = 18,
    AIRLINK_MESH_MSG_CONFIG_COMMIT = 19,
    AIRLINK_MESH_MSG_CONFIG_ABORT = 20,
    AIRLINK_MESH_MSG_REBOOT = 21,
    AIRLINK_MESH_MSG_OTA_BEGIN = 32,
    AIRLINK_MESH_MSG_OTA_CHUNK = 33,
    AIRLINK_MESH_MSG_OTA_ACK = 34,
    AIRLINK_MESH_MSG_OTA_COMMIT = 35,
    AIRLINK_MESH_MSG_OTA_ABORT = 36,
    AIRLINK_MESH_MSG_ERROR = 127,
} airlink_mesh_message_type_t;

enum {
    AIRLINK_MESH_FLAG_FROM_ROOT = 1U << 0,
    AIRLINK_MESH_FLAG_HIGH_PRIORITY = 1U << 1,
    AIRLINK_MESH_FLAG_GROUP = 1U << 2,
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint8_t source[6];
    uint8_t destination[6];
    uint8_t session_id[AIRLINK_MESH_SESSION_ID_SIZE];
    uint64_t sequence;
    uint16_t payload_length;
    uint16_t reserved;
} airlink_mesh_wire_header_t;

typedef struct {
    uint8_t fleet_key[32];
    uint8_t network_id[6];
    uint8_t local_mac[6];
    uint8_t session_id[AIRLINK_MESH_SESSION_ID_SIZE];
    uint64_t next_sequence;
    bool from_root;
} airlink_mesh_crypto_context_t;

typedef struct {
    bool initialized;
    uint8_t source[6];
    uint8_t session_id[AIRLINK_MESH_SESSION_ID_SIZE];
    uint64_t highest_sequence;
    uint64_t seen_bitmap;
    uint8_t retired_session_ids[3][AIRLINK_MESH_SESSION_ID_SIZE];
    uint8_t retired_session_count;
    uint8_t retired_session_next;
} airlink_mesh_replay_window_t;

typedef struct {
    airlink_mesh_wire_header_t header;
    uint8_t payload[AIRLINK_MESH_MAX_PAYLOAD];
    size_t payload_length;
} airlink_mesh_decoded_packet_t;

typedef enum {
    AIRLINK_MESH_DECODE_OK = 0,
    AIRLINK_MESH_DECODE_MALFORMED = 1,
    AIRLINK_MESH_DECODE_AUTH_FAILED = 2,
    AIRLINK_MESH_DECODE_REPLAY = 3,
} airlink_mesh_decode_result_t;

bool airlink_mesh_derive_softap_password(const uint8_t fleet_key[32],
                                         const uint8_t network_id[6],
                                         char output[44]);
bool airlink_mesh_encode(airlink_mesh_crypto_context_t *context,
                         airlink_mesh_message_type_t type, uint16_t flags,
                         const uint8_t destination[6], const void *payload,
                         size_t payload_length, uint8_t *output,
                         size_t output_capacity, size_t *output_length);
bool airlink_mesh_decode(const uint8_t fleet_key[32], const uint8_t network_id[6],
                         const uint8_t *packet, size_t packet_length,
                         airlink_mesh_replay_window_t *replay,
                         airlink_mesh_decoded_packet_t *decoded);
airlink_mesh_decode_result_t airlink_mesh_decode_ex(
    const uint8_t fleet_key[32], const uint8_t network_id[6],
    const uint8_t *packet, size_t packet_length,
    airlink_mesh_replay_window_t *replay,
    airlink_mesh_decoded_packet_t *decoded);
