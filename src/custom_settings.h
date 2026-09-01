#pragma once

#include <stddef.h>
#include <stdint.h>

namespace CustomSettings {

enum kind_t : uint8_t {
    KIND_NUMERIC,
    KIND_ENUM,
};

struct option_view_t {
    uint8_t value;
    const char *label;
};

struct entry_view_t {
    kind_t kind;
    uint8_t category;
    uint32_t mop_mask;
    const char *name;
    const char *label;
    const char *units;
    uint8_t flags;
    uint8_t width;
    int16_t scale;
    int16_t step;
    uint8_t decimals;
    uint32_t minimum;
    uint32_t maximum;
    const option_view_t *options;
    uint8_t option_count;
};

typedef void (*entry_visitor_t)(const entry_view_t &entry, bool value_ok,
                                uint32_t value, void *context);

void init();
bool ensure_loaded();
bool contains(const char *name);
void visit_category(uint8_t category, uint8_t mop, entry_visitor_t visitor,
                    void *context);
bool write_raw(const char *name, uint32_t value);

// Invalidate metadata after a device reboot, firmware replacement, or LAN change.
void invalidate(const char *reason);

}  // namespace CustomSettings
