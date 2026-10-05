#include "status_screen.h"

#if AB_LCD_ENABLED

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include "uart_arbiter.h"
#include "oxi_arbiter.h"
#include "live_stream.h"
#include "live_tce.h"
#include "airsense_state.h"
#include "debug_log.h"
#include <esp32-hal-periman.h>
#include <driver/gpio.h>



namespace StatusScreen {

#define SCR_W               240
#define SCR_H               240
#define SCR_MARGIN          4
#define REFRESH_MS          1000
#define BL_FREQ_HZ          5000
#define BL_RES_BITS         10
#define BL_FULL             ((1u << BL_RES_BITS) - 1)
#define PMD_STALE_MS        3000    // no PMD sample for this long -> show "--"
#define PMD_RETRY_MS        5000    // throttle failed subscribe attempts
#define BTN_STUCK_MS        5000    // held longer than this: treat as stuck, ignore
#define BL_REFRESH_MS       1000    // re-check the backlight hardware this often
#define THERAPY_POLL_MS     3000    // read therapy pressure (MKI/MKE) this often
#define THERAPY_STALE_MS    10000   // no successful read for this long -> show "--"
#define THERAPY_READ_MS     400     // per-read response timeout

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *gfx = nullptr;

static const int btn_pins[] = { AB_BTN1_GPIO, AB_BTN2_GPIO, AB_BTN3_GPIO };
#define BTN_COUNT (sizeof(btn_pins) / sizeof(btn_pins[0]))
static uint32_t btn_down_ms[BTN_COUNT] = {};   // 0 = released
static bool     btn_stuck[BTN_COUNT] = {};

static uint32_t wake_until = 0;
static bool     woke_once = false;
static int      bl_duty = -1;
static uint32_t bl_check_ms = 0;
static uint32_t last_draw_ms = 0;

// ---- Mask pressure from the PMD live stream (subscribed during therapy) ----

static LiveStream::consumer_handle_t pmd_handle = -1;
static uint32_t pmd_retry_ms = 0;
static portMUX_TYPE pmd_mux = portMUX_INITIALIZER_UNLOCKED;
static int32_t  pmd_sum = 0;
static uint16_t pmd_count = 0;
static uint32_t pmd_last_ms = 0;
static float    pressure_cmh2o = 0;

// Runs on the UART rx task: keep it tiny.
static void on_pmd(const void *sample, uint16_t sample_size, void *ctx) {
    (void)ctx;
    if (sample_size < sizeof(LiveTce::Sample)) return;
    const LiveTce::Sample *s = (const LiveTce::Sample *)sample;
    portENTER_CRITICAL(&pmd_mux);
    pmd_sum += s->mkp;
    pmd_count++;
    pmd_last_ms = millis();
    portEXIT_CRITICAL(&pmd_mux);
}

static void update_pmd_subscription(bool want) {
    if (want && pmd_handle < 0) {
        if (pmd_retry_ms && millis() - pmd_retry_ms < PMD_RETRY_MS) return;
        pmd_handle = LiveStream::subscribe(LiveTce::TAG, on_pmd, nullptr);
        if (pmd_handle < 0) {
            pmd_retry_ms = millis();
            if (pmd_retry_ms == 0) pmd_retry_ms = 1;
            Log::logf(CAT_GENERAL, LOG_WARN, "[LCD] pressure stream (TCE) subscribe failed\n");
        } else {
            pmd_retry_ms = 0;
        }
    } else if (!want && pmd_handle >= 0) {
        LiveStream::unsubscribe(pmd_handle);
        pmd_handle = -1;
    }
}

// Average of the samples since the last call (~25 per second), so the
// displayed value doesn't jump with every breath. Returns false if stale.
static bool take_pressure(float *out) {
    portENTER_CRITICAL(&pmd_mux);
    int32_t  sum = pmd_sum;
    uint16_t n = pmd_count;
    uint32_t last = pmd_last_ms;
    pmd_sum = 0;
    pmd_count = 0;
    portEXIT_CRITICAL(&pmd_mux);

    if (n > 0) pressure_cmh2o = (float)sum / n / 50.0f;   // MKP / 50 = cmH2O
    if (pmd_handle < 0 || last == 0 || millis() - last > PMD_STALE_MS) return false;
    *out = pressure_cmh2o;
    return true;
}

// ---- Therapy pressure (MKI) and exhale pressure (MKE), polled in therapy ----
//
// These are the AirSense's own pressure values (OSCAR: Pressure and EPR
// Pressure), unlike the measured mask pressure above. Plain request/reply
// reads, kept outside the SD recorder's acquisition windows, never blocking.

static uart_transaction_t *therapy_ticket = nullptr;
static uint8_t  therapy_step = 0;           // 0 = MKI, 1 = MKE
static uint32_t therapy_poll_ms = 0;
static int32_t  therapy_raw[2] = {-1, -1};  // raw /50 = cmH2O
static uint32_t therapy_ok_ms[2] = {0, 0};

static void cancel_therapy_read() {
    if (therapy_ticket) Arbiter::cancel_transaction(therapy_ticket);
    therapy_ticket = nullptr;
    therapy_step = 0;
}

static void poll_therapy_pressure(bool therapy) {
    if (!therapy) {
        cancel_therapy_read();
        therapy_raw[0] = therapy_raw[1] = -1;
        return;
    }
    const uint32_t now = millis();
    if (!therapy_ticket) {
        if (therapy_step == 0 && therapy_poll_ms &&
            uint32_t(now - therapy_poll_ms) < THERAPY_POLL_MS) return;
        if (therapy_step == 0) therapy_poll_ms = now ? now : 1;
        const uart_send_window_t window = {0, 0, false, true};  // outside acquisition
        therapy_ticket = Arbiter::begin_var(therapy_step == 0 ? "MKI" : "MKE",
                                            CMD_SRC_INTERNAL, CMD_PRIO_LOW, 16,
                                            THERAPY_READ_MS, window);
        return;
    }
    if (!Arbiter::transaction_done(therapy_ticket)) {
        if (!Arbiter::transaction_expired(therapy_ticket)) return;
        cancel_therapy_read();
        return;
    }
    char value[16] = {};
    const auto result = Arbiter::finish_var(therapy_ticket, value, sizeof(value));
    therapy_ticket = nullptr;
    if (result == Arbiter::VarResult::Ok && value[0]) {
        char *end = nullptr;
        const unsigned long raw = strtoul(value, &end, 16);
        if (end && *end == 0 && raw <= 2000) {
            therapy_raw[therapy_step] = int32_t(raw);
            therapy_ok_ms[therapy_step] = now ? now : 1;
        }
    }
    therapy_step = therapy_step == 0 ? 1 : 0;
}

static bool therapy_value(int idx, float *out) {
    if (therapy_raw[idx] < 0 || !therapy_ok_ms[idx] ||
        uint32_t(millis() - therapy_ok_ms[idx]) > THERAPY_STALE_MS) return false;
    *out = therapy_raw[idx] / 50.0f;
    return true;
}

// ---- Drawing ----

struct Line {
    int16_t  y;
    uint8_t  size;          // built-in 6x8 font scale
    char     text[24];      // last drawn text, padded
    uint16_t color;
};

enum { L_IP, L_TIME, L_STATE, L_PRES, L_THER, L_SPO2, L_PULSE, L_COUNT };

static Line lines[L_COUNT] = {
    {   6, 2, "", 0 },      // IP address
    {  30, 5, "", 0 },      // time
    {  80, 3, "", 0 },      // AirSense state
    { 114, 2, "", 0 },      // mask pressure (measured)
    { 136, 2, "", 0 },      // therapy / exhale pressure (AirSense)
    { 164, 3, "", 0 },      // SpO2
    { 202, 2, "", 0 },      // pulse
};

// Draw text on a line, padded to the full width with background-filled
// spaces so old text is overwritten without clearing (no flicker).
// Skips the SPI work when nothing changed.
static void draw_line(int idx, const char *s, uint16_t color) {
    Line &l = lines[idx];
    int width = (SCR_W - SCR_MARGIN) / (6 * l.size);
    if (width > (int)sizeof(l.text) - 1) width = sizeof(l.text) - 1;

    char padded[sizeof(l.text)];
    snprintf(padded, sizeof(padded), "%-*.*s", width, width, s);
    if (color == l.color && strcmp(padded, l.text) == 0) return;

    gfx->setTextSize(l.size);
    gfx->setTextColor(color, RGB565_BLACK);
    gfx->setCursor(SCR_MARGIN, l.y);
    gfx->print(padded);

    memcpy(l.text, padded, sizeof(l.text));
    l.color = color;
}

static void draw_status() {
    char buf[32];

    // IP address
    if (WiFi.status() == WL_CONNECTED) {
        draw_line(L_IP, WiFi.localIP().toString().c_str(), RGB565_CYAN);
    } else if (WiFi.getMode() & WIFI_AP) {
        snprintf(buf, sizeof(buf), "AP %s", WiFi.softAPIP().toString().c_str());
        draw_line(L_IP, buf, RGB565_ORANGE);
    } else {
        draw_line(L_IP, "No WiFi", RGB565_DARKGREY);
    }

    // Local time (NTP, or ResMed clock fallback). Before any time source
    // is available the clock still reads 1970.
    time_t t = time(nullptr);
    if (t > 1700000000) {
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%H:%M", &tm);
        draw_line(L_TIME, buf, RGB565_WHITE);
    } else {
        draw_line(L_TIME, "--:--", RGB565_DARKGREY);
    }

    // AirSense state
    system_state_t st = Arbiter::get_state();
    bool therapy = (st == SYS_THERAPY);
    if (therapy) {
        draw_line(L_STATE, "THERAPY", RGB565_GREEN);
    } else if (st == SYS_IDLE) {
        if (AirSenseState::present_recently()) draw_line(L_STATE, "STANDBY", RGB565_WHITE);
        else                       draw_line(L_STATE, "NO AIRSENSE", RGB565_DARKGREY);
    } else if (st == SYS_ERROR) {
        draw_line(L_STATE, "UART ERROR", RGB565_RED);
    } else {
        draw_line(L_STATE, system_state_name(st), RGB565_YELLOW);
    }

    // Measured mask pressure (1 s average of the live stream)
    float p;
    if (therapy && take_pressure(&p)) {
        snprintf(buf, sizeof(buf), "Mask    %.1f cmH2O", p);
        draw_line(L_PRES, buf, RGB565_WHITE);
    } else {
        take_pressure(&p);      // drain any leftover samples
        draw_line(L_PRES, "Mask    --", RGB565_DARKGREY);
    }

    // AirSense therapy pressure, plus exhale pressure when EPR lowers it
    float ipap, epap;
    if (therapy && therapy_value(0, &ipap)) {
        if (therapy_value(1, &epap) && epap < ipap - 0.05f)
            snprintf(buf, sizeof(buf), "Therapy %.1f/%.1f", ipap, epap);
        else
            snprintf(buf, sizeof(buf), "Therapy %.1f cmH2O", ipap);
        draw_line(L_THER, buf, RGB565_CYAN);
    } else {
        draw_line(L_THER, "Therapy --", RGB565_DARKGREY);
    }

    // SpO2 / pulse from whichever oximetry source is feeding
    oxi_reading_t r;
    OxiArbiter::snapshot(r);
    if (r.valid && r.spo2 > 0) {
        snprintf(buf, sizeof(buf), "SpO2 %d%%", r.spo2);
        draw_line(L_SPO2, buf, RGB565_WHITE);
        if (r.pulse_bpm > 0) {
            snprintf(buf, sizeof(buf), "Pulse %d bpm", r.pulse_bpm);
            draw_line(L_PULSE, buf, RGB565_LIGHTGREY);
        } else {
            draw_line(L_PULSE, "", RGB565_LIGHTGREY);
        }
    } else {
        draw_line(L_SPO2, "SpO2 --", RGB565_DARKGREY);
        draw_line(L_PULSE, "", RGB565_LIGHTGREY);
    }
}

// ---- Backlight ----

static void set_backlight(int duty) {
    const uint32_t now = millis();
    if (duty == bl_duty && uint32_t(now - bl_check_ms) < BL_REFRESH_MS) return;
    bl_check_ms = now;

    // Self-heal: if anything took the pin away from LEDC or changed its duty,
    // restore it and say so in the log.
    if (perimanGetPinBusType(AB_LCD_BL_GPIO) != ESP32_BUS_TYPE_LEDC) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[LCD] backlight GPIO%d lost its PWM output, re-attaching\n",
                  AB_LCD_BL_GPIO);
        ledcAttach(AB_LCD_BL_GPIO, BL_FREQ_HZ, BL_RES_BITS);
        bl_duty = -1;
    } else if (duty == bl_duty) {
        const uint32_t hw = ledcRead(AB_LCD_BL_GPIO);
        if (hw != uint32_t(duty))
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[LCD] backlight duty was %lu, expected %d; restoring\n",
                      (unsigned long)hw, duty);
    }
    ledcWrite(AB_LCD_BL_GPIO, duty);
    bl_duty = duty;
}

