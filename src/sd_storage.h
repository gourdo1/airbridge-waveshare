#pragma once

#include <stdint.h>
#include <stddef.h>

namespace fs {
class FS;
class File;
}

namespace SdStorage {

struct Status {
    bool supported;
    bool mounted;
    uint64_t card_bytes;
    uint64_t used_bytes;
    char error[48];
};

inline uint16_t get_le16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

inline uint32_t get_le32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

inline void put_le16(uint8_t *data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
}

inline void put_le32(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}


// Binary storage records use little-endian fields regardless of alignment.
bool write_exact(fs::File &file, const uint8_t *data, size_t size);

void init();
bool mounted();
fs::FS *filesystem();
void refresh_usage();
void get_status(Status &out);

}  // namespace SdStorage
