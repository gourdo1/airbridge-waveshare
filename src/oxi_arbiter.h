#pragma once
#include <Arduino.h>
#include "oxi_ble.h"  // oxi_state_t, oxi_reading_t

typedef enum {
    OXI_SRC_NONE,
    OXI_SRC_BLE,
    OXI_SRC_UDP,
} oxi_source_t;

// IEEE 11073 SFLOAT to integer - SpO2/HR are always int
// Returns -1 for NaN/NRes/reserved
int16_t parse_sfloat(uint16_t raw);

namespace OxiArbiter {
    void init();

    void feed(oxi_source_t src, int8_t spo2, int16_t pulse_bpm, bool valid);
    // The claiming transport publishes its address and display name together.
    void set_source(const char *id, const char *name);
    void get_source(char *id, size_t id_size, char *name, size_t name_size);

    void start_feed();
    // Transport cleanup stops only its own source; NONE is an explicit stop.
    void stop_feed(oxi_source_t source = OXI_SRC_NONE);
    bool is_feeding();

    void snapshot(oxi_reading_t &reading, oxi_source_t *source = nullptr);
    // Latest accepted reading no later than time_ms, including invalid updates.
    // False when that point has no predecessor in the bounded history.
    bool snapshot_at(uint32_t time_ms, oxi_reading_t &reading);
    oxi_source_t active_source();

    void poll();
}
