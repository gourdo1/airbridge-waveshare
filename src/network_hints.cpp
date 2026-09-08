#include "network_hints.h"
#include "debug_log.h"
#include <Preferences.h>
#include "nvs_optional.h"

#define HINT_NVS_VERSION    1

static NetworkHint hints[HINT_MAX];
static int hint_count = 0;

static Preferences prefs;

static int find_idx_exact(const char *ssid, const uint8_t *bssid) {
    for (int i = 0; i < hint_count; i++) {
        if (strcasecmp(hints[i].ssid, ssid) == 0 &&
            memcmp(hints[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_oldest_idx() {
    int best = 0;
    for (int i = 1; i < hint_count; i++) {
        if (hints[i].last_used_ms < hints[best].last_used_ms) best = i;
    }
    return best;
}

void NetworkHints::init() {
    memset(hints, 0, sizeof(hints));
    hint_count = 0;

    if (!open_optional_preferences(prefs, "wifi_hints")) return;
    uint8_t ver = prefs.getUChar("version", 0);
    if (ver != HINT_NVS_VERSION) {
        prefs.end();
        Log::logf(CAT_WIFI, LOG_DEBUG, "[WIFI] Hints: empty (no/old NVS)\n");
        return;
    }
    int n = prefs.getUChar("count", 0);
    if (n > HINT_MAX) n = HINT_MAX;
    if (n > 0) {
        prefs.getBytes("blob", hints, n * sizeof(NetworkHint));
    }
    prefs.end();
    hint_count = n;
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Hints: loaded %d entries\n", hint_count);
}

void NetworkHints::save() {
    prefs.begin("wifi_hints", false);
    prefs.putUChar("version", HINT_NVS_VERSION);
    prefs.putUChar("count", (uint8_t)hint_count);
    if (hint_count > 0) {
        prefs.putBytes("blob", hints, hint_count * sizeof(NetworkHint));
    } else {
        prefs.remove("blob");
    }
    prefs.end();
}

NetworkHint *NetworkHints::find_best(const char *ssid) {
    NetworkHint *best = nullptr;
    for (int i = 0; i < hint_count; i++) {
        if (strcasecmp(hints[i].ssid, ssid) != 0) continue;
        if (!best || hints[i].last_used_ms > best->last_used_ms) best = &hints[i];
    }
    return best;
}

NetworkHint *NetworkHints::find_exact(const char *ssid, const uint8_t *bssid) {
    int idx = find_idx_exact(ssid, bssid);
    return idx >= 0 ? &hints[idx] : nullptr;
}

void NetworkHints::upsert(const char *ssid, const uint8_t *bssid,
                          uint8_t channel, bool pmf_disable) {
    if (!ssid || !bssid) return;

    int idx = find_idx_exact(ssid, bssid);
    if (idx < 0) {
        if (hint_count < HINT_MAX) {
            idx = hint_count++;
        } else {
            idx = find_oldest_idx();
        }
        memset(&hints[idx], 0, sizeof(hints[idx]));
        strncpy(hints[idx].ssid, ssid, sizeof(hints[idx].ssid) - 1);
        memcpy(hints[idx].bssid, bssid, 6);
    }
    hints[idx].channel = channel;
    if (pmf_disable) hints[idx].flags |= HINT_FLAG_PMF_DISABLE;
    hints[idx].last_used_ms = millis();
    save();

    Log::logf(CAT_WIFI, LOG_DEBUG,
              "[WIFI] Hint upsert: %s ch=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X flags=0x%02X\n",
              hints[idx].ssid, hints[idx].channel,
              bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
              hints[idx].flags);
}

void NetworkHints::clear_for(const char *ssid) {
    bool changed = false;
    int i = 0;
    while (i < hint_count) {
        if (strcasecmp(hints[i].ssid, ssid) == 0) {
            for (int j = i; j < hint_count - 1; j++) hints[j] = hints[j + 1];
            hint_count--;
            memset(&hints[hint_count], 0, sizeof(hints[hint_count]));
            changed = true;
        } else {
            i++;
        }
    }
    if (changed) {
        save();
        Log::logf(CAT_WIFI, LOG_DEBUG, "[WIFI] Hints cleared for '%s'\n", ssid);
    }
}

void NetworkHints::clear_all() {
    memset(hints, 0, sizeof(hints));
    hint_count = 0;
    save();
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Hints cleared\n");
}

int NetworkHints::count() { return hint_count; }

const NetworkHint *NetworkHints::at(int idx) {
    if (idx < 0 || idx >= hint_count) return nullptr;
    return &hints[idx];
}
