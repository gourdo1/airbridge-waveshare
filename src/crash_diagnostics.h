#pragma once

#include <esp_err.h>
#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace CrashDiagnostics {

constexpr size_t BACKTRACE_MAX = 16;

enum class State : uint8_t { Unsupported, Empty, Available, Invalid };

struct Snapshot {
    State state = State::Unsupported;
    size_t size = 0;
    uint32_t stored_size = 0;
    esp_err_t error = ESP_OK;
    bool summary_available = false;
    char task[17] = {};
    char reason[160] = {};
    char elf_sha[65] = {};
    uint32_t pc = 0;
    uint32_t cause = 0;
    uint32_t exception_address = 0;
    uint32_t backtrace[BACKTRACE_MAX] = {};
    uint8_t backtrace_depth = 0;
    bool backtrace_corrupt = false;
};

void init();
bool snapshot(Snapshot &out);
const char *state_name(State state);
// Null means success. Refuses during a download or active device work.
const char *clear();

class Dump {
public:
    ~Dump();
    Dump(const Dump &) = delete;
    Dump &operator=(const Dump &) = delete;

    size_t size() const { return size_; }
    size_t read(size_t offset, void *out, size_t capacity) const;

private:
    friend std::unique_ptr<Dump> open_dump(const char *&error);
    Dump() = default;

    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    uint32_t mapping_ = 0;
};

// One read-only mapping at a time; destruction releases it and permits clear.
std::unique_ptr<Dump> open_dump(const char *&error);

}  // namespace CrashDiagnostics
