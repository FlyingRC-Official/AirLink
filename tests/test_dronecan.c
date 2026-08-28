// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airlink_dronecan.h"
#include "canard.h"
#include "uavcan.protocol.GetNodeInfo.h"
#include "uavcan.protocol.NodeStatus.h"
#include "uavcan.tunnel.Targetted.h"

typedef struct {
    bool received;
    uint8_t transfer_id;
    struct uavcan_tunnel_Targetted message;
} receiver_state_t;

static bool should_accept(const CanardInstance *instance, uint64_t *signature,
                          uint16_t data_type_id, CanardTransferType transfer_type,
                          uint8_t source_node_id)
{
    (void)instance;
    (void)source_node_id;
    if (transfer_type == CanardTransferTypeBroadcast &&
        data_type_id == UAVCAN_TUNNEL_TARGETTED_ID) {
        *signature = UAVCAN_TUNNEL_TARGETTED_SIGNATURE;
        return true;
    }
    return false;
}

static void on_transfer(CanardInstance *instance, CanardRxTransfer *transfer)
{
    receiver_state_t *state = canardGetUserReference(instance);
    assert(transfer->data_type_id == UAVCAN_TUNNEL_TARGETTED_ID);
    assert(!uavcan_tunnel_Targetted_decode(transfer, &state->message));
    state->transfer_id = transfer->transfer_id;
    state->received = true;
}

static void test_targetted_golden_vector(void)
{
    struct uavcan_tunnel_Targetted message = {0};
    message.protocol.protocol = UAVCAN_TUNNEL_PROTOCOL_MAVLINK2;
    message.target_node = 10;
    message.serial_id = 0;
    message.options = 0;
    message.baudrate = 115200;
    message.buffer.len = 4;
    memcpy(message.buffer.data, (const uint8_t[]){0xfd, 1, 2, 3}, 4);

    uint8_t encoded[UAVCAN_TUNNEL_TARGETTED_MAX_SIZE] = {0};
    const uint32_t length = uavcan_tunnel_Targetted_encode(&message, encoded);
    static const uint8_t expected[] = {
        0x01, 0x14, 0x00, 0x00, 0xc2, 0x01, 0xfd, 0x01, 0x02, 0x03,
    };
    assert(length == sizeof(expected));
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);

    CanardRxTransfer transfer = {
        .payload_head = encoded,
        .payload_len = (uint16_t)length,
    };
    struct uavcan_tunnel_Targetted decoded = {0};
    assert(!uavcan_tunnel_Targetted_decode(&transfer, &decoded));
    assert(decoded.protocol.protocol == UAVCAN_TUNNEL_PROTOCOL_MAVLINK2);
    assert(decoded.target_node == 10 && decoded.serial_id == 0);
    assert(decoded.baudrate == 115200 && decoded.buffer.len == 4);
    assert(memcmp(decoded.buffer.data, message.buffer.data, 4) == 0);
}

static void test_node_encodings(void)
{
    struct uavcan_protocol_NodeStatus status = {
        .uptime_sec = UINT32_C(0x12345678),
        .health = UAVCAN_PROTOCOL_NODESTATUS_HEALTH_OK,
        .mode = UAVCAN_PROTOCOL_NODESTATUS_MODE_OPERATIONAL,
        .vendor_specific_status_code = 1,
    };
    uint8_t encoded[UAVCAN_PROTOCOL_NODESTATUS_MAX_SIZE] = {0};
    const uint32_t length = uavcan_protocol_NodeStatus_encode(&status, encoded);
    static const uint8_t expected[] = {0x78, 0x56, 0x34, 0x12, 0x00, 0x01, 0x00};
    assert(length == sizeof(expected));
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);

    struct uavcan_protocol_GetNodeInfoResponse info = {0};
    info.status = status;
    info.software_version.major = 0;
    info.software_version.minor = 3;
    info.hardware_version.major = 1;
    const char name[] = "com.flyingrc.airlink";
    info.name.len = sizeof(name) - 1U;
    memcpy(info.name.data, name, sizeof(name) - 1U);
    uint8_t response[UAVCAN_PROTOCOL_GETNODEINFO_RESPONSE_MAX_SIZE] = {0};
    const uint32_t response_length =
        uavcan_protocol_GetNodeInfoResponse_encode(&info, response);
    assert(response_length > sizeof(name));
    assert(memcmp(response + response_length - (sizeof(name) - 1U),
                  name, sizeof(name) - 1U) == 0);
}

