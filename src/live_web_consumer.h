#pragma once
#include "live_pmd.h"

// Web UI consumer for the PMD live stream. Owns the per-frame ring buffer
// served by /api/live, and pushes throttled SSE "live" events for clients
// that prefer event-source pull.

namespace LiveWebConsumer {

// Lifecycle is driven by SSE client presence: web_ui calls acquire() on
// AsyncEventSource onConnect and release() on onDisconnect. The PMD device
// subscription is held only while at least one SSE client is connected, with
// a small grace period to absorb page reloads / brief network blips.

void init();
void shutdown();

void acquire();
void release();
void tick();

// Copy samples with seq > since_seq into out[]. Returns count. Always
// writes the current sequence to *cur_seq (NULL allowed).
int  get_samples(LivePmd::Sample *out, int max,
                 uint16_t since_seq, uint16_t *cur_seq);

bool     is_active();
uint16_t current_seq();

}
