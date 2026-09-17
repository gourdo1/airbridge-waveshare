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
    void set_source_id(const char *id);
    void get_source_id(char *out, size_t size);

    void start_feed();
    // Transport cleanup stops only its own source; NONE is an explicit stop.
    void stop_feed(oxi_source_t source = OXI_SRC_NONE);
    bool is_feeding();

    void snapshot(oxi_reading_t &reading, oxi_source_t *source = nullptr);
    oxi_source_t active_source();

    void poll();
}
