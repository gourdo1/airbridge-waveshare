#include <Arduino.h>
#include <atomic>
#include "app_config.h"
#include "build_info.h"
#include "debug_log.h"
#include "uart_arbiter.h"
#include "tcp_bridge.h"
#include "wifi_setup.h"
#include "oxi_ble.h"
#include "oxi_udp.h"
#include "oxi_arbiter.h"
#include "airbridge_ota.h"
#include "tls_memory.h"
#include "web_ui.h"
#include "qframe.h"
#include "network_hints.h"
#include "live_stream.h"
#include "live_web_consumer.h"
#include "sd_storage.h"
#include "edf_recorder.h"
#include "export_sync.h"
#include "custom_settings.h"
#include "clinical_jobs.h"
#include "airsense_state.h"
#include "air10_clock.h"
#include "board.h"
#if defined(AB_BOARD_WROOM_S3)
#include "hal/usb_serial_jtag_ll.h"

// Rev D senses VBUS through 100k/150k. Disconnect the self-powered USB
// peripheral when the host removes VBUS; power conversion is independent.
static void IRAM_ATTR usb_vbus_changed() {
    usb_serial_jtag_ll_phy_enable_pad(digitalRead(14) == HIGH);
}
#endif

extern void dispatch_command(const char *line, String &response);

#define SERIAL_LINE_MAX 256
static char serial_line[SERIAL_LINE_MAX];
static int serial_pos = 0;

static void serial_poll() {
    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\n' || c == '\r') {
            if (serial_pos > 0) {
                serial_line[serial_pos] = '\0';

                if (serial_line[0] == '$') {
                    // Internal command
                    String response;
                    dispatch_command(serial_line + 1, response);
                    if (response.length() > 0) Serial.print(response);
                } else {
                    // Q-frame
                    char resp_buf[512] = {};
                    uint16_t resp_len = sizeof(resp_buf);
                    bool ok = Arbiter::send_cmd(serial_line, CMD_SRC_INTERNAL,
                                                CMD_PRIO_NORMAL, resp_buf,
                                                &resp_len, 2000);
                    if (ok) {
                        Serial.println(resp_buf);
                    } else {
                        Serial.println("ERR:TIMEOUT");
                    }
                }
                serial_pos = 0;
            }
        } else if (serial_pos < SERIAL_LINE_MAX - 1) {
            serial_line[serial_pos++] = c;
        }
    }
}

// Health monitoring:
//   ROP/STK every 10s to detect therapy state and device restarts
//   MOP on the same pass for the published therapy mode
//   DAC/TIC every minute for the status clock
//   MHR every 30 min and once on therapy stop
#define HEALTH_POLL_INTERVAL_MS     10000
#define HEALTH_TIMEOUT_MS           500
#define DEVICE_TIME_POLL_INTERVAL_MS 60000
#define DEVICE_TIME_MAX_AGE_MS       (DEVICE_TIME_POLL_INTERVAL_MS + 3 * HEALTH_POLL_INTERVAL_MS)

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
    if (Air10Clock::read(t, HEALTH_TIMEOUT_MS))
        snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d",
                 t.year, t.month, t.day, t.hour, t.minute);
    publish_device_time(text);
}

bool pull_time_from_resmed(bool force = false);

void setup() {
    Serial.begin(115200);
#if defined(AB_BOARD_WROOM_S3)
    pinMode(14, INPUT);
    attachInterrupt(digitalPinToInterrupt(14), usb_vbus_changed, CHANGE);
    usb_vbus_changed();
#endif
    delay(500);
    while (Serial.available()) Serial.read();  // flush boot garbage
    Log::init();

    if (!aircannect::TlsMemory::begin())
        Log::logf(CAT_GENERAL, LOG_ERROR, "[INIT] TLS allocator installation failed\n");

    Log::boot();
    Log::printf("Chip: %s, Heap: %d bytes\n", ESP.getChipModel(), ESP.getFreeHeap());

    Config::init();
    // NetworkHints must come up before Config::load runs the wnet migration,
    // because that step calls NetworkHints::upsert with legacy hint values.
    NetworkHints::init();
    Config::load();
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] Config loaded\n");

    SdStorage::init();
    CustomSettings::init();

    Arbiter::init(Serial1, PIN_AS10_RX, PIN_AS10_TX);
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] UART arbiter started\n");

    bool wifi_ok = WiFiSetup::init();

    TcpBridge::init();
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] TCP bridge started\n");

    if (wifi_ok) {
        auto &cfg = Config::get();
        if (cfg.debug_port > 0 && cfg.debug_port != cfg.tcp_port)
            TcpBridge::init_debug_server(cfg.debug_port);

        if (cfg.http_port > 0 && cfg.http_port != cfg.tcp_port)
            WebUI::init(cfg.http_port);

        OtaManager::init();
    }

    // If NTP didn't sync, fall back to resmed device clock
    if (!WiFiSetup::time_synced()) pull_time_from_resmed();

    OxiArbiter::init();
    OxiBle::init();
    OxiUdp::init();
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] BLE oximetry started\n");

    LiveStream::init();
    LiveWebConsumer::init();
    EdfRecorder::init();

    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] All systems go\n");
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

bool pull_time_from_resmed(bool force) {
    Air10Clock::Calendar t;
    if (!Air10Clock::read(t) ||
        !WiFiSetup::set_fallback_time(t.year, t.month, t.day,
                                      t.hour, t.minute, t.second, force)) return false;
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] Time from ResMed: %04d-%02d-%02d %02d:%02d:%02d\n",
              t.year, t.month, t.day, t.hour, t.minute, t.second);
    return true;
}

static void sync_resmed_clock() {
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

void loop() {
    serial_poll();
    ClinicalJobs::tick();

#if AB_STORAGE_HAS_SDCARD
    static uint32_t recorder_retry_ms = 0;
    if (millis() - recorder_retry_ms >= 5000) {
        recorder_retry_ms = millis();
        EdfRecorder::init();
    }
#endif

    OtaManager::handle();
    WiFiSetup::check();
    Log::poll();
    TcpBridge::poll_debug_clients();
    WebUI::handle();

    // Suspend WiFi scanning during therapy/streaming/oximetry/OTA
    system_state_t sys_st = Arbiter::get_state();
    bool oxi_active = OxiArbiter::is_feeding();
    bool ota_active = (sys_st == SYS_OTA_AIRSENSE || sys_st == SYS_OTA_ESP);

    if (sys_st == SYS_THERAPY || sys_st == SYS_TRANSPARENT ||
        ota_active || oxi_active) {
        WiFiSetup::suspend_roaming();
    } else {
        WiFiSetup::resume_roaming();
    }

    static bool prev_ota_active = false;
    if (ota_active) {
        OxiBle::suspend();
        if (!prev_ota_active && sys_st == SYS_OTA_ESP) LiveStream::suspend();
    } else {
        OxiBle::resume();
        if (prev_ota_active) LiveStream::resume();
    }
    prev_ota_active = ota_active;

    sync_resmed_clock();

    LiveWebConsumer::tick();

    OxiArbiter::poll();

    AirSenseState::poll();

    delay(10);
}