// A press wakes the screen. A button held longer than BTN_STUCK_MS is
// treated as stuck (or a pin pulled low by other circuitry) and ignored
// until it reads released again.
static void poll_buttons(uint32_t now) {
    for (size_t i = 0; i < BTN_COUNT; i++) {
        const int pin = btn_pins[i];
        if (pin < 0) continue;
        if (digitalRead(pin) == HIGH) {
            if (btn_stuck[i])
                Log::logf(CAT_GENERAL, LOG_INFO,
                          "[LCD] button GPIO%d released after %lu ms\n",
                          pin, (unsigned long)(now - btn_down_ms[i]));
            btn_down_ms[i] = 0;
            btn_stuck[i] = false;
            continue;
        }
        if (!btn_down_ms[i]) btn_down_ms[i] = now ? now : 1;
        if (btn_stuck[i]) continue;
        if (uint32_t(now - btn_down_ms[i]) >= BTN_STUCK_MS) {
            btn_stuck[i] = true;
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[LCD] button GPIO%d held low for %d s, ignoring it until released\n",
                      pin, BTN_STUCK_MS / 1000);
            continue;
        }
        wake_until = now + AB_LCD_WAKE_MS;
    }
}

// ---- Public ----

void init() {
    bus = new Arduino_ESP32SPI(AB_LCD_DC_GPIO, AB_LCD_CS_GPIO, AB_LCD_SCLK_GPIO,
                               AB_LCD_MOSI_GPIO, GFX_NOT_DEFINED);
    gfx = new Arduino_ST7789(bus, AB_LCD_RST_GPIO, 0 /* rotation */,
                             true /* IPS */, SCR_W, SCR_H);
    if (!gfx->begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR, "[LCD] display init failed\n");
        delete gfx;
        gfx = nullptr;
        return;
    }
    gfx->fillScreen(RGB565_BLACK);

    for (int pin : btn_pins) {
        if (pin >= 0) pinMode(pin, INPUT_PULLUP);
    }

    ledcAttach(AB_LCD_BL_GPIO, BL_FREQ_HZ, BL_RES_BITS);
    set_backlight(BL_FULL);     // full brightness while booting

    draw_line(L_STATE, "STARTING", RGB565_YELLOW);
    Log::logf(CAT_GENERAL, LOG_INFO, "[LCD] status screen ready\n");
}

