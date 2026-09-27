#include "json_cursor.h"
#include "json_util.h"
#include <stdio.h>
#include <string.h>

void JsonCursor::token(const char *text, bool quoted, size_t length) {
    text_ = text;
    length_ = length == SIZE_MAX ? strlen(text) : length;
    offset_ = 0;
    quoted_ = quoted;
    quote_phase_ = 0;
}

void JsonCursor::field(const char *key, const char *text, bool quoted) {
    token(key);
    pending_ = text;
    pending_quoted_ = quoted;
}

void JsonCursor::integer(const char *key, int64_t value) {
    snprintf(number_, sizeof(number_), "%lld", (long long)value);
    field(key, number_, false);
}

void JsonCursor::decimal(const char *key, int64_t raw, int16_t scale, uint8_t places) {
    uint64_t magnitude = raw < 0 ? -raw : raw;
    uint32_t divisor = scale > 0 ? scale : 1;
    if (scale < 0) magnitude *= -(int32_t)scale;
    size_t length = snprintf(number_, sizeof(number_), "%s%llu",
                              raw < 0 ? "-" : "",
                              (unsigned long long)(magnitude / divisor));
    uint32_t remainder = magnitude % divisor;
    if (places) number_[length++] = '.';
    uint8_t written = 0;
    while (written < places && length < sizeof(number_) - 2) {
        remainder *= 10;
        number_[length++] = '0' + remainder / divisor;
        remainder %= divisor;
        written++;
    }
    // Round exact rational values to nearest, ties to even, without newlib's
    // heap-backed floating-point formatting. Keep the bounded text buffer.
    if (written == places && (remainder * 2 > divisor ||
        (remainder * 2 == divisor && ((number_[length - 1] - '0') & 1)))) {
        size_t first = raw < 0 ? 1 : 0;
        size_t digit = length;
        bool carry = true;
        while (digit > first && carry) {
            char &c = number_[--digit];
            if (c == '.') continue;
            if (c == '9') c = '0';
            else { c++; carry = false; }
        }
        if (carry) {
            memmove(number_ + first + 1, number_ + first, length - first);
            number_[first] = '1';
            length++;
        }
    }
    number_[length] = '\0';
    field(key, number_);
}

size_t JsonCursor::drain(char *out, size_t capacity) {
    size_t written = 0;
    while (written < capacity) {
        char c;
        if (escape_) { c = escape_; escape_ = 0; }
        else if (!text_) {
            if (!pending_) break;
            token(pending_, pending_quoted_);
            pending_ = nullptr;
            continue;
        } else if (quoted_ && !quote_phase_) { c = '"'; quote_phase_ = 1; }
        else if (offset_ < length_) {
            c = text_[offset_++];
            if (quoted_) {
                char encoded[6];
                size_t count = aircannect::json_escape_char(c, encoded, true);
                if (!count) continue;
                c = encoded[0];
                if (count == 2) escape_ = encoded[1];
            }
        } else {
            text_ = nullptr;
            if (!quoted_) continue;
            c = '"';
        }
        if (out) out[written] = c;
        written++;
    }
    return written;
}
