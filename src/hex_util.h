#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class HexCase : uint8_t {
    Lower,
    Upper,
};

inline int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline bool parse_hex(const char *text, size_t width, uint32_t &out,
                       bool uppercase_only = false) {
    if (!text || !width || width > 8) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < width; i++) {
        int nibble = hex_nibble(text[i]);
        if (nibble < 0 || (uppercase_only && text[i] >= 'a' && text[i] <= 'f'))
            return false;
        value = (value << 4) | static_cast<uint32_t>(nibble);
    }
    out = value;
    return true;
}
char hex_digit(uint8_t value, HexCase letter_case);
bool hex_text_valid(const char *value, size_t length);
bool hex_text_normalize(const char *value,
                        size_t length,
                        char *out,
                        size_t out_size,
                        HexCase letter_case);
bool hex_encode(const uint8_t *bytes,
                size_t length,
                char *out,
                size_t out_size,
                HexCase letter_case);
bool hex_decode(const char *text,
                size_t text_length,
                uint8_t *out,
                size_t out_size,
                size_t &decoded_length);
bool sha256_text_valid(const char *value);

}  // namespace aircannect
