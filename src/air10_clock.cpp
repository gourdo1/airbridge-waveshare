#include "air10_clock.h"
#include "airsense_state.h"
#include "uart_arbiter.h"
#include "wifi_setup.h"
#include "edf_recorder.h"
#include "debug_log.h"
#include <Arduino.h>
#include <atomic>

static constexpr uint32_t DEVICE_TIME_POLL_INTERVAL_MS = 60000;
static constexpr uint32_t DEVICE_TIME_MAX_AGE_MS = 90000;
static constexpr uint16_t CLOCK_TIMEOUT_MS = 500;

static std::atomic<bool> clock_sync_pending{true};
static uint32_t clock_sync_attempt_ms = 0;
static std::atomic<bool> clock_sync_attempted{false};
static portMUX_TYPE device_time_mux = portMUX_INITIALIZER_UNLOCKED;
static char device_time[20] = "--";
static uint32_t device_time_ms = 0;

void Air10Clock::status_time(char (&out)[20]) {
    system_state_t state = Arbiter::get_state();
    portENTER_CRITICAL(&device_time_mux);
    bool fresh = (state == SYS_IDLE || state == SYS_THERAPY) &&
        uint32_t(millis() - device_time_ms) < DEVICE_TIME_MAX_AGE_MS;
    memcpy(out, device_time, sizeof(out));
    portEXIT_CRITICAL(&device_time_mux);
    if (!fresh) strcpy(out, "--");
}

static void publish_device_time(const char *text) {
    portENTER_CRITICAL(&device_time_mux);
    snprintf(device_time, sizeof(device_time), "%s", text);
    device_time_ms = millis();
    portEXIT_CRITICAL(&device_time_mux);
}

void Air10Clock::invalidate() { publish_device_time("--"); }

bool Air10Clock::read(Calendar &out, uint16_t timeout_ms, uint32_t *captured_ms) {
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        char before[9], tic[7], after[9];
        if (!Arbiter::get_var("DAC", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL,
                              before, sizeof(before), timeout_ms) ||
            !Arbiter::get_var("TIC", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL,
                              tic, sizeof(tic), timeout_ms)) return false;
        const uint32_t sampled_ms = millis();
        if (!Arbiter::get_var("DAC", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL,
                              after, sizeof(after), timeout_ms)) return false;
        if (strcmp(before, after) != 0) continue;
        if (!parse_calendar(before, tic, out)) return false;
        if (captured_ms) *captured_ms = sampled_ms;
        return true;
    }
    return false;
}

void Air10Clock::poll_status() {
    portENTER_CRITICAL(&device_time_mux);
    bool due = device_time[0] == '-' ||
        uint32_t(millis() - device_time_ms) >= DEVICE_TIME_POLL_INTERVAL_MS;
    portEXIT_CRITICAL(&device_time_mux);
    if (!due) return;

    char text[20] = "--";
    Air10Clock::Calendar t;
    if (Air10Clock::read(t, CLOCK_TIMEOUT_MS))
        snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d",
                 t.year, t.month, t.day, t.hour, t.minute);
    publish_device_time(text);
}

void Air10Clock::request_sync() {
    clock_sync_pending = true;
    clock_sync_attempted = false;
}

static bool push_time_to_resmed() {
    if (!WiFiSetup::time_synced()) return false;

    struct tm t;
    time_t now = time(nullptr);
    localtime_r(&now, &t);

    char dac_cmd[32], tic_cmd[32];
    snprintf(dac_cmd, sizeof(dac_cmd), "P S #DAC %02d%02d%04d",
             t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    snprintf(tic_cmd, sizeof(tic_cmd), "P S #TIC %02d%02d%02d",
             t.tm_hour, t.tm_min, t.tm_sec);

    char resp[64] = {};
    uint16_t resp_len = sizeof(resp);
    bool ok_dac = Arbiter::send_cmd(dac_cmd, CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, resp, &resp_len);
    if (!ok_dac) {
        // A device error is terminal until TIMESYNC; a timeout can be retried.
        if (resp[0]) clock_sync_pending = false;
        Log::logf(CAT_GENERAL, LOG_WARN, "[INIT] ResMed date %s: %s\n",
                  resp[0] ? "rejected (use TIMESYNC to retry)" : "timeout", resp);
        return false;
    }
    publish_device_time("--");
    resp[0] = '\0';
    resp_len = sizeof(resp);
    bool ok_tic = Arbiter::send_cmd(tic_cmd, CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, resp, &resp_len);
    if (!ok_tic && resp[0]) {
        clock_sync_pending = false;
        Log::logf(CAT_GENERAL, LOG_WARN, "[INIT] ResMed time rejected (use TIMESYNC to retry): %s\n", resp);
        return false;
    }

    if (ok_dac && ok_tic) {
        Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] ResMed clock set: %02d%02d%04d %02d%02d%02d\n",
                  t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
                  t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        Log::logf(CAT_GENERAL, LOG_WARN, "[INIT] ResMed clock set failed (dac=%d tic=%d)\n",
                  ok_dac, ok_tic);
    }
    return ok_dac && ok_tic;
}

bool Air10Clock::pull_time(bool force) {
    Air10Clock::Calendar t;
    if (!Air10Clock::read(t) ||
        !WiFiSetup::set_fallback_time(t.year, t.month, t.day,
                                      t.hour, t.minute, t.second, force)) return false;
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] Time from ResMed: %04d-%02d-%02d %02d:%02d:%02d\n",
              t.year, t.month, t.day, t.hour, t.minute, t.second);
    return true;
}

void Air10Clock::handle() {
    if (!clock_sync_pending || !AirSenseState::present_recently() || !WiFiSetup::time_synced()) return;
    if (!AirSenseState::device_standby()) return;
    static const char *last_blocked = nullptr;
    const char *blocked = nullptr;
    if (!EdfRecorder::clock_write_allowed(&blocked)) {
        if (blocked != last_blocked)
            Log::logf(CAT_GENERAL, LOG_INFO, "ResMed clock sync deferred: %s\n", blocked);
        last_blocked = blocked;
        return;
    }
    last_blocked = nullptr;
    if (clock_sync_attempted && millis() - clock_sync_attempt_ms < 30000) return;
    clock_sync_attempt_ms = millis();
    clock_sync_attempted = true;
    if (push_time_to_resmed()) clock_sync_pending = false;
}
