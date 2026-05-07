#include "live_stream.h"
#include "uart_arbiter.h"
#include "debug_log.h"
#include <string.h>
#include <esp_timer.h>

#define LS_CB_WARN_US           2000
#define LS_DEVICE_CMD_TIMEOUT   1000
#define LS_DECODE_BUF           64

namespace LiveStream {

struct Stream {
    char        tag[LIVE_STREAM_TAG_LEN + 1];
    decode_fn_t decode_fn;
    uint16_t    sample_size;
    uint8_t     ref_count;
    bool        subscribed;
    bool        in_use;
};

struct Consumer {
    bool          in_use;
    int8_t        stream_idx;
    consumer_cb_t cb;
    void         *ctx;
};

static Stream    streams[LIVE_STREAMS_MAX];
static Consumer  consumers[LIVE_CONSUMERS_MAX];
static bool      initialized = false;

static int find_stream_idx(const char *tag) {
    for (int i = 0; i < LIVE_STREAMS_MAX; i++) {
        if (streams[i].in_use &&
            memcmp(streams[i].tag, tag, LIVE_STREAM_TAG_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static bool device_subscribe(int sidx, bool on) {
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "P S &%s %d", streams[sidx].tag, on ? 1 : 0);
    char resp[32] = {};
    uint16_t resp_len = sizeof(resp);
    return Arbiter::send_cmd(cmd, CMD_SRC_INTERNAL, CMD_PRIO_NORMAL,
                             resp, &resp_len, LS_DEVICE_CMD_TIMEOUT);
}

void init() {
    if (initialized) return;
    memset(streams, 0, sizeof(streams));
    memset(consumers, 0, sizeof(consumers));
    initialized = true;
    Log::logf(CAT_GENERAL, LOG_INFO, "[LS] broker init\n");
}

bool register_stream(const char *tag, decode_fn_t decode_fn, uint16_t sample_size) {
    if (!initialized) init();
    if (!tag || !decode_fn || sample_size == 0 || sample_size > LS_DECODE_BUF)
        return false;
    if (find_stream_idx(tag) >= 0) return false;

    for (int i = 0; i < LIVE_STREAMS_MAX; i++) {
        if (!streams[i].in_use) {
            memcpy(streams[i].tag, tag, LIVE_STREAM_TAG_LEN);
            streams[i].tag[LIVE_STREAM_TAG_LEN] = 0;
            streams[i].decode_fn   = decode_fn;
            streams[i].sample_size = sample_size;
            streams[i].ref_count   = 0;
            streams[i].subscribed  = false;
            streams[i].in_use      = true;
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[LS] stream '%s' registered\n", streams[i].tag);
            return true;
        }
    }
    Log::logf(CAT_GENERAL, LOG_WARN,
              "[LS] stream table full, '%c%c%c' rejected\n",
              tag[0], tag[1], tag[2]);
    return false;
}

consumer_handle_t subscribe(const char *tag, consumer_cb_t cb, void *ctx) {
    if (!cb) return -1;
    int sidx = find_stream_idx(tag);
    if (sidx < 0) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[LS] subscribe to unknown tag '%c%c%c'\n",
                  tag[0], tag[1], tag[2]);
        return -1;
    }

    int cidx = -1;
    for (int i = 0; i < LIVE_CONSUMERS_MAX; i++) {
        if (!consumers[i].in_use) { cidx = i; break; }
    }
    if (cidx < 0) {
        Log::logf(CAT_GENERAL, LOG_WARN, "[LS] consumer table full\n");
        return -1;
    }

    consumers[cidx].in_use     = true;
    consumers[cidx].stream_idx = (int8_t)sidx;
    consumers[cidx].cb         = cb;
    consumers[cidx].ctx        = ctx;

    streams[sidx].ref_count++;
    if (streams[sidx].ref_count == 1 && !streams[sidx].subscribed) {
        if (device_subscribe(sidx, true)) {
            streams[sidx].subscribed = true;
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[LS] %s subscribed (refs=1)\n", streams[sidx].tag);
        } else {
            Log::logf(CAT_GENERAL, LOG_DEBUG,
                      "[LS] %s subscribe failed (will retry on resync)\n",
                      streams[sidx].tag);
        }
    }
    return (consumer_handle_t)cidx;
}

void unsubscribe(consumer_handle_t h) {
    if (h < 0 || h >= LIVE_CONSUMERS_MAX) return;
    if (!consumers[h].in_use) return;

    int sidx = consumers[h].stream_idx;
    consumers[h].in_use = false;
    consumers[h].cb     = nullptr;
    consumers[h].ctx    = nullptr;

    if (sidx >= 0 && sidx < LIVE_STREAMS_MAX && streams[sidx].in_use) {
        if (streams[sidx].ref_count > 0) streams[sidx].ref_count--;
        if (streams[sidx].ref_count == 0 && streams[sidx].subscribed) {
            device_subscribe(sidx, false);
            streams[sidx].subscribed = false;
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[LS] %s unsubscribed (refs=0)\n", streams[sidx].tag);
        }
    }
}

void suspend() {
    for (int i = 0; i < LIVE_STREAMS_MAX; i++) {
        if (!streams[i].in_use || !streams[i].subscribed) continue;
        device_subscribe(i, false);
        streams[i].subscribed = false;
        Log::logf(CAT_GENERAL, LOG_INFO, "[LS] %s suspended\n", streams[i].tag);
    }
}

void resume() {
    for (int i = 0; i < LIVE_STREAMS_MAX; i++) {
        if (!streams[i].in_use) continue;
        if (streams[i].ref_count > 0 && !streams[i].subscribed) {
            if (device_subscribe(i, true)) {
                streams[i].subscribed = true;
                Log::logf(CAT_GENERAL, LOG_INFO,
                          "[LS] %s resumed\n", streams[i].tag);
            }
        }
    }
}

void resync() {
    for (int i = 0; i < LIVE_STREAMS_MAX; i++) {
        if (!streams[i].in_use) continue;
        if (streams[i].ref_count > 0 && !streams[i].subscribed) {
            if (device_subscribe(i, true)) {
                streams[i].subscribed = true;
                Log::logf(CAT_GENERAL, LOG_INFO,
                          "[LS] %s re-subscribed\n", streams[i].tag);
            }
        }
    }
}

void on_l_frame(const uint8_t *payload, uint16_t len) {
    if (!initialized || len < LIVE_STREAM_TAG_LEN) return;

    int sidx = -1;
    for (int i = 0; i < LIVE_STREAMS_MAX; i++) {
        if (streams[i].in_use &&
            memcmp(streams[i].tag, payload, LIVE_STREAM_TAG_LEN) == 0) {
            sidx = i;
            break;
        }
    }
    if (sidx < 0) return;

    // Decode once into a stack buffer, then fan out to consumers.
    uint8_t sample_buf[LS_DECODE_BUF];
    uint16_t sample_size = streams[sidx].sample_size;
    if (sample_size > sizeof(sample_buf)) return;
    if (!streams[sidx].decode_fn(payload, len, sample_buf, sample_size))
        return;

    for (int i = 0; i < LIVE_CONSUMERS_MAX; i++) {
        if (!consumers[i].in_use) continue;
        if (consumers[i].stream_idx != sidx) continue;
        consumer_cb_t cb = consumers[i].cb;
        void *ctx = consumers[i].ctx;
        if (!cb) continue;

        int64_t t0 = esp_timer_get_time();
        cb(sample_buf, sample_size, ctx);
        int64_t dt = esp_timer_get_time() - t0;
        if (dt > LS_CB_WARN_US) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[LS] %s consumer cb slow: %lld us\n",
                      streams[sidx].tag, (long long)dt);
        }
    }
}

bool is_stream_active(const char *tag) {
    int sidx = find_stream_idx(tag);
    return sidx >= 0 && streams[sidx].subscribed;
}

bool is_active() {
    for (int i = 0; i < LIVE_STREAMS_MAX; i++) {
        if (streams[i].in_use && streams[i].subscribed) return true;
    }
    return false;
}

}
