#pragma once

#include <stdint.h>
#include <string.h>

namespace Air10Clock {

// Published by the main health loop; never queries UART.
void status_time(char (&out)[20]);

struct Calendar {
    int year, month, day, hour, minute, second;
};

inline bool parse_calendar(const char *dac, const char *tic, Calendar &out) {
    if (!dac || !tic || strlen(dac) != 8 || strlen(tic) != 6) return false;
    int date = 0, clock = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (dac[i] < '0' || dac[i] > '9') return false;
        date = date * 10 + dac[i] - '0';
    }
    for (unsigned i = 0; i < 6; i++) {
        if (tic[i] < '0' || tic[i] > '9') return false;
        clock = clock * 10 + tic[i] - '0';
    }
    Calendar value = {date % 10000, date / 10000 % 100, date / 1000000,
                      clock / 10000, clock / 100 % 100, clock % 100};
    if (!value.year || value.month < 1 || value.month > 12 ||
        value.day < 1 || value.hour > 23 || value.minute > 59 || value.second > 59)
        return false;
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int limit = days[value.month - 1];
    if (value.month == 2 && value.year % 4 == 0 &&
        (value.year % 100 != 0 || value.year % 400 == 0)) limit++;
    if (value.day > limit) return false;
    out = value;
    return true;
}

// Civil seconds, not UTC: UDT/UTI have no timezone information.
inline bool decode(uint32_t day, uint32_t uti, int64_t &civil) {
    const uint32_t hour = uti >> 16;
    const uint32_t minute = (uti >> 8) & 0xff;
    const uint32_t second = uti & 0xff;
    if (day < 0x1000 || day >= 0xffff || hour > 23 || minute > 59 || second > 59)
        return false;
    civil = static_cast<int64_t>(day) * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

inline uint16_t therapy_day(int64_t civil) {
    return static_cast<uint16_t>((civil - 43200) / 86400);
}

inline uint32_t milliseconds_to_noon(int64_t civil) {
    const int64_t next = (int64_t(therapy_day(civil)) + 1) * 86400 + 43200;
    return static_cast<uint32_t>((next - civil) * 1000);
}

struct Anchor {
    bool native_valid = false;
    int64_t native_start = 0;
    int64_t export_start = 0;
    uint32_t captured_ms = 0;

    int64_t native_at(uint32_t now_ms) const {
        return native_start + static_cast<uint32_t>(now_ms - captured_ms) / 1000;
    }

    bool stable(int64_t native_now, uint32_t now_ms) const {
        const int64_t error = native_now - native_at(now_ms);
        return native_valid && error >= -3 && error <= 3;
    }

    bool mask_minute(uint16_t raw, uint16_t &corrected) const {
        if (raw == 0xffff) { corrected = raw; return true; }
        if (!native_valid || raw > 1440) return false;
        const int64_t delta = export_start - native_start;
        const int64_t minutes = (delta + (delta >= 0 ? 30 : -30)) / 60;
        const int64_t shifted = raw + minutes +
            (static_cast<int64_t>(therapy_day(native_start)) -
             therapy_day(export_start)) * 1440;
        // Moving part of a daily aggregate across noon requires splitting STR,
        // not clamping MaskOn/MaskOff while retaining the original statistics.
        if (shifted < 0 || shifted > 1440) return false;
        corrected = static_cast<uint16_t>(shifted);
        return true;
    }
};

}
