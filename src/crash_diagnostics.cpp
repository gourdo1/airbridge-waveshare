#include "crash_diagnostics.h"
#include "airsense_state.h"

#include <esp_core_dump.h>
#include <esp_partition.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>

namespace CrashDiagnostics {
namespace {

std::mutex mutex;
Snapshot cached;
const esp_partition_t *partition = nullptr;
bool initialized = false;
bool download_active = false;

void refresh_locked() {
    cached = {};
    partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!partition) return;

    cached.state = State::Invalid;
    uint32_t stored_size = 0;
    cached.error = esp_partition_read(partition, 0, &stored_size, sizeof(stored_size));
    if (cached.error != ESP_OK) return;
    if (stored_size == UINT32_MAX) {
        cached.state = State::Empty;
        return;
    }
    cached.stored_size = stored_size;

    cached.error = esp_core_dump_image_check();
    if (cached.error == ESP_ERR_NOT_FOUND) {
        cached.state = State::Empty;
        cached.error = ESP_OK;
        return;
    }
    if (cached.error != ESP_OK) return;

    size_t address = 0, size = 0;
    cached.error = esp_core_dump_image_get(&address, &size);
    if (cached.error != ESP_OK) return;
    if (address != partition->address || !size || size > partition->size) {
        cached.error = ESP_ERR_INVALID_SIZE;
        return;
    }
    cached.state = State::Available;
    cached.size = size;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t summary = {};
    cached.error = esp_core_dump_get_summary(&summary);
    if (cached.error != ESP_OK) return;

    cached.summary_available = true;
    memcpy(cached.task, summary.exc_task,
           std::min(sizeof(summary.exc_task), sizeof(cached.task) - 1));
    memcpy(cached.elf_sha, summary.app_elf_sha256,
           std::min(sizeof(summary.app_elf_sha256), sizeof(cached.elf_sha) - 1));
    cached.pc = summary.exc_pc;
    cached.cause = summary.ex_info.exc_cause;
    cached.exception_address = summary.ex_info.exc_vaddr;
    cached.backtrace_depth = std::min(
        static_cast<size_t>(summary.exc_bt_info.depth), BACKTRACE_MAX);
    memcpy(cached.backtrace, summary.exc_bt_info.bt,
           cached.backtrace_depth * sizeof(cached.backtrace[0]));
    cached.backtrace_corrupt = summary.exc_bt_info.corrupted;
    cached.error = esp_core_dump_get_panic_reason(cached.reason, sizeof(cached.reason));
    if (cached.error == ESP_ERR_NOT_FOUND) cached.error = ESP_OK;
#endif
}

}  // namespace

void init() {
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) return;
    refresh_locked();
    initialized = true;
}

bool snapshot(Snapshot &out) {
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock() || !initialized) return false;
    out = cached;
    return true;
}

const char *state_name(State state) {
    switch (state) {
        case State::Unsupported: return "unsupported";
        case State::Empty: return "empty";
        case State::Available: return "available";
        case State::Invalid: return "invalid";
    }
    return "unknown";
}

const char *clear() {
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock() || !initialized) return "busy";
    if (!partition) return "unsupported";
    if (download_active) return "download_active";
    if (!AirSenseState::local_background_allowed()) return "not_idle";
    if (cached.state == State::Empty) return nullptr;

    const esp_err_t result = esp_core_dump_image_erase();
    // A failed erase may have changed the image; never retain a valid cache then.
    refresh_locked();
    if (result != ESP_OK) return esp_err_to_name(result);
    if (cached.state != State::Empty) return "clear_not_confirmed";
    return nullptr;
}

std::unique_ptr<Dump> open_dump(const char *&error) {
    error = nullptr;
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock() || !initialized) {
        error = "busy";
        return {};
    }
    if (download_active) {
        error = "download_active";
        return {};
    }
    if (cached.state != State::Available) {
        error = state_name(cached.state);
        return {};
    }

    std::unique_ptr<Dump> dump(new (std::nothrow) Dump);
    if (!dump) {
        error = "allocation_failed";
        return {};
    }
    const void *data = nullptr;
    const esp_err_t result = esp_partition_mmap(
        partition, 0, cached.size, ESP_PARTITION_MMAP_DATA, &data, &dump->mapping_);
    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        return {};
    }
    dump->data_ = static_cast<const uint8_t *>(data);
    dump->size_ = cached.size;
    download_active = true;
    return dump;
}

Dump::~Dump() {
    if (!data_) return;
    std::lock_guard<std::mutex> lock(mutex);
    esp_partition_munmap(mapping_);
    download_active = false;
}

size_t Dump::read(size_t offset, void *out, size_t capacity) const {
    if (!out || offset >= size_) return 0;
    const size_t count = std::min(capacity, size_ - offset);
    memcpy(out, data_ + offset, count);
    return count;
}

}  // namespace CrashDiagnostics
