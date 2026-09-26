#pragma once

#include <stddef.h>
#include <stdint.h>

namespace EdfCatalog {

enum EntryFlags : uint8_t {
    ENTRY_LIVE_COMPLETE = 1u << 0,
    ENTRY_IDENTIFICATION_READY = 1u << 1,
    ENTRY_STR_READY = 1u << 2,
};

struct Entry {
    char therapy_day[9];
    char file_prefix[16];
    uint8_t flags;
    uint32_t finalized_epoch;
    uint32_t str_revision;
};

struct Status {
    bool supported;
    bool ready;
    uint32_t entries;
    uint32_t generation;
    char error[48];
};

void init();
bool commit(const Entry &entry);
bool read(uint32_t index, Entry &entry);
bool find(const char *file_prefix, Entry &entry);
bool snapshot_prefixes(char *out, size_t out_size, uint32_t &count);
void get_status(Status &out);

}  // namespace EdfCatalog
