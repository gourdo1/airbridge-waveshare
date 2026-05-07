#pragma once
#include <Arduino.h>

#define HINT_MAX                8
#define HINT_FLAG_PMF_DISABLE   0x01

struct NetworkHint {
    char        ssid[33];
    uint8_t     bssid[6];
    uint8_t     channel;
    uint8_t     flags;
    uint32_t    last_used_ms;
};

namespace NetworkHints {
    void init();
    void save();

    // Most recently used hint matching ssid, or NULL.
    NetworkHint *find_best(const char *ssid);

    // Exact (ssid, bssid) match, or NULL.
    NetworkHint *find_exact(const char *ssid, const uint8_t *bssid);

    // Insert or refresh. Updates last_used_ms. LRU evict if full.
    void upsert(const char *ssid, const uint8_t *bssid, uint8_t channel,
                bool pmf_disable);

    // Drop all hints for a given ssid (called when slot is removed).
    void clear_for(const char *ssid);

    void clear_all();

    int count();
    const NetworkHint *at(int idx);
}
