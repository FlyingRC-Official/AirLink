// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "airlink_core.h"

typedef struct {
    uint8_t buffer[AIRLINK_MAX_FRAME_SIZE];
    uint16_t length;
    uint16_t expected;
    uint32_t errors;
} airlink_mavlink_parser_t;

typedef struct {
    const uint8_t *bytes;
    size_t length;
    uint32_t message_id;
    uint8_t system_id;
    uint8_t component_id;
    bool mavlink2;
    bool crc_known;
    bool crc_valid;
    bool high_priority;
    bool heartbeat_armed;
} airlink_mavlink_frame_t;

void airlink_mavlink_parser_reset(airlink_mavlink_parser_t *parser);
bool airlink_mavlink_parse_byte(airlink_mavlink_parser_t *parser, uint8_t byte,
                                airlink_mavlink_frame_t *frame);
