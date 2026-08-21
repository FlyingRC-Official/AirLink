// SPDX-License-Identifier: Apache-2.0
#include "airlink_mavlink.h"

#include <string.h>

typedef struct { uint32_t id; uint8_t crc_extra; } crc_entry_t;

/* MAVLink common.xml CRC extras for messages needed by AirLink routing and
 * flight-safety decisions. Unknown dialect messages remain structurally
 * routable but are never used to authorize a client or change safety state. */
static const crc_entry_t CRC_EXTRAS[] = {
    {0, 50}, {1, 124}, {2, 137}, {4, 237}, {11, 89},
    {20, 214}, {21, 159}, {22, 220}, {23, 168}, {24, 24},
    {25, 23}, {26, 170}, {27, 144}, {29, 115}, {30, 39},
    {31, 246}, {32, 185}, {33, 104}, {35, 244}, {36, 222},
    {37, 212}, {38, 9}, {39, 254}, {40, 230}, {41, 28},
    {42, 28}, {43, 132}, {44, 221}, {45, 232}, {46, 11},
    {47, 153}, {51, 196}, {62, 183}, {65, 118}, {66, 148},
    {69, 243}, {70, 124}, {73, 38}, {74, 20}, {75, 158},
    {76, 152}, {77, 143}, {82, 49}, {84, 143}, {86, 5},
    {109, 185}, {110, 84}, {111, 34}, {125, 203}, {126, 220},
    {147, 154}, {148, 178}, {152, 208}, {193, 71}, {230, 163},
    {231, 105}, {241, 90}, {242, 104}, {245, 130}, {253, 83},
    {254, 46}, {256, 71}, {257, 131}, {258, 187}, {259, 92},
    {260, 146}, {261, 179}, {262, 12}, {263, 133}, {264, 49},
    {265, 26}, {266, 193}, {267, 35}, {268, 14}, {269, 109},
    {270, 59}, {280, 70}, {281, 48}, {282, 123}, {283, 74},
    {284, 99}, {285, 137}, {286, 210}, {287, 1}, {288, 20},
    {299, 19}, {301, 243}, {310, 28}, {311, 95}, {320, 243},
    {321, 88}, {322, 243}, {323, 78}, {324, 132}, {330, 23},
    {331, 91}, {332, 236}, {333, 231}, {334, 72}, {335, 225},
};

static bool crc_extra_for(uint32_t id, uint8_t *extra)
{
    size_t low = 0, high = sizeof(CRC_EXTRAS) / sizeof(CRC_EXTRAS[0]);
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (CRC_EXTRAS[mid].id == id) { *extra = CRC_EXTRAS[mid].crc_extra; return true; }
        if (CRC_EXTRAS[mid].id < id) low = mid + 1; else high = mid;
    }
    return false;
}

static void crc_accumulate(uint8_t data, uint16_t *crc)
{
    uint8_t tmp = data ^ (uint8_t)(*crc & 0xffU);
    tmp ^= (uint8_t)(tmp << 4U);
    *crc = (uint16_t)((*crc >> 8U) ^ ((uint16_t)tmp << 8U) ^
                      ((uint16_t)tmp << 3U) ^ ((uint16_t)tmp >> 4U));
}

static bool priority_message(uint32_t id)
{
    if (id == 11 || id == 23 || id == 69 || id == 70 || id == 75 || id == 76 || id == 77) return true;
    if (id >= 37 && id <= 51) return true;
    if (id == 66 || id == 82 || id == 84 || id == 86 || id == 110) return true;
    return id >= 282 && id <= 288;
}

void airlink_mavlink_parser_reset(airlink_mavlink_parser_t *parser)
{
    if (parser != NULL) *parser = (airlink_mavlink_parser_t){0};
}

static bool finish_frame(airlink_mavlink_parser_t *parser, airlink_mavlink_frame_t *frame)
{
    const bool v2 = parser->buffer[0] == 0xfd;
    const uint8_t payload_length = parser->buffer[1];
    const size_t header_length = v2 ? 10U : 6U;
    const size_t crc_offset = header_length + payload_length;
    const uint32_t message_id = v2 ?
        ((uint32_t)parser->buffer[7] | ((uint32_t)parser->buffer[8] << 8U) |
         ((uint32_t)parser->buffer[9] << 16U)) : parser->buffer[5];
    uint8_t extra = 0;
    const bool known = crc_extra_for(message_id, &extra);
    bool crc_valid = false;
    if (known) {
        uint16_t crc = UINT16_C(0xffff);
        for (size_t i = 1; i < crc_offset; ++i) crc_accumulate(parser->buffer[i], &crc);
        crc_accumulate(extra, &crc);
        const uint16_t received = (uint16_t)parser->buffer[crc_offset] |
                                  ((uint16_t)parser->buffer[crc_offset + 1U] << 8U);
        crc_valid = crc == received;
        if (!crc_valid) parser->errors++;
    }
    const size_t payload_offset = header_length;
    const bool heartbeat_valid = message_id == 0 && known && crc_valid && payload_length >= 9;
    const bool armed = heartbeat_valid &&
                       (parser->buffer[payload_offset + 6U] & 0x80U) != 0;
    *frame = (airlink_mavlink_frame_t){
        .bytes = parser->buffer,
        .length = parser->length,
        .message_id = message_id,
        .system_id = parser->buffer[v2 ? 5 : 3],
        .component_id = parser->buffer[v2 ? 6 : 4],
        .mavlink2 = v2,
        .crc_known = known,
        .crc_valid = crc_valid,
        .high_priority = priority_message(message_id),
        .heartbeat_valid = heartbeat_valid,
        .heartbeat_armed = armed,
        .heartbeat_type = heartbeat_valid ? parser->buffer[payload_offset + 4U] : 0,
        .heartbeat_autopilot = heartbeat_valid ? parser->buffer[payload_offset + 5U] : 0,
    };
    return true;
}

bool airlink_mavlink_heartbeat_is_autopilot(const airlink_mavlink_frame_t *frame)
{
    /* MAV_COMP_ID_AUTOPILOT1 is 1 and MAV_AUTOPILOT_INVALID is 8.  Pinning the
     * safety state to the primary autopilot component prevents companion,
     * camera and GCS heartbeats on the FC UART from clearing an armed latch. */
    return frame != NULL && frame->heartbeat_valid && frame->component_id == 1U &&
           frame->heartbeat_autopilot != 8U;
}

bool airlink_mavlink_parse_byte(airlink_mavlink_parser_t *parser, uint8_t byte,
                                airlink_mavlink_frame_t *frame)
{
    if (parser == NULL || frame == NULL) return false;
    if (parser->length == 0) {
        if (byte != 0xfe && byte != 0xfd) return false;
        parser->buffer[parser->length++] = byte;
        return false;
    }
    if (parser->length >= sizeof(parser->buffer)) {
        parser->errors++;
        parser->length = parser->expected = 0;
        return false;
    }
    parser->buffer[parser->length++] = byte;
    if (parser->length == 2) {
        parser->expected = (parser->buffer[0] == 0xfd ? 12U : 8U) + byte;
    } else if (parser->buffer[0] == 0xfd && parser->length == 3 && (byte & 0x01U)) {
        parser->expected += 13U;
    }
    if (parser->expected != 0 && parser->length == parser->expected) {
        finish_frame(parser, frame);
        parser->length = parser->expected = 0;
        return true;
    }
    return false;
}
