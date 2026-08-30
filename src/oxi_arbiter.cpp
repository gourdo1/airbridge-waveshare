#include "oxi_arbiter.h"
#include "uart_arbiter.h"
#include "qframe.h"
#include "app_config.h"
#include "debug_log.h"
#include <stdint.h>

// integer pow10 for exponent magnitudes 0-7
static const int32_t pow10_int[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000};
#define POW10_MAX ((int)(sizeof(pow10_int)/sizeof(pow10_int[0])))  // 8

int16_t parse_sfloat(uint16_t raw) {
    if (raw == 0x07FF || raw == 0x0800 || raw == 0x07FE ||
        raw == 0x0801 || raw == 0x0802) return -1;

    // 12-bit two's-complement mantissa, sign-extended into int32
    int32_t mantissa = raw & 0x0FFF;
    if (mantissa & 0x0800) mantissa |= (int32_t)0xFFFFF000;

    // 4-bit two's-complement exponent, range -8..7
    int8_t exp = (int8_t)((raw >> 12) & 0x0F);
    if (exp & 0x08) exp |= 0xF0;

    int32_t value;
    if (exp == 0) {
        value = mantissa;
    } else if (exp > 0) {
        if (exp >= POW10_MAX) return -1;
        value = mantissa * pow10_int[exp];
    } else {
        int abs_exp = -exp;  // 1..8
        if (abs_exp >= POW10_MAX) return -1;
        value = mantissa / pow10_int[abs_exp];
    }

    if (value < INT16_MIN || value > INT16_MAX) return -1;
    return (int16_t)value;
}

static oxi_reading_t reading = { -1, -1, false, 0 };
static volatile bool feeding = false;
static oxi_source_t src_active = OXI_SRC_NONE;
static uint32_t src_last_time = 0;
static char source_id[32] = "";
static portMUX_TYPE reading_mux = portMUX_INITIALIZER_UNLOCKED;

#define SOURCE_TIMEOUT_MS 10000

static uint8_t oxh_seq = 0;
static uint8_t oxh_toggle = 0;
static bool last_valid = false;
static uint32_t last_inject = 0;

static const char *src_name(oxi_source_t s) {
    switch (s) {
        case OXI_SRC_BLE: return "BLE";
        case OXI_SRC_UDP: return "UDP";
        default: return "NONE";
    }
}

static void inject_lframe() {
    system_state_t st = Arbiter::get_state();
    if (st == SYS_TRANSPARENT || st == SYS_OTA_AIRSENSE || st == SYS_OTA_ESP) return;

    auto &cfg = Config::get();
    if (cfg.oxi_feed_therapy_only && st != SYS_THERAPY) return;
    oxi_reading_t current;
    oxi_source_t current_source;
    OxiArbiter::snapshot(current, &current_source);
    if (!cfg.oxi_lframe_continuous && !current.valid) return;

    if (current.valid != last_valid) {
        if (current.valid)
            Log::logf(CAT_OXI, LOG_INFO, "[OXI] Finger detected: SpO2=%d%% HR=%d bpm (%s)\n",
                      current.spo2, current.pulse_bpm, src_name(current_source));
        else
            Log::logf(CAT_OXI, LOG_INFO, "[OXI] Finger lost\n");
        last_valid = current.valid;
    }

    uint8_t toggle = oxh_toggle ? 0x02 : 0x00;
    oxh_toggle ^= 1;

    uint8_t oxs, sas;
    uint16_t hrr;
    uint8_t sar;

    if (current.valid) {
        oxs = 0x81 | toggle;
        sas = 0x80 | toggle;
        hrr = (uint16_t)current.pulse_bpm;
        sar = (uint8_t)current.spo2;
    } else {
        oxs = 0x99 | toggle;
        sas = 0x98 | toggle;
        hrr = 0x1FF;
        sar = 0x7F;
    }

    char payload[17];
    snprintf(payload, sizeof(payload), "OXH%02X%02X%03X%02X%02X10",
             oxh_seq, oxs, hrr, sas, sar);
    oxh_seq = (oxh_seq + 1) & 0xFF;

    Log::logf(CAT_OXI, LOG_DEBUG, "[OXI] L-frame seq=%02X %s SpO2=%d HR=%d t=%lu\n",
              (oxh_seq - 1) & 0xFF, current.valid ? "valid" : "no-finger",
              current.spo2, current.pulse_bpm, millis());

    uint8_t frame_buf[32];
    int frame_len = qframe_build('L', (const uint8_t *)payload, 16,
                                 frame_buf, sizeof(frame_buf));
    if (frame_len < 0) return;

    Arbiter::send_frame(frame_buf, (uint16_t)frame_len, CMD_SRC_OXI, CMD_PRIO_LOW);
}

