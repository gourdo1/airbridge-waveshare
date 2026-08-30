#include "air10_stored.h"

#include <string.h>

namespace Air10Stored {
namespace {

int hex_nibble(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool parse_hex(const uint8_t *text, size_t width, uint32_t &value) {
    value = 0;
    for (size_t i = 0; i < width; i++) {
        const int nibble = hex_nibble(text[i]);
        if (nibble < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(nibble);
    }
    return true;
}

bool minute_matches(int32_t sample, uint16_t expected,
                    uint16_t tolerance) {
    if (sample < 0 || sample > 1440 || expected > 1440) return false;
    const uint32_t difference = sample > expected
        ? static_cast<uint32_t>(sample - expected)
        : static_cast<uint32_t>(expected - sample);
    return difference <= tolerance;
}

}  // namespace

bool parse_value(const uint8_t *payload, size_t payload_len, Value &out) {
    out = {};
    if (!payload || payload_len < 10 ||
        memcmp(payload, "FFFF", 4) != 0 ||
        memcmp(payload + payload_len - 4, "FFFF", 4) != 0) {
        return false;
    }

    uint32_t byte_count = 0;
    if (!parse_hex(payload + 4, 2, byte_count) ||
        byte_count > MAX_SAMPLES * sizeof(int16_t) ||
        (byte_count & 1u) != 0 ||
        payload_len != 10 + byte_count * 2) {
        return false;
    }

    out.byte_count = static_cast<uint8_t>(byte_count);
    if (byte_count == 0) return true;

    out.sample_count = static_cast<uint8_t>(byte_count / sizeof(int16_t));
    for (uint8_t i = 0; i < out.sample_count; i++) {
        uint32_t sample = 0;
        if (!parse_hex(payload + 6 + i * 4, 4, sample)) return false;
        out.samples[i] = static_cast<int16_t>(static_cast<uint16_t>(sample));
    }
    out.present = true;
    return true;
}

bool contains_minute(const Value &value, uint16_t expected,
                     uint16_t tolerance) {
    if (!value.present || expected > 1440) return false;
    for (uint8_t i = 0; i < value.sample_count; i++)
        if (minute_matches(value.samples[i], expected, tolerance)) return true;
    return false;
}

bool contains_interval(const Value &starts, const Value &ends,
                       uint16_t expected_start, uint16_t expected_end,
                       uint16_t tolerance) {
    if (!starts.present || !ends.present || expected_start > 1440 ||
        expected_end > 1440) {
        return false;
    }
    const uint8_t count = starts.sample_count < ends.sample_count
        ? starts.sample_count : ends.sample_count;
    for (uint8_t i = 0; i < count; i++) {
        if (minute_matches(starts.samples[i], expected_start, tolerance) &&
            minute_matches(ends.samples[i], expected_end, tolerance)) {
            return true;
        }
    }
    return false;
}

}  // namespace Air10Stored
