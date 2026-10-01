#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Air10Stream {

constexpr uint8_t FIELD_MAX = 8;

struct Field {
    char name[4];
    uint8_t width;
};

struct Schema {
    char tag[4];
    Field fields[FIELD_MAX];
    uint8_t field_count;
    bool from_firmware;
};

struct Frame {
    uint8_t sequence;
    int32_t values[FIELD_MAX];
};

// Shared stock layouts and Airbreak discovery. Resolve only outside UART RX.
bool fallback_schema(Schema &schema, const char *tag, uint16_t mid, uint16_t vid);
bool resolve_schema(Schema &schema, const char *tag, uint16_t mid, uint16_t vid);
bool schema_has_field(const Schema &schema, const char *name);
bool decode_frame(const uint8_t *payload, uint16_t len, const Schema &schema,
                  Frame &decoded);
bool decoded_value(const Schema &schema, const Frame &decoded,
                   const char *name, int16_t &value);
bool decoded_value32(const Schema &schema, const Frame &decoded,
                     const char *name, int32_t &value);

}  // namespace Air10Stream
