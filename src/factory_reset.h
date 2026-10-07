#pragma once

namespace FactoryReset {

// Schedules only; marker persistence runs in OtaManager's main-loop reboot.
bool request(const char **error = nullptr);

// Before Config/network/SD initialization. Consumes the marker before any
// destructive action; any full NVS erase attempt restarts without loading config.
// False means pending intent is unreadable or cannot be consumed: boot must halt.
bool run_pending_on_boot();

}  // namespace FactoryReset
