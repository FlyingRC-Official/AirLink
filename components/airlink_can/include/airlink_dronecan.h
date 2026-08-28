// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRLINK_DRONECAN_TUNNEL_PAYLOAD_MAX 120U
#define AIRLINK_DRONECAN_KEEPALIVE_INTERVAL_US UINT64_C(500000)

bool airlink_dronecan_targetted_matches(uint8_t source_node_id,
                                        uint8_t protocol,
                                        uint8_t target_node_id,
                                        int8_t serial_id,
                                        uint8_t local_node_id,
                                        uint8_t remote_node_id,
                                        int8_t configured_serial_id);

size_t airlink_dronecan_chunk_size(size_t remaining);
bool airlink_dronecan_keepalive_due(uint64_t now_us, uint64_t last_tx_us);
