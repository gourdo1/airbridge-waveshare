#pragma once

#include "custom_settings.h"
#include "json_cursor.h"
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
class Cursor : private JsonCursor {
public:
    size_t read(const Snapshot &snapshot, char *out, size_t capacity);

private:
    bool next(const Snapshot &snapshot);
    uint16_t row_ = 0;
    uint8_t stage_ = 0, option_ = 0;
    size_t option_offset_ = 0;
};

int collect(Snapshot &snapshot);  // Worker only.
bool read_raw(const char *cmd, int &value);
bool known_stock(const char *cmd);
void mode_label(int mode, char *out, size_t capacity);

}  // namespace ClinicalSettings
