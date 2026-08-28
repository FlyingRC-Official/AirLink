// SPDX-License-Identifier: Apache-2.0
#include "airlink_dronecan.h"

#include "uavcan.tunnel.Protocol.h"

bool airlink_dronecan_targetted_matches(uint8_t source_node_id,
                                        uint8_t protocol,
                                        uint8_t target_node_id,
                                        int8_t serial_id,
                                        uint8_t local_node_id,
                                        uint8_t remote_node_id,
                                        int8_t configured_serial_id)
{
    const bool protocol_supported = protocol == UAVCAN_TUNNEL_PROTOCOL_UNDEFINED ||
                                    protocol == UAVCAN_TUNNEL_PROTOCOL_MAVLINK ||
                                    protocol == UAVCAN_TUNNEL_PROTOCOL_MAVLINK2;
    return protocol_supported && source_node_id == remote_node_id &&
           target_node_id == local_node_id && serial_id == configured_serial_id;
}

size_t airlink_dronecan_chunk_size(size_t remaining)
{
    return remaining > AIRLINK_DRONECAN_TUNNEL_PAYLOAD_MAX ?
           AIRLINK_DRONECAN_TUNNEL_PAYLOAD_MAX : remaining;
}

bool airlink_dronecan_keepalive_due(uint64_t now_us, uint64_t last_tx_us)
{
    return now_us - last_tx_us >= AIRLINK_DRONECAN_KEEPALIVE_INTERVAL_US;
}
