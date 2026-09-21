#pragma once

#include "custom_settings.h"
#include <stddef.h>
#include <stdint.h>

namespace ClinicalSettings {

struct Value {
    uint32_t raw;
    uint16_t descriptor;
    uint8_t group;
    uint8_t flags;
};
static_assert(sizeof(Value) == 8, "Clinical values must remain compact");

class Cursor;
class Snapshot {
public:
    Snapshot() = default;
    ~Snapshot();
    Snapshot(const Snapshot &) = delete;
    Snapshot &operator=(const Snapshot &) = delete;
    void reset();
    size_t length() const { return length_; }
    size_t storage_bytes() const { return count_ * sizeof(Value); }
    const char *error() const { return error_; }

private:
    friend class Cursor;
    friend int collect(Snapshot &snapshot);
    CustomSettings::MetadataLease metadata_;
    Value *values_ = nullptr;
    uint16_t count_ = 0;
    size_t length_ = 0;
    const char *error_ = nullptr;
};

// A resumable token stream. Its strings refer only to the pinned snapshot.
class Cursor {
public:
    size_t read(const Snapshot &snapshot, char *out, size_t capacity);

private:
    bool next(const Snapshot &snapshot);
    void token(const char *text, bool quoted = false, size_t length = SIZE_MAX);
    void field(const char *key, const char *text, bool quoted = true);
    void integer(const char *key, int64_t value);
    void decimal(const char *key, double value, uint8_t places);

    uint16_t row_ = 0;
    uint8_t stage_ = 0, option_ = 0;
    size_t option_offset_ = 0;
    const char *text_ = nullptr, *pending_ = nullptr;
    size_t offset_ = 0, length_ = 0;
    bool quoted_ = false, pending_quoted_ = false;
    uint8_t quote_phase_ = 0;
    char number_[48] = {};
    char escape_ = 0;
};

int collect(Snapshot &snapshot);  // Worker only.
bool read_raw(const char *cmd, int &value);
bool known_stock(const char *cmd);

}  // namespace ClinicalSettings