static void test_multiframe_crc_and_transfer_id(void)
{
    _Alignas(max_align_t) uint8_t tx_arena[8192] = {0};
    _Alignas(max_align_t) uint8_t rx_arena[8192] = {0};
    CanardInstance sender;
    CanardInstance receiver;
    receiver_state_t state = {0};
    canardInit(&sender, tx_arena, sizeof(tx_arena), NULL, NULL, NULL);
    canardInit(&receiver, rx_arena, sizeof(rx_arena), on_transfer, should_accept, &state);
    canardSetLocalNodeID(&sender, 125);
    canardSetLocalNodeID(&receiver, 10);

    struct uavcan_tunnel_Targetted message = {0};
    message.protocol.protocol = UAVCAN_TUNNEL_PROTOCOL_MAVLINK2;
    message.target_node = 10;
    message.baudrate = 115200;
    message.buffer.len = AIRLINK_DRONECAN_TUNNEL_PAYLOAD_MAX;
    for (uint8_t i = 0; i < message.buffer.len; ++i) message.buffer.data[i] = i;
    uint8_t encoded[UAVCAN_TUNNEL_TARGETTED_MAX_SIZE] = {0};
    const uint16_t payload_length =
        (uint16_t)uavcan_tunnel_Targetted_encode(&message, encoded);
    assert(payload_length == 126U);

    uint8_t transfer_id = 7;
    const int16_t frame_count = canardBroadcast(
        &sender, UAVCAN_TUNNEL_TARGETTED_SIGNATURE, UAVCAN_TUNNEL_TARGETTED_ID,
        &transfer_id, CANARD_TRANSFER_PRIORITY_MEDIUM, encoded, payload_length);
    assert(frame_count == 19);
    assert(transfer_id == 8);

    int frames_seen = 0;
    while (canardPeekTxQueue(&sender) != NULL) {
        const CanardCANFrame frame = *canardPeekTxQueue(&sender);
        const uint8_t tail = frame.data[frame.data_len - 1U];
        assert((tail & 0x1fU) == 7U);
        if (frames_seen == 0) assert((tail & 0x80U) != 0U && (tail & 0x40U) == 0U);
        if (frames_seen == frame_count - 1) assert((tail & 0x40U) != 0U);
        assert(canardHandleRxFrame(&receiver, &frame,
                                   UINT64_C(1000) + (uint64_t)frames_seen) >= 0);
        canardPopTxQueue(&sender);
        frames_seen++;
    }
    assert(frames_seen == frame_count);
    assert(state.received && state.transfer_id == 7);
    assert(state.message.buffer.len == AIRLINK_DRONECAN_TUNNEL_PAYLOAD_MAX);
    assert(memcmp(state.message.buffer.data, message.buffer.data,
                  AIRLINK_DRONECAN_TUNNEL_PAYLOAD_MAX) == 0);
}

static void test_filters_chunking_and_keepalive(void)
{
    assert(airlink_dronecan_targetted_matches(10, UAVCAN_TUNNEL_PROTOCOL_MAVLINK2,
                                               125, 0, 125, 10, 0));
    assert(airlink_dronecan_targetted_matches(10, UAVCAN_TUNNEL_PROTOCOL_MAVLINK,
                                               125, 0, 125, 10, 0));
    assert(airlink_dronecan_targetted_matches(10, UAVCAN_TUNNEL_PROTOCOL_UNDEFINED,
                                               125, 0, 125, 10, 0));
    assert(!airlink_dronecan_targetted_matches(11, UAVCAN_TUNNEL_PROTOCOL_MAVLINK2,
                                                125, 0, 125, 10, 0));
    assert(!airlink_dronecan_targetted_matches(10, 99, 125, 0, 125, 10, 0));
    assert(!airlink_dronecan_targetted_matches(10, UAVCAN_TUNNEL_PROTOCOL_MAVLINK2,
                                                124, 0, 125, 10, 0));
    assert(!airlink_dronecan_targetted_matches(10, UAVCAN_TUNNEL_PROTOCOL_MAVLINK2,
                                                125, 1, 125, 10, 0));
    assert(airlink_dronecan_chunk_size(0) == 0);
    assert(airlink_dronecan_chunk_size(120) == 120);
    assert(airlink_dronecan_chunk_size(121) == 120);
    assert(!airlink_dronecan_keepalive_due(499999, 0));
    assert(airlink_dronecan_keepalive_due(500000, 0));
    assert(airlink_dronecan_keepalive_due(900000, 400000));
}

int main(void)
{
    test_targetted_golden_vector();
    test_node_encodings();
    test_multiframe_crc_and_transfer_id();
    test_filters_chunking_and_keepalive();
    puts("DroneCAN tests passed");
    return 0;
}
