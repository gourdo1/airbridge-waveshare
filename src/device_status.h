#pragma once

#include "uart_arbiter.h"
#include "oxi_arbiter.h"
#include "oxi_ble.h"

namespace DeviceStatus {

struct Snapshot {
    system_state_t sys;
    oxi_state_t oxi;
    oxi_reading_t reading;
    int rop;
    int mhr;
    bool feeding;
};

// Copy published owner state only; presentation must not query the device.
inline Snapshot snapshot() {
    Snapshot out;
    out.sys = Arbiter::get_state();
    out.oxi = OxiBle::get_state();
    OxiArbiter::snapshot(out.reading);
    out.rop = Arbiter::get_cached_rop();
    out.mhr = Arbiter::get_cached_mhr();
    out.feeding = OxiArbiter::is_feeding();
    return out;
}

}  // namespace DeviceStatus
