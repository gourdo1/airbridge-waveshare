#include "custom_settings_protocol.h"

#include <string.h>

namespace CustomSettingsProtocol {
namespace {

bool hex_digit(char c, uint8_t &out) {
    if (c >= '0' && c <= '9') {
        out = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    return false;
}

bool fixed_hex(const char *text, size_t digits, uint32_t &out) {
    if (!text || digits == 0 || digits > 8) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < digits; i++) {
        uint8_t nibble = 0;
        if (!hex_digit(text[i], nibble)) return false;
        value = (value << 4) | nibble;
    }
    out = value;
    return true;
}

bool token(const char *&cursor, const char *&start, size_t &length) {
    if (!cursor || *cursor == '\0' || *cursor == ' ') return false;
    start = cursor;
    while (*cursor && *cursor != ' ') cursor++;
    length = (size_t)(cursor - start);
    return length > 0;
}

bool separator(const char *&cursor) {
    if (*cursor != ' ') return false;
    cursor++;
    return *cursor != ' ';
}

bool fixed_hex_token(const char *start, size_t length, size_t expected,
                     uint32_t &out) {
    return length == expected && fixed_hex(start, expected, out);
}

bool variable_name(const char *start, size_t length, char out[4]) {
    if (length != 3) return false;
    for (size_t i = 0; i < 3; i++) {
        char c = start[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        out[i] = c;
    }
    out[3] = '\0';
    return true;
}

}  // namespace

bool parse_header(const char *value, header_t &out) {
    if (!value) return false;
    const char *cursor = value;
    const char *start = nullptr;
    size_t length = 0;
    uint32_t version = 0;
    uint32_t count = 0;

    if (!token(cursor, start, length) ||
        !fixed_hex_token(start, length, 2, version) ||
        !separator(cursor) ||
        !token(cursor, start, length) ||
        !fixed_hex_token(start, length, 2, count) || *cursor != '\0') {
        return false;
    }

    out.version = (uint8_t)version;
    out.count = (uint8_t)count;
    return true;
}

bool parse_entry(const char *value, entry_t &out) {
    if (!value) return false;
    memset(&out, 0, sizeof(out));

    const char *cursor = value;
    const char *start = nullptr;
    size_t length = 0;
    if (!token(cursor, start, length) || length != 2 || start[0] != 'V' ||
        (start[1] != '4' && start[1] != '8')) {
        return false;
    }
    out.kind = start[1] == '4' ? ENTRY_NUMERIC : ENTRY_ENUM;

    uint32_t field = 0;
    if (!separator(cursor) || !token(cursor, start, length) ||
        !fixed_hex_token(start, length, 2, field) || field > 4) {
        return false;
    }
    out.category = (uint8_t)field;

    if (!separator(cursor) || !token(cursor, start, length) ||
        !fixed_hex_token(start, length, 8, out.mop_mask)) {
        return false;
    }

    if (!separator(cursor) || !token(cursor, start, length) ||
        !variable_name(start, length, out.name)) {
        return false;
    }

    if (out.kind == ENTRY_ENUM) {
        if (*cursor == '\0') {
            out.label = cursor;
            return true;
        }
        if (!separator(cursor)) return false;
        out.label = cursor;
        out.label_len = strlen(cursor);
        return true;
    }

    uint32_t raw_scale = 0;
    uint32_t raw_step = 0;
    if (!separator(cursor) || !token(cursor, start, length) ||
        !fixed_hex_token(start, length, 4, raw_scale) ||
        !separator(cursor) || !token(cursor, start, length) ||
        !fixed_hex_token(start, length, 4, raw_step) ||
        !separator(cursor) || !token(cursor, start, length) ||
        !fixed_hex_token(start, length, 2, field)) {
        return false;
    }
    out.scale = (int16_t)(uint16_t)raw_scale;
    out.step = (int16_t)(uint16_t)raw_step;
    out.decimals = (uint8_t)field;

    if (!separator(cursor) || strlen(cursor) < 3 || cursor[2] != ':' ||
        !fixed_hex(cursor, 2, field)) {
        return false;
    }
    cursor += 3;
    if (strlen(cursor) < field) return false;
    out.units = cursor;
    out.units_len = field;
    cursor += field;

    if (*cursor == '\0') {
        out.label = cursor;
        return true;
    }
    if (*cursor != ' ') return false;
    cursor++;
    out.label = cursor;
    out.label_len = strlen(cursor);
    return true;
}

bool parse_numeric_caps(const char *value, numeric_caps_t &out) {
    if (!value) return false;
    const char *cursor = value;
    const char *start = nullptr;
    size_t length = 0;
    uint32_t flags = 0;

    if (!token(cursor, start, length) ||
        !fixed_hex_token(start, length, 1, flags) ||
        !separator(cursor) || !token(cursor, start, length)) {
        return false;
    }
    size_t width = length;
    uint32_t minimum = 0;
    if (width == 0 || width > 8 || !fixed_hex(start, width, minimum) ||
        !separator(cursor) || !token(cursor, start, length) ||
        length != width) {
        return false;
    }
    uint32_t maximum = 0;
    if (!fixed_hex(start, width, maximum) || *cursor != '\0') return false;

    out.flags = (uint8_t)flags;
    out.width = (uint8_t)width;
    out.minimum = minimum;
    out.maximum = maximum;
    return true;
}

bool parse_enum_flags(const char *value, uint8_t &flags) {
    if (!value || value[0] == '\0' || value[1] != '\0') return false;
    uint8_t nibble = 0;
    if (!hex_digit(value[0], nibble)) return false;
    flags = nibble;
    return true;
}

bool parse_enum_option(const char *value, enum_option_t &out) {
    if (!value) return false;
    const char *cursor = value;
    const char *start = nullptr;
    size_t length = 0;
    uint32_t count = 0;
    uint32_t permission = 0;

    if (!token(cursor, start, length) ||
        !fixed_hex_token(start, length, 2, count) ||
        !separator(cursor) || !token(cursor, start, length) ||
        !fixed_hex_token(start, length, 1, permission) || permission != 1) {
        return false;
    }

    out.count = (uint8_t)count;
    if (*cursor == '\0') {
        out.label = cursor;
        out.label_len = 0;
        return true;
    }
    if (!separator(cursor)) return false;
    out.label = cursor;
    out.label_len = strlen(cursor);
    return true;
}

bool parse_hex_value(const char *value, uint32_t &out, uint8_t *width) {
    if (!value) return false;
    size_t length = strlen(value);
    if (length == 0 || length > 8 || !fixed_hex(value, length, out)) return false;
    if (width) *width = (uint8_t)length;
    return true;
}

}  // namespace CustomSettingsProtocol
