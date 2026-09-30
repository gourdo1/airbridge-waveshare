#include "air10_clock.h"
#include "airsense_state.h"
#include "uart_arbiter.h"
#include "wifi_setup.h"
#include "edf_recorder.h"
#include "debug_log.h"
#include "qframe.h"
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

static bool write_clock_value(const char *command) {
    char response[64] = {};
    uint16_t length = sizeof(response);
    bool ok = Arbiter::send_cmd(command, CMD_SRC_INTERNAL, CMD_PRIO_NORMAL,
                                response, &length);
    const char *value = qframe_response_value(response);
    // The device can refuse the change and return R with its unchanged value.
    if (ok && value && strcmp(value, command + 9) == 0) return true;

    const bool rejected = !ok && response[0];
    if (rejected) clock_sync_pending = false;
    Log::logf(CAT_TIME, LOG_WARN, "ResMed clock write %s: %s -> %s\n",
              rejected ? "rejected (use TIMESYNC to retry)" : "not applied; will retry",
              command, response[0] ? response : "timeout");
    return false;
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

    if (!write_clock_value(dac_cmd)) return false;
    publish_device_time("--");
    const uint32_t write_ms = millis();
    if (!write_clock_value(tic_cmd)) return false;

    Air10Clock::Calendar observed;
    uint32_t read_ms;
    if (!Air10Clock::read(observed, CLOCK_TIMEOUT_MS, &read_ms)) {
        Log::logf(CAT_TIME, LOG_WARN, "ResMed clock verification read failed\n");
        return false;
    }

    const Air10Clock::Calendar requested = {
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec,
    };
    const int64_t advance = Air10Clock::civil_seconds(observed) -
                            Air10Clock::civil_seconds(requested);
    // RTC reads have whole-second precision; bound advancement by UART time.
    const uint32_t max_advance = uint32_t(read_ms - write_ms) / 1000 + 1;
    char text[20];
    snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d",
             observed.year, observed.month, observed.day, observed.hour, observed.minute);
    publish_device_time(text);
    if (advance < 0 || advance > max_advance) {
        Log::logf(CAT_TIME, LOG_WARN,
                  "ResMed clock verification failed; will retry: requested=%s %s actual=%s:%02d\n",
                  dac_cmd + 9, tic_cmd + 9, text, observed.second);
        return false;
    }

    Log::logf(CAT_TIME, LOG_INFO, "ResMed clock synchronized: %s:%02d\n",
              text, observed.second);
    return true;
}

bool Air10Clock::pull_time(bool force) {
    Air10Clock::Calendar t;
    if (!Air10Clock::read(t) ||
        !WiFiSetup::set_fallback_time(t.year, t.month, t.day,
                                      t.hour, t.minute, t.second, force)) return false;
    return true;
}

void Air10Clock::handle() {
    if (!clock_sync_pending || !AirSenseState::present_recently() || !WiFiSetup::time_synced()) return;
    if (!AirSenseState::device_standby()) return;
    static const char *last_blocked = nullptr;
    const char *blocked = nullptr;
    if (!EdfRecorder::clock_write_allowed(&blocked)) {
        if (blocked != last_blocked)
            Log::logf(CAT_TIME, LOG_DEBUG, "ResMed clock sync deferred: %s\n", blocked);
        last_blocked = blocked;
        return;
    }
    last_blocked = nullptr;
    if (clock_sync_attempted && millis() - clock_sync_attempt_ms < 30000) return;
    clock_sync_attempt_ms = millis();
    clock_sync_attempted = true;
    if (push_time_to_resmed()) clock_sync_pending = false;
}
