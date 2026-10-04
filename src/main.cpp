#include <Arduino.h>
#include "app_config.h"
#include "build_info.h"
#include "debug_log.h"
#include "crash_diagnostics.h"
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
#include "air10_stream.h"
#include "live_tce.h"
#include "live_web_consumer.h"
#include "sd_storage.h"
#include "edf_recorder.h"
#include "export_sync.h"
#include "custom_settings.h"
#include "clinical_jobs.h"
#include "airsense_state.h"
#include "air10_clock.h"
#include "board.h"
#include "status_screen.h"
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
                    if (response.length() > 0) Log::write_crlf(Serial, response);
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
        } else if (c == '\b' || c == '\x7f') {
            if (serial_pos > 0) --serial_pos;
        } else if (serial_pos < SERIAL_LINE_MAX - 1) {
            serial_line[serial_pos++] = c;
        }
    }
}

void setup() {
    Arbiter::prepare_tx(PIN_AS10_TX);

#ifdef AB_LCD_BACKLIGHT_GPIO
    // Keep the unused local display dark, including boards with a BL pull-up.
    pinMode(AB_LCD_BACKLIGHT_GPIO, OUTPUT);
    digitalWrite(AB_LCD_BACKLIGHT_GPIO, LOW);
#endif
#if AB_POWER_HOLD_GPIO >= 0
    pinMode(AB_POWER_HOLD_GPIO, OUTPUT);      // keep battery path latched on
    digitalWrite(AB_POWER_HOLD_GPIO, HIGH);
#endif
#if AB_LCD_BL_GPIO >= 0
    pinMode(AB_LCD_BL_GPIO, OUTPUT);          // keep panel dark until it is initialized
    digitalWrite(AB_LCD_BL_GPIO, LOW);
#endif
#if AB_TOUCH_RST_GPIO >= 0
    pinMode(AB_TOUCH_RST_GPIO, OUTPUT);       // hold touch controller in reset,
    digitalWrite(AB_TOUCH_RST_GPIO, LOW);     // off the shared I2C bus
#endif

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
    CrashDiagnostics::init();
    Log::logf(CAT_GENERAL, LOG_DEBUG, "Chip: %s, Heap: %d bytes\n", ESP.getChipModel(), ESP.getFreeHeap());

    Config::init();
    // NetworkHints must come up before Config::load runs the wnet migration,
    // because that step calls NetworkHints::upsert with legacy hint values.
    NetworkHints::init();
    Config::load();
    Log::logf(CAT_CONFIG, LOG_DEBUG, "Configuration loaded\n");

    SdStorage::init();
    CustomSettings::init();

    Arbiter::init(Serial1, PIN_AS10_RX, PIN_AS10_TX);
    Log::logf(CAT_ARB, LOG_DEBUG, "Initialization returned\n");
    LiveStream::init();
    LiveTce::register_parser();

    // Before WiFi, which can block for a minute during SmartConfig.
    StatusScreen::init();

    bool wifi_ok = WiFiSetup::init();

    TcpBridge::init();

    if (wifi_ok) {
        auto &cfg = Config::get();
        if (cfg.debug_port > 0 && cfg.debug_port != cfg.tcp_port)
            TcpBridge::init_debug_server(cfg.debug_port);

        if (cfg.http_port > 0 && cfg.http_port != cfg.tcp_port)
            WebUI::init(cfg.http_port);

        OtaManager::init();
    }

    // If NTP didn't sync, fall back to resmed device clock
    if (!WiFiSetup::time_synced()) Air10Clock::pull_time();

    OxiArbiter::init();
    OxiBle::init();
    OxiUdp::init();

    EdfRecorder::init();

    Log::logf(CAT_GENERAL, LOG_DEBUG, "Startup initialization complete\n");
}

// Ask Air10Clock to re-sync the AirSense clock after each NTP refresh (the
// first sync after boot is covered by its own boot-time request) and when the
// local UTC offset changes (daylight saving transition or a new tz setting).
// Air10Clock applies its usual policy: standby only, resmed_time setting.
#define TIME_OFFSET_CHECK_MS 60000

// Local time minus UTC, in seconds (includes DST).
static long utc_offset_seconds(time_t now) {
    struct tm lt, gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
    int day = lt.tm_yday - gt.tm_yday;
    if (day > 1) day = -1;          // year boundary: local is Dec 31, UTC is Jan 1
    else if (day < -1) day = 1;     // year boundary: local is Jan 1, UTC is Dec 31
    return day * 86400L + (lt.tm_hour - gt.tm_hour) * 3600L +
           (lt.tm_min - gt.tm_min) * 60L;
}

static void check_time_triggers() {
    static uint32_t seen_ntp_syncs = 0;
    static bool     utc_offset_known = false;
    static long     last_utc_offset = 0;
    static uint32_t last_offset_check_ms = 0;

    uint32_t syncs = WiFiSetup::ntp_sync_count();
    if (syncs != seen_ntp_syncs) {
        bool first = (seen_ntp_syncs == 0);
        seen_ntp_syncs = syncs;
        if (!first) {
            Log::logf(CAT_TIME, LOG_DEBUG, "ResMed clock resync requested: NTP resync\n");
            Air10Clock::request_sync();
        }
    }

    if (utc_offset_known && millis() - last_offset_check_ms < TIME_OFFSET_CHECK_MS) return;
    last_offset_check_ms = millis();
    time_t now = time(nullptr);
    if (now < 1700000000) return;   // no valid time yet
    long offset = utc_offset_seconds(now);
    if (utc_offset_known && offset != last_utc_offset) {
        Log::logf(CAT_TIME, LOG_INFO,
                  "ResMed clock resync requested: UTC offset %+ld -> %+ld min\n",
                  last_utc_offset / 60, offset / 60);
        Air10Clock::request_sync();
    }
    last_utc_offset = offset;
    utc_offset_known = true;
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

    check_time_triggers();
    Air10Clock::handle();

    LiveWebConsumer::tick();
    StatusScreen::tick();

    OxiArbiter::poll();

    AirSenseState::poll();
    Air10Stream::poll();

    delay(10);
}
