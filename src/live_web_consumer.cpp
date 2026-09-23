#include "live_web_consumer.h"
#include "live_stream.h"
#include "live_pmd.h"
#include "web_ui.h"
#include "oxi_arbiter.h"
#include "debug_log.h"

#define LWC_BATCH           5           // samples per SSE event (~5 Hz fire @ 25 Hz)

namespace LiveWebConsumer {

static Sample buf[HISTORY_CAPACITY];
static uint16_t head = 0;
static uint16_t ring_count = 0;
static uint16_t sent_seq = 0;
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;
static LiveStream::consumer_handle_t handle = -1;

// Refcounted SSE client tracking
static int      client_count = 0;

// Coalesce N samples per SSE event so the wire format mirrors /api/live and
// each event amortizes the SSE framing overhead across the batch.
static void flush_batch() {
    Sample batch[LWC_BATCH];
    portENTER_CRITICAL(&ring_mux);
    uint16_t available = (uint16_t)(head - sent_seq);
    if (available > ring_count) available = ring_count;
    if (available < LWC_BATCH || client_count == 0) {
        portEXIT_CRITICAL(&ring_mux);
        return;
    }
    uint16_t begin = (uint16_t)(head - available);
    for (uint8_t i = 0; i < LWC_BATCH; i++)
        batch[i] = buf[(uint16_t)(begin + i) % HISTORY_CAPACITY];
    sent_seq = (uint16_t)(begin + LWC_BATCH);
    uint16_t sequence = sent_seq;
    portEXIT_CRITICAL(&ring_mux);

    oxi_reading_t r;
    OxiArbiter::snapshot(r);
    char json[256];
    int n = snprintf(json, sizeof(json),
                     "{\"seq\":%u,\"samples\":[", sequence);
    for (int i = 0; i < LWC_BATCH && n < (int)sizeof(json); i++) {
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
}

static void on_pmd(const void *sample, uint16_t sample_size, void *ctx) {
    (void)ctx;
    if (sample_size < sizeof(LivePmd::Sample)) return;
    const LivePmd::Sample *s = (const LivePmd::Sample *)sample;

    // Always store every frame (25 Hz) into the ring; the GET /api/live
    // backfill endpoint walks since_seq -> head at full resolution.
    portENTER_CRITICAL(&ring_mux);
    uint16_t idx = head % HISTORY_CAPACITY;
    buf[idx] = {s->mkp, s->rfl, s->lyk};
    head = (uint16_t)(head + 1);

    if (ring_count < HISTORY_CAPACITY) ring_count++;
    portEXIT_CRITICAL(&ring_mux);
}

static void do_subscribe() {
    if (handle >= 0) return;
    handle = LiveStream::subscribe(LivePmd::TAG, on_pmd, nullptr);
    if (handle < 0) {
        Log::logf(CAT_STREAM, LOG_WARN, "[LWC] subscribe to PMD failed\n");
    } else {
        portENTER_CRITICAL(&ring_mux);
        int clients = client_count;
        portEXIT_CRITICAL(&ring_mux);
        Log::logf(CAT_STREAM, LOG_INFO, "[LWC] consumer registered (clients=%d)\n",
                  clients);
    }
}

static void do_unsubscribe() {
    if (handle < 0) return;
    LiveStream::unsubscribe(handle);
    handle = -1;
    portENTER_CRITICAL(&ring_mux);
    ring_count = 0;
    sent_seq = head;
    portEXIT_CRITICAL(&ring_mux);
    Log::logf(CAT_STREAM, LOG_INFO, "[LWC] unsubscribed (idle)\n");
}

void init() {
    LivePmd::register_parser();
    // No device subscription here. acquire() handles that on first client.
}

void shutdown() {
    do_unsubscribe();
    portENTER_CRITICAL(&ring_mux);
    client_count = 0;
    portEXIT_CRITICAL(&ring_mux);
}

void acquire() {
    portENTER_CRITICAL(&ring_mux);
    client_count++;
    portEXIT_CRITICAL(&ring_mux);
    // subscribe/unsubscribe is deferred to tick()
}

void release() {
    portENTER_CRITICAL(&ring_mux);
    if (client_count > 0) client_count--;
    portEXIT_CRITICAL(&ring_mux);
}

void tick() {
    portENTER_CRITICAL(&ring_mux);
    bool wanted = client_count > 0;
    portEXIT_CRITICAL(&ring_mux);
    if (wanted && handle < 0) {
        do_subscribe();
    }
    if (!wanted && handle >= 0) {
        do_unsubscribe();
    }
    // Bounded work on the main task; the UART callback only stores samples.
    if (wanted) for (uint8_t i = 0; i < 4; i++) flush_batch();
}

int get_samples(Sample *out, int max,
                uint16_t since_seq, uint16_t *cur_seq) {
    portENTER_CRITICAL(&ring_mux);
    uint16_t seq = head;
    if (cur_seq) *cur_seq = seq;
    if (!out || max <= 0) {
        portEXIT_CRITICAL(&ring_mux);
        return 0;
    }

    uint16_t available = (uint16_t)(seq - since_seq);
    if (available > ring_count) available = ring_count;
    if (available > (uint16_t)max) available = (uint16_t)max;

    for (uint16_t i = 0; i < available; i++) {
        uint16_t idx = (uint16_t)(seq - available + i) % HISTORY_CAPACITY;
        out[i] = buf[idx];
    }
    portEXIT_CRITICAL(&ring_mux);
    return available;
}

bool is_active() {
    return LiveStream::is_stream_active(LivePmd::TAG);
}

}
