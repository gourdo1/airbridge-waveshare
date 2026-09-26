#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "crc.h"

namespace EdfPending {
constexpr size_t WIRE_SIZE = 64;
struct Record {
    uint16_t native_day = 0, mid = 0, vid = 0;
    char srn[24] = {}, prefix[16] = {}, day[9] = {};
};

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
    memcpy(out, "ABST", 4); out[4] = 3;
    put(out + 6, r.native_day, 2);
    put(out + 8, r.mid, 2); put(out + 10, r.vid, 2);
    memcpy(out + 12, r.srn, 24); memcpy(out + 36, r.prefix, 16);
    memcpy(out + 52, r.day, 9);
    put(out + 62, crc16_ccitt(out, 62), 2);
}
inline bool decode(const uint8_t *in, size_t length, Record &r) {
    r = {};
    if (length != WIRE_SIZE || memcmp(in, "ABST", 4) || in[4] != 3 ||
        get(in + 62, 2) != crc16_ccitt(in, 62)) return false;
    r.native_day = get(in + 6, 2);
    r.mid = get(in + 8, 2); r.vid = get(in + 10, 2);
    memcpy(r.srn, in + 12, 24); memcpy(r.prefix, in + 36, 16);
    memcpy(r.day, in + 52, 9);
    return r.srn[23] == 0 && r.srn[0] &&
           r.native_day >= 0x1000 && r.native_day < 0xffff &&
           valid_name(r.prefix, sizeof(r.prefix), true) &&
           valid_name(r.day, sizeof(r.day), false);
}
}
