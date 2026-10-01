#pragma once

#include <stdint.h>

namespace OxiBlePolicy {

constexpr uint32_t ABSENCE_MS = 30000;
constexpr uint32_t NOTIFY_TIMEOUT_MS = 10000;
constexpr uint32_t INVALID_TIMEOUT_MS = 30000;
constexpr uint32_t RECONNECT_HOLDOFF_MS = 180000;
constexpr uint32_t CHARGING_HOLDOFF_MS = 15000;

struct Holdoff {
    uint32_t deadline = 0;
    uint32_t last_seen = 0;
    bool active = false;
    bool clear_when_absent = false;

    void start(uint32_t now, bool charging, bool until_absent) {
        deadline = now + (charging ? CHARGING_HOLDOFF_MS : RECONNECT_HOLDOFF_MS);
        last_seen = now;
        clear_when_absent = until_absent;
        active = true;
    }

    void update(uint32_t now, bool observing, uint32_t observing_since) {
        if (!active) return;
        bool absent = clear_when_absent && observing && now - last_seen >= ABSENCE_MS &&
                      now - observing_since >= ABSENCE_MS;
        if (absent || static_cast<int32_t>(now - deadline) >= 0) active = false;
    }
};

struct Retry {
    uint32_t deadline = 0;
    uint32_t delay = 15000;
    bool waiting = false;

    bool ready(uint32_t now) const {
        return !waiting || static_cast<int32_t>(now - deadline) >= 0;
    }

    void result(bool connected, uint32_t now) {
        if (connected) {
            *this = {};
        } else {
            deadline = now + delay;
            waiting = true;
            if (delay < 60000) delay *= 2;
        }
    }
};

enum class Disconnect { None, Silent, Invalid };

struct Samples {
    uint32_t last_received = 0;
    uint32_t invalid_since = 0;
    bool invalid = false;
    bool received_any = false;

    void start(uint32_t now) {
        *this = {};
        last_received = now;
    }

    void received(bool valid, uint32_t now) {
        received_any = true;
        last_received = now;
        if (!valid && !invalid) invalid_since = now;
        invalid = !valid;
    }

    void subscribed(uint32_t now) {
        if (!received_any) last_received = now;
    }

    Disconnect check(uint32_t now) const {
        if (now - last_received >= NOTIFY_TIMEOUT_MS) return Disconnect::Silent;
        if (invalid && now - invalid_since >= INVALID_TIMEOUT_MS) return Disconnect::Invalid;
        return Disconnect::None;
    }
};

}  // namespace OxiBlePolicy
