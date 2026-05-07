#include "live_web_consumer.h"
#include "live_stream.h"
#include "live_pmd.h"
#include "web_ui.h"
#include "oxi_arbiter.h"
#include "debug_log.h"

#define LWC_BUF             128         // ~5 s @ 25 Hz
#define LWC_BATCH           5           // samples per SSE event (~5 Hz fire @ 25 Hz)
#define LWC_GRACE_MS        10000       // hold subscription for 10 s after last client leaves

namespace LiveWebConsumer {

static LivePmd::Sample buf[LWC_BUF];
static volatile uint16_t head = 0;
static LiveStream::consumer_handle_t handle = -1;

// Refcounted SSE client tracking
static int      client_count = 0;
static uint32_t release_ms   = 0;

// Coalesce N samples per SSE event so the wire format mirrors /api/live and
// each event amortizes the SSE framing overhead across the batch.
static LivePmd::Sample batch[LWC_BATCH];
static uint8_t batch_n = 0;

static void flush_batch() {
    if (batch_n == 0) return;

    const oxi_reading_t &r = OxiArbiter::get_reading();
    char json[256];
    int n = snprintf(json, sizeof(json),
                     "{\"seq\":%u,\"samples\":[", head);
    for (int i = 0; i < batch_n && n < (int)sizeof(json); i++) {
        n += snprintf(json + n, sizeof(json) - n,
                      "%s[%d,%d,%d]", i ? "," : "",
                      batch[i].mkp, batch[i].rfl, batch[i].lyk);
    }
    if (n < (int)sizeof(json)) {
        n += snprintf(json + n, sizeof(json) - n,
                      "],\"spo2\":%d,\"pulse\":%d}",
                      r.valid ? r.spo2 : -1,
                      r.valid ? r.pulse_bpm : -1);
    }
    WebUI::push_event("live", json);
    batch_n = 0;
}

static void on_pmd(const void *sample, uint16_t sample_size, void *ctx) {
    (void)ctx;
    if (sample_size < sizeof(LivePmd::Sample)) return;
    const LivePmd::Sample *s = (const LivePmd::Sample *)sample;

    // Always store every frame (25 Hz) into the ring; the GET /api/live
    // backfill endpoint walks since_seq -> head at full resolution.
    uint16_t idx = head % LWC_BUF;
    buf[idx] = *s;
    head = (uint16_t)(head + 1);

    // Accumulate into batch; flush when full.
    batch[batch_n++] = *s;
    if (batch_n >= LWC_BATCH) flush_batch();
}

static void do_subscribe() {
    if (handle >= 0) return;
    handle = LiveStream::subscribe(LivePmd::TAG, on_pmd, nullptr);
    if (handle < 0) {
        Log::logf(CAT_GENERAL, LOG_WARN, "[LWC] subscribe to PMD failed\n");
    } else {
        Log::logf(CAT_GENERAL, LOG_INFO, "[LWC] subscribed (clients=%d)\n",
                  client_count);
    }
}

static void do_unsubscribe() {
    if (handle < 0) return;
    LiveStream::unsubscribe(handle);
    handle = -1;
    head = 0;       // drop stale ring; next subscribe starts fresh
    batch_n = 0;
    Log::logf(CAT_GENERAL, LOG_INFO, "[LWC] unsubscribed (idle)\n");
}

void init() {
    LivePmd::register_parser();
    // No device subscription here. acquire() handles that on first client.
}

void shutdown() {
    do_unsubscribe();
    client_count = 0;
    release_ms = 0;
}

void acquire() {
    if (client_count == 0) release_ms = 0;
    client_count++;
    if (client_count == 1 && handle < 0) do_subscribe();
}

void release() {
    if (client_count > 0) client_count--;
    if (client_count == 0) {
        release_ms = millis();
        if (release_ms == 0) release_ms = 1;
    }
}

void tick() {
    if (release_ms == 0 || client_count != 0) return;
    if (millis() - release_ms >= LWC_GRACE_MS) {
        do_unsubscribe();
        release_ms = 0;
    }
}

int get_samples(LivePmd::Sample *out, int max,
                uint16_t since_seq, uint16_t *cur_seq) {
    uint16_t seq = head;
    if (cur_seq) *cur_seq = seq;
    if (!out || max <= 0) return 0;

    uint16_t available = (uint16_t)(seq - since_seq);
    if (available > LWC_BUF) available = LWC_BUF;
    if (available > (uint16_t)max) available = (uint16_t)max;

    for (uint16_t i = 0; i < available; i++) {
        uint16_t idx = (uint16_t)(seq - available + i) % LWC_BUF;
        out[i] = buf[idx];
    }
    return available;
}

bool is_active() {
    return LiveStream::is_stream_active(LivePmd::TAG);
}

uint16_t current_seq() {
    return head;
}

}
