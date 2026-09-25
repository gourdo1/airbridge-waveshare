#include "status_screen.h"

#if AB_LCD_ENABLED

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include "uart_arbiter.h"
#include "oxi_arbiter.h"
#include "live_stream.h"
#include "live_pmd.h"
#include "debug_log.h"

bool airsense_is_present();     // main.cpp

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

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *gfx = nullptr;

static const int btn_pins[] = { AB_BTN1_GPIO, AB_BTN2_GPIO, AB_BTN3_GPIO };

static uint32_t wake_until = 0;
static bool     woke_once = false;
static int      bl_duty = -1;
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
    if (sample_size < sizeof(LivePmd::Sample)) return;
    const LivePmd::Sample *s = (const LivePmd::Sample *)sample;
    portENTER_CRITICAL(&pmd_mux);
    pmd_sum += s->mkp;
    pmd_count++;
    pmd_last_ms = millis();
    portEXIT_CRITICAL(&pmd_mux);
}

static void update_pmd_subscription(bool want) {
    if (want && pmd_handle < 0) {
        if (pmd_retry_ms && millis() - pmd_retry_ms < PMD_RETRY_MS) return;
        pmd_handle = LiveStream::subscribe(LivePmd::TAG, on_pmd, nullptr);
        if (pmd_handle < 0) {
            pmd_retry_ms = millis();
            if (pmd_retry_ms == 0) pmd_retry_ms = 1;
            Log::logf(CAT_GENERAL, LOG_WARN, "[LCD] PMD subscribe failed\n");
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

// ---- Drawing ----

struct Line {
    int16_t  y;
    uint8_t  size;          // built-in 6x8 font scale
    char     text[24];      // last drawn text, padded
    uint16_t color;
};

enum { L_IP, L_TIME, L_STATE, L_PRES, L_SPO2, L_PULSE, L_COUNT };

static Line lines[L_COUNT] = {
    {   6, 2, "", 0 },      // IP address
    {  34, 5, "", 0 },      // time
    {  88, 3, "", 0 },      // AirSense state
    { 128, 3, "", 0 },      // mask pressure
    { 166, 3, "", 0 },      // SpO2
    { 204, 2, "", 0 },      // pulse
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
        if (airsense_is_present()) draw_line(L_STATE, "STANDBY", RGB565_WHITE);
        else                       draw_line(L_STATE, "NO AIRSENSE", RGB565_DARKGREY);
    } else if (st == SYS_ERROR) {
        draw_line(L_STATE, "UART ERROR", RGB565_RED);
    } else {
        draw_line(L_STATE, system_state_name(st), RGB565_YELLOW);
    }

    // Mask pressure, only meaningful during therapy
    float p;
    if (therapy && take_pressure(&p)) {
        snprintf(buf, sizeof(buf), "%.1f cmH2O", p);
        draw_line(L_PRES, buf, RGB565_WHITE);
    } else {
        take_pressure(&p);      // drain any leftover samples
        draw_line(L_PRES, "-- cmH2O", RGB565_DARKGREY);
    }

    // SpO2 / pulse from whichever oximetry source is feeding
    oxi_reading_t r = OxiArbiter::get_reading();
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
    if (duty == bl_duty) return;
    ledcWrite(AB_LCD_BL_GPIO, duty);
    bl_duty = duty;
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

    for (int pin : btn_pins) {
        if (pin >= 0 && digitalRead(pin) == LOW) wake_until = now + AB_LCD_WAKE_MS;
    }
    bool awake = (int32_t)(wake_until - now) > 0;
    set_backlight(awake ? BL_FULL : AB_LCD_DIM_DUTY);

    update_pmd_subscription(Arbiter::get_state() == SYS_THERAPY);

    if (last_draw_ms != 0 && now - last_draw_ms < REFRESH_MS) return;
    last_draw_ms = now;
    if (last_draw_ms == 0) last_draw_ms = 1;
    draw_status();
}

}

#endif  // AB_LCD_ENABLED
