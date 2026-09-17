#pragma once

#include <stdint.h>

namespace Air10Clock {

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
