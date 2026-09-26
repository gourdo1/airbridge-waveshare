#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "crc.h"
#include "air10_stored.h"

namespace EdfPending {
constexpr size_t WIRE_SIZE = 92;
constexpr uint8_t END_KNOWN = 1, SEGMENT_END = 2;
struct Record {
    uint8_t flags = 0;
    uint16_t native_day = 0, mid = 0, vid = 0;
    int64_t native_start = 0, native_end = 0;
    int64_t mask_start = 0;
    char srn[24] = {}, prefix[16] = {}, day[9] = {};
};

inline bool matches_interval(const Record &r, const Air10Stored::Value &on,
                             const Air10Stored::Value &off, uint16_t tolerance) {
    if (!on.present || !off.present) return false;
    const int64_t noon = int64_t(r.native_day) * 86400 + 43200;
    const int64_t start = (r.mask_start - noon) / 60;
    const int64_t end = (r.native_end - noon) / 60;
    if (start < 0 || start >= 1440) return false;
    if ((r.flags & END_KNOWN) && (end < start || end > 1440)) return false;
    for (uint8_t i = 0; i < on.sample_count && i < off.sample_count &&
                        i < Air10Stored::MAX_SAMPLES; i++) {
        const int16_t a = on.samples[i], b = off.samples[i];
        if (a < 0 || b < a || b > 1440 || a > start + tolerance || b < start) continue;
        if (!(r.flags & END_KNOWN)) return true;
        if (b + tolerance < end) continue;
        if ((r.flags & SEGMENT_END) || b <= end + tolerance) return true;
    }
    return false;
}
inline void put(uint8_t *out, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; i++) out[i] = value >> (8 * i);
}
inline uint64_t get(const uint8_t *in, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; i++) value |= uint64_t(in[i]) << (8 * i);
    return value;
}
inline bool valid_name(const char *text, size_t size, bool prefix) {
    if (text[size - 1]) return false;
    for (size_t i = 0; i + 1 < size; i++) {
        if (prefix && i == 8) { if (text[i] != '_') return false; }
        else if (text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}
inline void encode(const Record &r, uint8_t out[WIRE_SIZE]) {
    memset(out, 0, WIRE_SIZE);
    memcpy(out, "ABST", 4); out[4] = 2; out[5] = r.flags;
    put(out + 6, r.native_day, 2);
    put(out + 8, r.mid, 2); put(out + 10, r.vid, 2);
    put(out + 16, r.native_start, 8); put(out + 24, r.native_end, 8);
    put(out + 32, r.mask_start, 8);
    memcpy(out + 40, r.srn, 24); memcpy(out + 64, r.prefix, 16);
    memcpy(out + 80, r.day, 9);
    put(out + 90, crc16_ccitt(out, 90), 2);
}
inline bool decode(const uint8_t *in, size_t length, Record &r) {
    r = {};
    if (length != WIRE_SIZE || memcmp(in, "ABST", 4) || in[4] != 2 ||
        get(in + 90, 2) != crc16_ccitt(in, 90)) return false;
    r.flags = in[5]; r.native_day = get(in + 6, 2);
    r.mid = get(in + 8, 2); r.vid = get(in + 10, 2);
    r.native_start = get(in + 16, 8); r.native_end = get(in + 24, 8);
    memcpy(r.srn, in + 40, 24); memcpy(r.prefix, in + 64, 16);
    memcpy(r.day, in + 80, 9);
    r.mask_start = get(in + 32, 8);
    return !(r.flags & ~3u) && r.srn[23] == 0 && r.srn[0] &&
           r.native_day >= 0x1000 && r.native_day < 0xffff &&
           r.native_start >= int64_t(r.native_day) * 86400 + 43200 &&
           r.native_start < int64_t(r.native_day + 1) * 86400 + 43200 &&
           r.mask_start >= int64_t(r.native_day) * 86400 + 43200 &&
           r.mask_start <= r.native_start &&
           valid_name(r.prefix, sizeof(r.prefix), true) &&
           valid_name(r.day, sizeof(r.day), false);
}
}
