#pragma once

namespace AirSenseState {

// Run from the main loop; UART work remains serialized by Arbiter.
void poll();
void request_refresh();

int rop();
int mhr();
int mop();
bool present_recently();

bool system_idle();
bool device_standby();
bool local_background_allowed();

}  // namespace AirSenseState
