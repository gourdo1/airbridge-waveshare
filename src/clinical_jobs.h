#pragma once

#include <Arduino.h>

namespace ClinicalJobs {

using Handler = int (*)(bool write, const String &body, String &result);
void init(Handler handler);
// Results outlive HTTP requests: 30 s unread, at least 2 s after completion
// once delivered. Callers must not resubmit writes after a polling failure.
bool submit(bool write, const String &body, uint32_t &id);
int poll(uint32_t id, String &result);  // 202 pending, 410 expired, 503 busy
uint16_t timeout_ms();               // Remaining worker budget, capped per UART request

}
