// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRLINK_ESCAPE_PATTERN_MAX 32U

typedef bool (*airlink_stream_emit_fn)(const uint8_t *data, size_t length, void *context);

typedef struct {
    uint8_t bytes[AIRLINK_ESCAPE_PATTERN_MAX];
    size_t length;
    uint64_t started_us;
} airlink_escape_matcher_t;

typedef enum {
    AIRLINK_ESCAPE_PENDING = 0,
    AIRLINK_ESCAPE_MATCHED,
    AIRLINK_ESCAPE_EMIT_FAILED,
} airlink_escape_result_t;

size_t airlink_stream_chunk_size(size_t remaining, size_t maximum);
airlink_escape_result_t airlink_escape_feed(airlink_escape_matcher_t *matcher,
                                             const uint8_t *pattern, size_t pattern_length,
                                             const uint8_t *input, size_t input_length,
                                             uint64_t now_us, airlink_stream_emit_fn emit,
                                             void *context, size_t *consumed);
bool airlink_escape_flush_expired(airlink_escape_matcher_t *matcher, uint64_t now_us,
                                  uint64_t timeout_us, airlink_stream_emit_fn emit,
                                  void *context);

