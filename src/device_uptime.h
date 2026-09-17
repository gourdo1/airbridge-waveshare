#pragma once

#include <stdint.h>

namespace DeviceUptime {

inline bool parse(const char *text, uint32_t &ticks) {
    if (!text || !*text) return false;
    uint32_t value = 0;
    unsigned length = 0;
    for (; *text; text++) {
        int digit = *text >= '0' && *text <= '9' ? *text - '0'
                  : *text >= 'A' && *text <= 'F' ? *text - 'A' + 10
                  : *text >= 'a' && *text <= 'f' ? *text - 'a' + 10 : -1;
        if (digit < 0 || ++length > 8) return false;
        value = (value << 4) | static_cast<unsigned>(digit);
    }
    ticks = value;
    return true;
}

class Tracker {
    bool initialized = false;
    uint32_t previous_ticks = 0;
    uint32_t previous_ms = 0;

public:
    bool observe(uint32_t ticks, uint32_t now_ms) {
        const uint32_t elapsed_ticks = (now_ms - previous_ms) / 10;
        const uint32_t advance = ticks - previous_ticks;
        // STK ticks every 10 ms. Allow UART latency and independent clocks;
        // unsigned subtraction preserves ordinary STK and millis() wraps.
        const uint32_t tolerance = 200 + elapsed_ticks / 100;
        const bool restarted = initialized &&
            (advance > elapsed_ticks + tolerance ||
             (elapsed_ticks > tolerance && advance < elapsed_ticks - tolerance));
        initialized = true;
        previous_ticks = ticks;
        previous_ms = now_ms;
        return restarted;
    }
};

}
