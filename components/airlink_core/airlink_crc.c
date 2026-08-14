// SPDX-License-Identifier: Apache-2.0
#include "airlink_core.h"

uint32_t airlink_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = -(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}
