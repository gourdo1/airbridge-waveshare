#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Air10Stored {

constexpr size_t MAX_SAMPLES = 10;

struct Value {
    bool present;
    uint8_t byte_count;
    uint8_t sample_count;
    int16_t samples[MAX_SAMPLES];
};

bool parse_value(const uint8_t *payload, size_t payload_len, Value &out);
bool parse_response(const uint8_t *payload, size_t payload_len,
                    const char *tag, uint16_t day, Value &out);
bool unsupported_response(const uint8_t *payload, size_t payload_len,
                          const char *tag, uint16_t day);
bool contains_minute(const Value &value, uint16_t expected,
                     uint16_t tolerance);
bool contains_interval(const Value &starts, const Value &ends,
                       uint16_t expected_start, uint16_t expected_end,
                       uint16_t tolerance);

}  // namespace Air10Stored