void OxiArbiter::init() {
    reading = { -1, -1, false, 0 };
    feeding = false;
    src_active = OXI_SRC_NONE;
    oxh_seq = 0;
    oxh_toggle = 0;
    last_valid = false;
    last_inject = 0;
}

void OxiArbiter::feed(oxi_source_t src, int8_t spo2, int16_t pulse_bpm, bool valid) {
    bool source_claimed = false;
    portENTER_CRITICAL(&reading_mux);
    if (src_active != OXI_SRC_NONE && src_active != src) {
        portEXIT_CRITICAL(&reading_mux);
        return;
    }

    // Only claim source on valid data
    if (src_active == OXI_SRC_NONE && valid) {
        source_claimed = true;
    }
    if (!valid && src_active == OXI_SRC_NONE) {
        portEXIT_CRITICAL(&reading_mux);
        return;
    }

    src_active = src;
    src_last_time = millis();
    reading.spo2 = spo2;
    reading.pulse_bpm = pulse_bpm;
    reading.valid = valid;
    reading.timestamp_ms = millis();
    portEXIT_CRITICAL(&reading_mux);

    if (source_claimed) {
        Log::logf(CAT_OXI, LOG_INFO, "[OXI] Source active: %s\n", src_name(src));
        Arbiter::lcd_message("Oximeter Connected", 15000);
        if (Config::get().oxi_auto_start && !feeding) start_feed();
    }
}

void OxiArbiter::start_feed() {
    feeding = true;
    Log::logf(CAT_OXI, LOG_INFO, "[OXI] Feeding started\n");
}

void OxiArbiter::stop_feed() {
    feeding = false;
    Log::logf(CAT_OXI, LOG_INFO, "[OXI] Feeding stopped\n");
}

bool OxiArbiter::is_feeding() { return feeding; }

void OxiArbiter::snapshot(oxi_reading_t &out, oxi_source_t *source) {
    portENTER_CRITICAL(&reading_mux);
    out = reading;
    if (source) *source = src_active;
    portEXIT_CRITICAL(&reading_mux);
}

oxi_source_t OxiArbiter::active_source() {
    oxi_source_t source;
    portENTER_CRITICAL(&reading_mux);
    source = src_active;
    portEXIT_CRITICAL(&reading_mux);
    return source;
}

void OxiArbiter::set_source_id(const char *id) {
    strncpy(source_id, id ? id : "", sizeof(source_id) - 1);
    source_id[sizeof(source_id) - 1] = '\0';
}

const char *OxiArbiter::get_source_id() { return source_id; }

void OxiArbiter::poll() {
    // release source after timeout
    oxi_source_t timed_out_source = OXI_SRC_NONE;
    portENTER_CRITICAL(&reading_mux);
    if (src_active != OXI_SRC_NONE && millis() - src_last_time > SOURCE_TIMEOUT_MS) {
        timed_out_source = src_active;
        src_active = OXI_SRC_NONE;
        reading.valid = false;
    }
    portEXIT_CRITICAL(&reading_mux);
    if (timed_out_source != OXI_SRC_NONE) {
        Log::logf(CAT_OXI, LOG_INFO, "[OXI] Source %s timed out\n",
                  src_name(timed_out_source));
        source_id[0] = '\0';
        if (feeding) stop_feed();
    }

    // inject at configured interval
    auto &cfg = Config::get();
    if (feeding && active_source() != OXI_SRC_NONE &&
        millis() - last_inject >= cfg.oxi_interval_ms) {
        last_inject = millis();
        inject_lframe();
    }
}
