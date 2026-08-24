// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airlink_stream.h"

static const uint8_t escape[] = "+++AIRLINK-CLI\r\n";
static uint8_t output[8192];
static size_t output_length;

static bool collect(const uint8_t *data, size_t length, void *context)
{
    (void)context;
    assert(output_length + length <= sizeof(output));
    memcpy(output + output_length, data, length);
    output_length += length;
    return true;
}

static void reset_output(void)
{
    memset(output, 0, sizeof(output));
    output_length = 0;
}

static void test_chunks(void)
{
    const size_t sizes[] = {1, 280, 281, 512, 1024};
    const size_t expected[][4] = {
        {1, 0, 0, 0}, {280, 0, 0, 0}, {280, 1, 0, 0},
        {280, 232, 0, 0}, {280, 280, 280, 184},
    };
    for (size_t case_index = 0; case_index < sizeof(sizes) / sizeof(sizes[0]); ++case_index) {
        size_t remaining = sizes[case_index];
        size_t part = 0;
        while (remaining > 0) {
            const size_t chunk = airlink_stream_chunk_size(remaining, 280);
            assert(chunk == expected[case_index][part++]);
            remaining -= chunk;
        }
    }
    assert(airlink_stream_chunk_size(100, 0) == 0);
}

static void test_every_split(void)
{
    const uint8_t prefix[] = {0x00, 0xff, 'x'};
    const uint8_t tail[] = "status\r\n";
    const size_t pattern_length = sizeof(escape) - 1U;
    for (size_t split = 0; split <= pattern_length; ++split) {
        airlink_escape_matcher_t matcher = {0};
        reset_output();
        uint8_t first[64];
        memcpy(first, prefix, sizeof(prefix));
        memcpy(first + sizeof(prefix), escape, split);
        size_t consumed = 0;
        airlink_escape_result_t result = airlink_escape_feed(
            &matcher, escape, pattern_length, first, sizeof(prefix) + split,
            1000, collect, NULL, &consumed);
        if (split < pattern_length) {
            assert(result == AIRLINK_ESCAPE_PENDING);
            uint8_t second[64];
            memcpy(second, escape + split, pattern_length - split);
            memcpy(second + pattern_length - split, tail, sizeof(tail) - 1U);
            result = airlink_escape_feed(&matcher, escape, pattern_length, second,
                                         pattern_length - split + sizeof(tail) - 1U,
                                         1100, collect, NULL, &consumed);
            assert(result == AIRLINK_ESCAPE_MATCHED);
            assert(consumed == pattern_length - split);
            assert(memcmp(second + consumed, tail, sizeof(tail) - 1U) == 0);
        } else {
            assert(result == AIRLINK_ESCAPE_MATCHED);
            assert(consumed == sizeof(prefix) + pattern_length);
        }
        assert(output_length == sizeof(prefix));
        assert(memcmp(output, prefix, sizeof(prefix)) == 0);
    }
}

static void test_overlap_and_timeout(void)
{
    airlink_escape_matcher_t matcher = {0};
    const uint8_t overlap[] = "++++AIRLINK-CLI\r\nnext";
    size_t consumed = 0;
    reset_output();
    assert(airlink_escape_feed(&matcher, escape, sizeof(escape) - 1U,
                               overlap, sizeof(overlap) - 1U, 1000,
                               collect, NULL, &consumed) == AIRLINK_ESCAPE_MATCHED);
    assert(output_length == 1 && output[0] == '+');
    assert(memcmp(overlap + consumed, "next", 4) == 0);

    memset(&matcher, 0, sizeof(matcher));
    reset_output();
    assert(airlink_escape_feed(&matcher, escape, sizeof(escape) - 1U,
                               escape, 7, 2000, collect, NULL, NULL) == AIRLINK_ESCAPE_PENDING);
    assert(airlink_escape_flush_expired(&matcher, 251999, 250000, collect, NULL));
    assert(output_length == 0);
    assert(airlink_escape_flush_expired(&matcher, 252000, 250000, collect, NULL));
    assert(output_length == 7 && memcmp(output, escape, 7) == 0);
}

static void test_random_roundtrip(void)
{
    uint8_t input[4096];
    uint32_t state = 0x1a2b3c4dU;
    for (size_t i = 0; i < sizeof(input); ++i) {
        state = state * 1664525U + 1013904223U;
        input[i] = (uint8_t)(state >> 24);
    }
    airlink_escape_matcher_t matcher = {0};
    reset_output();
    size_t offset = 0;
    while (offset < sizeof(input)) {
        const size_t length = sizeof(input) - offset > 37 ? 37 : sizeof(input) - offset;
        assert(airlink_escape_feed(&matcher, escape, sizeof(escape) - 1U,
                                   input + offset, length, 1000 + offset,
                                   collect, NULL, NULL) == AIRLINK_ESCAPE_PENDING);
        offset += length;
    }
    assert(airlink_escape_flush_expired(&matcher, 1000000, 250000, collect, NULL));
    assert(output_length == sizeof(input));
    assert(memcmp(output, input, sizeof(input)) == 0);
}

int main(void)
{
    test_chunks();
    test_every_split();
    test_overlap_and_timeout();
    test_random_roundtrip();
    puts("stream tests passed");
    return 0;
}
