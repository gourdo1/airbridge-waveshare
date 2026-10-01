#include "airsense_state.h"
#include "uart_arbiter.h"
#include "device_uptime.h"
#include "air10_clock.h"
#include "hex_util.h"
#include "custom_settings.h"
#include "live_stream.h"
#include "edf_recorder.h"
#include "export_sync.h"
#include "web_ui.h"
#include "debug_log.h"
#include <atomic>
#include <limits.h>

namespace AirSenseState {
namespace {
constexpr uint32_t HEALTH_POLL_INTERVAL_MS = 10000;
constexpr uint16_t HEALTH_TIMEOUT_MS = 500;
constexpr uint32_t MHR_POLL_INTERVAL_MS = 30UL * 60 * 1000;

uint32_t last_health_poll = 0, last_mhr_poll = 0;
uint32_t consecutive_timeouts = 0, airsense_seen_ms = 0;
bool airsense_present = false;
std::atomic<int> cached_rop{-1}, cached_mhr{-1}, cached_mop{-1};
std::atomic<bool> refresh_requested{false};
DeviceUptime::Tracker device_uptime;
Identity device_identity;
portMUX_TYPE identity_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> device_revision{0};
const char *identity_failed_tag = nullptr, *identity_failure = nullptr;

static bool read_identity_field(const char *tag, char *out, size_t capacity,
                                 uint16_t *number = nullptr) {
    Arbiter::VarReadTrace trace;
    auto result = Arbiter::read_var(tag, CMD_SRC_INTERNAL, CMD_PRIO_NORMAL,
                                    out, capacity, HEALTH_TIMEOUT_MS, &trace);
    uint32_t parsed = 0;
    if (result == Arbiter::VarResult::Ok) {
        if (!out[0] || strchr(out, '\r') || strchr(out, '\n')) {
            result = Arbiter::VarResult::Failed;
            trace.outcome = "invalid_text";
        } else if (number && !aircannect::parse_hex(out, strlen(out), parsed)) {
            result = Arbiter::VarResult::Failed;
            trace.outcome = "invalid_hex";
        } else if (number && parsed > UINT16_MAX) {
            result = Arbiter::VarResult::Failed;
            trace.outcome = "out_of_range";
        }
    }
    if (result != Arbiter::VarResult::Ok) {
        const bool changed = !identity_failed_tag || strcmp(identity_failed_tag, tag) ||
            !identity_failure || strcmp(identity_failure, trace.outcome);
        identity_failed_tag = tag;
        identity_failure = trace.outcome;
        Log::logf(CAT_HEALTH, changed ? LOG_WARN : LOG_DEBUG,
                  "Identity #%s result=%s queue=%lums sent=%u wait=%lums value=%s\n",
                  tag, trace.outcome, (unsigned long)trace.queue_ms,
                  unsigned(trace.sent), (unsigned long)trace.wait_ms, out);
        return false;
    }
    if (number) *number = static_cast<uint16_t>(parsed);
    return true;
}

static void refresh_identity() {
    Identity current;
    identity(current);
    if (!current.valid) {
        if (!read_identity_field("SRN", current.srn, sizeof(current.srn)) ||
            !read_identity_field("MID", current.mid_text, sizeof(current.mid_text), &current.mid) ||
            !read_identity_field("VID", current.vid_text, sizeof(current.vid_text), &current.vid))
            return;
        portENTER_CRITICAL(&identity_mux);
        const bool same_device = current.generation == device_identity.generation;
        if (same_device) {
            current.valid = true;
            device_identity = current;
            ++device_revision;
        }
        portEXIT_CRITICAL(&identity_mux);
        if (!same_device) {
            Log::logf(CAT_HEALTH, LOG_DEBUG, "Identity invalidated during read\n");
            return;
        }
        identity_failed_tag = identity_failure = nullptr;
    }
    if (!current.pna[0] && read_identity_field("PNA", current.pna, sizeof(current.pna))) {
        portENTER_CRITICAL(&identity_mux);
        if (current.generation == device_identity.generation) {
            memcpy(device_identity.pna, current.pna, sizeof(current.pna));
            ++device_revision;
        }
        portEXIT_CRITICAL(&identity_mux);
        identity_failed_tag = identity_failure = nullptr;
    }
}

static void poll_device_uptime() {
    char response[48] = {};
    if (!Arbiter::get_var("STK", CMD_SRC_INTERNAL, CMD_PRIO_HIGH,
                          response, sizeof(response), HEALTH_TIMEOUT_MS)) return;
    uint32_t ticks = 0;
    if (!DeviceUptime::parse(response, ticks) ||
        !device_uptime.observe(ticks, millis())) return;

    Log::logf(CAT_HEALTH, LOG_INFO, "AirSense restart detected by STK\n");
    invalidate_identity();
    Air10Clock::invalidate();
    CustomSettings::invalidate("STK reset");
    LiveStream::reattach();
    Air10Clock::request_sync();
    cached_mhr = -1;
    cached_mop = -1;
    if (Arbiter::get_state() == SYS_THERAPY) {
        Arbiter::set_state(SYS_IDLE);
    }
}

static void poll_mhr() {
    uint32_t raw;
    if (Arbiter::read_var_hex("MHR", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, raw) !=
        Arbiter::VarResult::Ok || raw > INT_MAX) {
        // UART unhappy; leave cache alone and retry next opportunity.
        return;
    }
    int new_mhr = static_cast<int>(raw);
    int prev_mhr = cached_mhr.load();
    cached_mhr = new_mhr;
    last_mhr_poll = millis();
    if (new_mhr != prev_mhr) {
        WebUI::push_status_event();
    }
}

static bool mhr_poll_due() {
    if (cached_mhr.load() < 0) return true;
    return millis() - last_mhr_poll >= MHR_POLL_INTERVAL_MS;
}

static void poll_therapy_mode() {
    uint32_t raw;
    const bool valid = Arbiter::read_var_hex("MOP", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL,
                                            raw, HEALTH_TIMEOUT_MS) == Arbiter::VarResult::Ok &&
        raw <= INT_MAX;
    cached_mop = valid ? static_cast<int>(raw) : -1;
}

static void poll_therapy_state() {
    char resp[64] = {};

    uint32_t t0 = millis();
    Log::logf(CAT_HEALTH, LOG_DEBUG, "ROP poll start t=%lu\n", t0);
    bool ok = Arbiter::get_var("ROP", CMD_SRC_INTERNAL, CMD_PRIO_HIGH,
                               resp, sizeof(resp), HEALTH_TIMEOUT_MS);
    if (ok) {
        consecutive_timeouts = 0;

        const char *rv = resp;
        airsense_present = rv && (strcmp(rv, "0000") == 0 || strcmp(rv, "0001") == 0);
        if (airsense_present) {
            airsense_seen_ms = millis();
            poll_device_uptime();
            refresh_identity();
            poll_therapy_mode();
            int new_rop = (int)strtoul(rv, nullptr, 16);
            int prev_rop = cached_rop.load();
            cached_rop = new_rop;

            system_state_t current = Arbiter::get_state();
            if (new_rop == 1 && current == SYS_IDLE) {
                Arbiter::set_state(SYS_THERAPY);
                Log::logf(CAT_HEALTH, LOG_INFO, "Therapy started\n");
                ExportSync::therapy_started();
            } else if (new_rop == 0 && current == SYS_THERAPY) {
                Arbiter::set_state(SYS_IDLE);
                Log::logf(CAT_HEALTH, LOG_INFO, "Therapy ended\n");
                EdfRecorder::request_stop();
                Air10Clock::request_sync();
                poll_mhr();
            }

            if (new_rop != prev_rop) {
                WebUI::push_status_event();
            }
            Air10Clock::poll_status();
        }
    } else {
        airsense_present = false;
        consecutive_timeouts++;
        Log::logf(CAT_HEALTH, LOG_DEBUG,
                  "ROP poll timeout (%d consecutive) t=%lu dt=%lu\n",
                  consecutive_timeouts, millis(), millis() - t0);

        if (consecutive_timeouts >= 3) {
            system_state_t current = Arbiter::get_state();
            if (current != SYS_ERROR && current != SYS_TRANSPARENT &&
                current != SYS_OTA_AIRSENSE && current != SYS_OTA_ESP) {
                invalidate_identity();
                Arbiter::set_state(SYS_ERROR);
                Log::logf(CAT_HEALTH, LOG_WARN, "AirSense unavailable: UART unresponsive\n");
            }
        }
    }
    if (!airsense_present) {
        Air10Clock::invalidate();
        cached_mop = -1;
    }
}

static void attempt_recovery() {
    if (Arbiter::get_state() != SYS_ERROR) return;

    char resp[32] = {};
    bool ok = Arbiter::get_var("BLS", CMD_SRC_INTERNAL, CMD_PRIO_HIGH,
                               resp, sizeof(resp));
    if (ok) {
        Log::logf(CAT_HEALTH, LOG_INFO, "Device responded, clearing error\n");
        consecutive_timeouts = 0;
        Arbiter::set_state(SYS_IDLE);
        CustomSettings::invalidate("UART recovery");
        // AirSense may have rebooted; force re-subscribe regardless of
        // the broker's stale subscribed flags.
        LiveStream::reattach();
    }
}

}  // namespace

void request_refresh() { refresh_requested = true; }

bool identity(Identity &out) {
    portENTER_CRITICAL(&identity_mux);
    out = device_identity;
    portEXIT_CRITICAL(&identity_mux);
    return out.valid;
}

uint32_t identity_generation() {
    portENTER_CRITICAL(&identity_mux);
    const uint32_t generation = device_identity.generation;
    portEXIT_CRITICAL(&identity_mux);
    return generation;
}

uint32_t identity_revision() { return device_revision.load(); }

void invalidate_identity() {
    portENTER_CRITICAL(&identity_mux);
    const uint32_t generation = device_identity.generation + 1;
    device_identity = {};
    device_identity.generation = generation;
    ++device_revision;
    portEXIT_CRITICAL(&identity_mux);
    request_refresh();
    EdfRecorder::request_stop();
}

int rop() { return cached_rop.load(); }
int mhr() { return cached_mhr.load(); }
int mop() { return cached_mop.load(); }

bool present_recently() {
    return airsense_present && millis() - airsense_seen_ms <= HEALTH_POLL_INTERVAL_MS;
}

bool system_idle() { return Arbiter::get_state() == SYS_IDLE; }

bool device_standby() {
    return system_idle() && rop() == 0;
}

bool local_background_allowed() {
    const auto state = Arbiter::get_state();
    return (state == SYS_IDLE || state == SYS_ERROR) && rop() != 1;
}

void poll() {
    const bool due = millis() - last_health_poll >= HEALTH_POLL_INTERVAL_MS;
    if (!due && !refresh_requested.load()) return;

    const auto state = Arbiter::get_state();
    if (state == SYS_IDLE || state == SYS_THERAPY) {
        refresh_requested.exchange(false);
        last_health_poll = millis();
        poll_therapy_state();
        LiveStream::resync();
        if (mhr_poll_due()) poll_mhr();
    } else if (due) {
        last_health_poll = millis();
        if (state == SYS_ERROR) attempt_recovery();
    }
}

}  // namespace AirSenseState
