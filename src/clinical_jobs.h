#pragma once

#include <Arduino.h>
#include "clinical_settings.h"

namespace ClinicalJobs {

using Handler = int (*)(const String &body, String &result);
enum class Kind : uint8_t { Read, Write, Report };
void init(Handler handler);
void tick();  // Nonblocking result/cache cleanup from the main loop.
// Results outlive HTTP requests: 30 s unread, at least 2 s after completion
// once delivered. Callers must not resubmit writes after a polling failure.
bool submit(Kind kind, const String &body, uint32_t &id);

// Pins the immutable values and metadata until the HTTP response is destroyed.
class Result {
public:
    Result() = default;
    ~Result();
    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;
    Result(Result &&other) noexcept;
    bool available() const { return id_ != 0; }
    size_t length() const { return length_; }
    size_t read(ClinicalSettings::Cursor &cursor, size_t offset, char *out, size_t capacity) const;
    void reset();

private:
    friend int poll(uint32_t id, Result &result);
    uint32_t id_ = 0;
    const char *data_ = nullptr;
    const ClinicalSettings::Snapshot *snapshot_ = nullptr;
    size_t length_ = 0;
};

int poll(uint32_t id, Result &result);  // 202 pending, 410 expired, 503 busy
uint16_t timeout_ms();               // Remaining worker budget, capped per UART request

}
