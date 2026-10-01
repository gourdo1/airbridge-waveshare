#pragma once
#include <stdint.h>

// Web UI consumer for the TCE live stream. Owns the per-frame ring buffer
// served by /api/live, and pushes throttled SSE "live" events for clients
// that prefer event-source pull.

namespace LiveWebConsumer {

constexpr uint16_t HISTORY_CAPACITY = 128;

struct Sample {
    int16_t mkp;
    int16_t rfl;
    int16_t lyk;
};

// Lifecycle is driven by /events/live presence: web_ui calls acquire() on
// AsyncEventSource onConnect and release() on onDisconnect. The TCE device
// subscription is held only while at least one live-chart client is connected.
// The RX callback only stores samples; tick() publishes from the main task.

void init();
void shutdown();

void acquire();
void release();
void tick();

// Copy samples with seq > since_seq into out[]. Returns count. Always
// writes the current sequence to *cur_seq (NULL allowed).
int  get_samples(Sample *out, int max,
                 uint16_t since_seq, uint16_t *cur_seq);

bool     is_active();

}
