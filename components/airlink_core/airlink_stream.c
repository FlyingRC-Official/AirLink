// SPDX-License-Identifier: Apache-2.0
#include "airlink_stream.h"

#include <string.h>

size_t airlink_stream_chunk_size(size_t remaining, size_t maximum)
{
    if (maximum == 0) return 0;
    return remaining > maximum ? maximum : remaining;
}

static size_t suffix_prefix(const uint8_t *bytes, size_t length,
                            const uint8_t *pattern, size_t pattern_length)
{
    size_t candidate = length < pattern_length - 1U ? length : pattern_length - 1U;
    while (candidate > 0) {
        if (memcmp(bytes + length - candidate, pattern, candidate) == 0) return candidate;
        candidate--;
    }
    return 0;
}

airlink_escape_result_t airlink_escape_feed(airlink_escape_matcher_t *matcher,
                                             const uint8_t *pattern, size_t pattern_length,
                                             const uint8_t *input, size_t input_length,
                                             uint64_t now_us, airlink_stream_emit_fn emit,
                                             void *context, size_t *consumed)
{
    if (consumed != NULL) *consumed = 0;
    if (matcher == NULL || pattern == NULL || input == NULL || emit == NULL ||
        pattern_length < 2U || pattern_length > AIRLINK_ESCAPE_PATTERN_MAX) {
        return AIRLINK_ESCAPE_EMIT_FAILED;
    }

    for (size_t i = 0; i < input_length; ++i) {
        uint8_t combined[AIRLINK_ESCAPE_PATTERN_MAX + 1U];
        memcpy(combined, matcher->bytes, matcher->length);
        combined[matcher->length] = input[i];
        const size_t combined_length = matcher->length + 1U;

        if (combined_length == pattern_length && memcmp(combined, pattern, pattern_length) == 0) {
            memset(matcher, 0, sizeof(*matcher));
            if (consumed != NULL) *consumed = i + 1U;
            return AIRLINK_ESCAPE_MATCHED;
        }

        const size_t keep = suffix_prefix(combined, combined_length, pattern, pattern_length);
        const size_t flush = combined_length - keep;
        if (flush > 0 && !emit(combined, flush, context)) return AIRLINK_ESCAPE_EMIT_FAILED;
        if (keep > 0) memcpy(matcher->bytes, combined + flush, keep);
        if (matcher->length == 0 && keep > 0) matcher->started_us = now_us;
        matcher->length = keep;
        if (keep == 0) matcher->started_us = 0;
    }
    if (consumed != NULL) *consumed = input_length;
    return AIRLINK_ESCAPE_PENDING;
}

bool airlink_escape_flush_expired(airlink_escape_matcher_t *matcher, uint64_t now_us,
                                  uint64_t timeout_us, airlink_stream_emit_fn emit,
                                  void *context)
{
    if (matcher == NULL || emit == NULL || matcher->length == 0 ||
        now_us - matcher->started_us < timeout_us) return true;
    const bool result = emit(matcher->bytes, matcher->length, context);
    memset(matcher, 0, sizeof(*matcher));
    return result;
}