void tick() {
    if (!gfx) return;
    uint32_t now = millis();

    // Keep the screen bright for a while after boot so the IP can be read.
    if (!woke_once) {
        wake_until = now + AB_LCD_WAKE_MS;
        woke_once = true;
    }

    poll_buttons(now);
    bool awake = (int32_t)(wake_until - now) > 0;
    set_backlight(awake ? BL_FULL : AB_LCD_DIM_DUTY);

    update_pmd_subscription(Arbiter::get_state() == SYS_THERAPY);
    poll_therapy_pressure(Arbiter::get_state() == SYS_THERAPY);

    if (last_draw_ms != 0 && now - last_draw_ms < REFRESH_MS) return;
    last_draw_ms = now;
    if (last_draw_ms == 0) last_draw_ms = 1;
    draw_status();
}

void status(String &out) {
    if (!gfx) {
        out = "lcd: not initialized\n";
        return;
    }
    const uint32_t now = millis();
    const int32_t left = int32_t(wake_until - now);
    const bool attached = perimanGetPinBusType(AB_LCD_BL_GPIO) == ESP32_BUS_TYPE_LEDC;
    out = "backlight: GPIO" + String(AB_LCD_BL_GPIO) +
          " duty=" + String(bl_duty) + "/" + String(BL_FULL) +
          " hw=" + (attached ? String(ledcRead(AB_LCD_BL_GPIO)) : String("detached")) +
          " dim=" + String(AB_LCD_DIM_DUTY) + "\n";
    out += "awake: " + String(left > 0 ? "yes" : "no") +
           " (" + String(left > 0 ? left : 0) + " ms left)\n";
    for (size_t i = 0; i < BTN_COUNT; i++) {
        const int pin = btn_pins[i];
        if (pin < 0) continue;
        out += "button GPIO" + String(pin) + ": " +
               (digitalRead(pin) == HIGH ? String("released") : String("LOW"));
        if (btn_down_ms[i])
            out += " (held " + String(now - btn_down_ms[i]) + " ms" +
                   (btn_stuck[i] ? String(", ignored as stuck)") : String(")"));
        out += "\n";

        // Hardware view of the pad, to see what (if anything) has taken it
        // over: out_sig 256 = plain GPIO output, other values = peripheral.
        gpio_io_config_t io = {};
        if (gpio_get_io_config((gpio_num_t)pin, &io) == ESP_OK) {
            char line[160];
            snprintf(line, sizeof(line),
                     "  io: owner=%s fun=%lu out_sig=%lu oe=%d oe_periph=%d ie=%d "
                     "pu=%d pd=%d od=%d slp=%d level=%d\n",
                     perimanGetTypeName(perimanGetPinBusType(pin)),
                     (unsigned long)io.fun_sel, (unsigned long)io.sig_out,
                     io.oe, io.oe_ctrl_by_periph, io.ie, io.pu, io.pd, io.od,
                     io.slp_sel, gpio_get_level((gpio_num_t)pin));
            out += line;
        }
    }
}

}

#endif  // AB_LCD_ENABLED
