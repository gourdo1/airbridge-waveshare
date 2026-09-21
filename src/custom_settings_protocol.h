#pragma once

#include <stddef.h>
#include <stdint.h>

namespace CustomSettingsProtocol {

enum entry_kind_t : uint8_t {
    ENTRY_NUMERIC,
    ENTRY_ENUM,
};

struct header_t {
    uint8_t version;
    uint8_t count;
};

struct entry_t {
    entry_kind_t kind;
    uint8_t category;
    uint32_t mop_mask;
    char name[4];
    int16_t scale;
    int16_t step;
    uint8_t decimals;
    const char *units;
    size_t units_len;
    const char *groups;
    uint8_t group_count;
    const char *label;
    size_t label_len;
};

struct numeric_caps_t {
    uint8_t flags;
    uint8_t width;
    uint32_t minimum;
    uint32_t maximum;
};

struct enum_option_t {
    uint8_t count;
    const char *label;
    size_t label_len;
};

bool parse_header(const char *value, header_t &out);
constexpr bool supported_version(uint8_t version) { return version == 1 || version == 2; }
bool parse_entry(const char *value, uint8_t version, entry_t &out);
// Consume a space followed by LL:TEXT, advancing by bytes rather than words.
bool parse_text(const char *&cursor, const char *&text, size_t &length);
bool parse_numeric_caps(const char *value, numeric_caps_t &out);
bool parse_enum_flags(const char *value, uint8_t &flags);
bool parse_enum_option(const char *value, enum_option_t &out);
bool parse_hex_value(const char *value, uint32_t &out, uint8_t *width = nullptr);

}  // namespace CustomSettingsProtocol
