#pragma once
#include <Arduino.h>

typedef enum {
    OXI_DISABLED,
    OXI_SCANNING,
    OXI_CONNECTING,
    OXI_BONDING,
    OXI_STREAMING,
    OXI_DISCONNECTED,
    OXI_OBSERVING,
} oxi_state_t;

inline const char *oxi_state_name(oxi_state_t s) {
    static const char *names[] = {
        "DISABLED","SCANNING","CONNECTING","BONDING","STREAMING","DISCONNECTED","OBSERVING"
    };
    return (s < sizeof(names)/sizeof(names[0])) ? names[s] : "?";
}

typedef struct {
    int8_t      spo2;
    int16_t     pulse_bpm;
    bool        valid;
    uint32_t    timestamp_ms;
} oxi_reading_t;

#define MAX_SCAN_RESULTS 8

// Four new devices; retain the legacy union of three bonds and three knowns.
constexpr int MAX_KNOWN_DEVICES = 6;
constexpr int KNOWN_DEVICE_LIMIT = 4;

struct oxi_known_device_t {
    char addr[18] = {};
    char name[32] = {};
    bool autoconnect = true;
};

struct oxi_scan_result_t {
    char addr[18];
    String name;
    int rssi;
    uint8_t addr_type;
};

namespace OxiBle {
    void init();

    void start_scan();
    void stop_scan();
    void connect(const char *addr = nullptr);
    void disconnect();
    void enable();
    void disable();

    void suspend();
    void resume();

    // OTA worker only. Separate from ordinary scan/connection suspension.
    bool release_memory(uint32_t timeout_ms);
    bool restore_memory(uint32_t timeout_ms);

    oxi_state_t get_state();
    uint32_t revision();  // Completed scans and known-device changes, not link state.

    void task(void *param);

    int get_scan_results(oxi_scan_result_t *out, int max);

    int get_known_devices(oxi_known_device_t *out, int max);
    bool set_autoconnect(const char *addr, bool enabled);

    // Async: enqueue a deletion request. Actual stop-scan / disconnect /
    // unpair happens in the BLE task. Safe to call from any context.
    void request_remove_known(const char *addr);
    void request_clear_all_known();
}
