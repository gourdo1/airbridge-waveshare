#pragma once

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <memory>

namespace StorageBrowser {

enum class Kind { List, File, Archive };

struct Request {
    Kind kind;
    char path[256];
    char selection[2048]; // Newline-separated child names; empty selects path.
    uint32_t offset;
};

class Transfer {
public:
    virtual ~Transfer() = default;
    virtual size_t read(uint8_t *out, size_t capacity) = 0;
    virtual bool failed() const = 0;
    virtual bool finished() const = 0;
    virtual void cancel() = 0;
};

using Ready = std::function<void(int code, const char *error,
    std::shared_ptr<Transfer> transfer, uint64_t size)>;

// One browser operation, including its response lifetime. Ready runs outside
// the SD executor. Size zero for archives denotes HTTP chunked transfer.
bool start(const Request &request, Ready ready, std::weak_ptr<Transfer> &active);

}  // namespace StorageBrowser
