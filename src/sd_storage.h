#pragma once

#include <stdint.h>

namespace fs {
class FS;
}

namespace SdStorage {

struct Status {
    bool supported;
    bool mounted;
    uint64_t card_bytes;
    uint64_t used_bytes;
    char error[48];
};

void init();
bool mounted();
fs::FS *filesystem();
void refresh_usage();
void get_status(Status &out);

}  // namespace SdStorage
