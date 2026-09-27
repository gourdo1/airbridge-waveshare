#include "sd_storage.h"

#include <Arduino.h>
#include <algorithm>
#include <string.h>

#include "board.h"
#include "debug_log.h"

#if AB_STORAGE_HAS_SDCARD
#include <SD_MMC.h>
#include <FS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "memory_manager.h"
#include "uart_arbiter.h"
#endif

namespace SdStorage {

static Status status = {
    AB_STORAGE_HAS_SDCARD != 0,
    false,
    0,
    0,
    "",
};
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;

#if AB_STORAGE_HAS_SDCARD
namespace {
struct Request {
    enum Kind { Run, Acquire, TryAcquire, Release, Close, Begin, End } kind;
    bool (*operation)(fs::FS &, void *);
    void *context;
    uint32_t generation;
    SemaphoreHandle_t done;
    bool success;
    TaskHandle_t caller;
    uint32_t reader_id;
};

QueueHandle_t requests = nullptr;
uint32_t generation = 1;
uint32_t active_session = 0;
uint32_t recorder_waiting = 0;
TaskHandle_t worker = nullptr;
TaskHandle_t direct_owner = nullptr;

struct OpenFile {
    fs::File file;
    uint32_t id = 0;
};
OpenFile readers[2];
uint32_t next_reader = 0;

void close_readers() {
    for (OpenFile &reader : readers) {
        reader.file.close();
        reader.id = 0;
    }
}

OpenFile *find_reader(uint32_t id) {
    for (OpenFile &reader : readers)
        if (reader.id == id) return &reader;
    return nullptr;
}

bool background_allowed() {
    return !__atomic_load_n(&recorder_waiting, __ATOMIC_ACQUIRE) &&
        Arbiter::local_background_allowed();
}

bool background_allowed(uint32_t expected) {
    return expected && expected == __atomic_load_n(&active_session, __ATOMIC_ACQUIRE) &&
        background_allowed();
}

void process_request(Request &value) {
    Request *request = &value;
    request->success = false;
    if (request->kind == Request::Begin) {
        if (!direct_owner && !active_session && mounted() && background_allowed()) {
            if (++generation == 0) ++generation;
            __atomic_store_n(&active_session, generation, __ATOMIC_RELEASE);
            request->generation = generation;
            request->success = true;
        }
    } else if (request->kind == Request::End) {
        if (active_session == request->generation) {
            close_readers();
            __atomic_store_n(&active_session, 0, __ATOMIC_RELEASE);
        }
        request->success = true;
    } else if (request->kind == Request::Acquire || request->kind == Request::TryAcquire) {
        if (!direct_owner && (request->kind == Request::Acquire || !active_session)) {
            close_readers();
            __atomic_store_n(&active_session, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&direct_owner, request->caller, __ATOMIC_RELEASE);
            request->success = true;
        }
    } else if (request->kind == Request::Close) {
        OpenFile *reader = find_reader(request->reader_id);
        if (reader) { reader->file.close(); reader->id = 0; }
        request->success = true;
    } else if (request->kind == Request::Release) {
        if (direct_owner == request->caller) {
            __atomic_store_n(&direct_owner, nullptr, __ATOMIC_RELEASE);
            request->success = true;
        }
    } else if (!direct_owner && background_allowed(request->generation) && mounted()) {
        request->success = request->operation(SD_MMC, request->context);
    }
}

void io_task(void *) {
    Request *request;
    while (true) {
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
        process_request(*request);
        xSemaphoreGive(request->done);
    }
}

bool init_worker() {
    if (worker) return true;
    if (!requests) requests = xQueueCreate(4, sizeof(Request *));
    if (!requests) return false;
    BaseType_t created = pdFAIL;
    if (aircannect::Memory::psram_available())
        created = xTaskCreatePinnedToCoreWithCaps(io_task, "sd_io", 4096,
            nullptr, 1, &worker, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS)
        created = xTaskCreatePinnedToCore(io_task, "sd_io", 4096,
            nullptr, 1, &worker, 0);
    if (created != pdPASS) worker = nullptr;
    return worker != nullptr;
}

bool direct_request(Request &request) {
    // No executor means no auxiliary readers; keep the same ownership token.
    if (request.kind == Request::Acquire || request.kind == Request::TryAcquire) {
        TaskHandle_t expected = nullptr;
        return __atomic_compare_exchange_n(&direct_owner, &expected, request.caller,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    if (request.kind == Request::Release) {
        TaskHandle_t expected = request.caller;
        return __atomic_compare_exchange_n(&direct_owner, &expected, nullptr,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return false;
}

bool dispatch(Request &request, bool control) {
    request.caller = xTaskGetCurrentTaskHandle();
    if (!worker) return direct_request(request);
    if (request.caller == worker) return false;
    StaticSemaphore_t done_state;
    request.done = xSemaphoreCreateBinaryStatic(&done_state);
    request.success = false;
    Request *pointer = &request;
    const bool queued = xQueueSend(requests, &pointer, control ? portMAX_DELAY : 0) == pdTRUE;
    if (queued) xSemaphoreTake(request.done, portMAX_DELAY);
    vSemaphoreDelete(request.done);
    return queued && request.success;
}
}  // namespace

bool write_exact(fs::File &file, const uint8_t *data, size_t size) {
    return file && file.write(data, size) == size;
}

static void mount_error(const char *message) {
    portENTER_CRITICAL(&status_mux);
    strncpy(status.error, message, sizeof(status.error) - 1);
    status.error[sizeof(status.error) - 1] = 0;
    portEXIT_CRITICAL(&status_mux);
}
#endif

void init() {
#if AB_STORAGE_HAS_SDCARD
    if (mounted()) return;
    if (!init_worker())
        Log::logf(CAT_GENERAL, LOG_WARN, "[SD] auxiliary I/O unavailable; recorder only\n");

    bool pins_ok;
    if (AB_SDMMC_WIDTH == 1) {
        pins_ok = SD_MMC.setPins(AB_SDMMC_CLK_GPIO, AB_SDMMC_CMD_GPIO,
                                 AB_SDMMC_D0_GPIO);
    } else {
        pins_ok = SD_MMC.setPins(AB_SDMMC_CLK_GPIO, AB_SDMMC_CMD_GPIO,
                                 AB_SDMMC_D0_GPIO, AB_SDMMC_D1_GPIO,
                                 AB_SDMMC_D2_GPIO, AB_SDMMC_D3_GPIO);
    }
    if (!pins_ok) {
        mount_error("pin configuration failed");
        Log::logf(CAT_GENERAL, LOG_ERROR, "[SD] pin configuration failed\n");
        return;
    }

    const bool one_bit = AB_SDMMC_WIDTH == 1;
    if (!SD_MMC.begin("/sdcard", one_bit, false, AB_SDMMC_FREQ_KHZ, 8)) {
        mount_error("mount failed");
        Log::logf(CAT_GENERAL, LOG_ERROR, "[SD] mount failed\n");
        return;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        mount_error("no card");
        Log::logf(CAT_GENERAL, LOG_WARN, "[SD] no card detected\n");
        return;
    }

    const uint64_t card_bytes = SD_MMC.cardSize();
    const uint64_t used_bytes = SD_MMC.usedBytes();
    portENTER_CRITICAL(&status_mux);
    status.card_bytes = card_bytes;
    status.used_bytes = used_bytes;
    status.error[0] = 0;
    __atomic_store_n(&status.mounted, true, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[SD] mounted width=%u freq=%ukHz size=%lluMB\n",
              AB_SDMMC_WIDTH, AB_SDMMC_FREQ_KHZ,
              static_cast<unsigned long long>(card_bytes / (1024 * 1024)));
#endif
}

bool mounted() {
    return __atomic_load_n(&status.mounted, __ATOMIC_ACQUIRE);
}

fs::FS *filesystem() {
#if AB_STORAGE_HAS_SDCARD
    return mounted() ? &SD_MMC : nullptr;
#else
    return nullptr;
#endif
}

void refresh_usage() {
#if AB_STORAGE_HAS_SDCARD
    if (!mounted()) return;
    const uint64_t used_bytes = SD_MMC.usedBytes();
    portENTER_CRITICAL(&status_mux);
    status.used_bytes = used_bytes;
    portEXIT_CRITICAL(&status_mux);
#endif
}

void get_status(Status &out) {
    portENTER_CRITICAL(&status_mux);
    out = status;
    portEXIT_CRITICAL(&status_mux);
}

bool acquire() {
#if AB_STORAGE_HAS_SDCARD
    __atomic_add_fetch(&recorder_waiting, 1, __ATOMIC_ACQ_REL);
    Request request = {};
    request.kind = Request::Acquire;
    const bool taken = dispatch(request, true);
    __atomic_sub_fetch(&recorder_waiting, 1, __ATOMIC_ACQ_REL);
    return taken;
#else
    return false;
#endif
}

bool try_acquire() {
#if AB_STORAGE_HAS_SDCARD
    Request request = {};
    request.kind = Request::TryAcquire;
    return dispatch(request, false);
#else
    return false;
#endif
}

void release() {
#if AB_STORAGE_HAS_SDCARD
    Request request = {};
    request.kind = Request::Release;
    if (!dispatch(request, true))
        Log::logf(CAT_GENERAL, LOG_ERROR, "[SD] release by non-owner\n");
#endif
}

bool Session::begin() {
#if AB_STORAGE_HAS_SDCARD
    Request request = {};
    request.kind = Request::Begin;
    if (!dispatch(request, false)) return false;
    generation_ = request.generation;
    return true;
#else
    return false;
#endif
}

void Session::end() {
#if AB_STORAGE_HAS_SDCARD
    if (!generation_) return;
    Request request = {};
    request.kind = Request::End;
    request.generation = generation_;
    (void)dispatch(request, true);
    generation_ = 0;
#endif
}

bool Session::valid() const {
#if AB_STORAGE_HAS_SDCARD
    return worker && mounted() && background_allowed(generation_);
#else
    return false;
#endif
}

bool Session::run(bool (*operation)(fs::FS &, void *), void *context) const {
#if AB_STORAGE_HAS_SDCARD
    if (!operation || !valid() || xTaskGetCurrentTaskHandle() == worker) return false;
    Request request = {};
    request.kind = Request::Run;
    request.operation = operation;
    request.context = context;
    request.generation = generation_;
    return dispatch(request, false);
#else
    return false;
#endif
}

bool Reader::open(const Session &session, const char *path, bool directory) {
    close();
#if AB_STORAGE_HAS_SDCARD
    if (!path) return false;
    const bool opened = session.run([&](fs::FS &fs) {
        OpenFile *slot = nullptr;
        for (OpenFile &reader : readers) if (!reader.id) { slot = &reader; break; }
        if (!slot) return false;
        fs::File file = fs.open(path, FILE_READ);
        if (!file || file.isDirectory() != directory) return false;
        size_ = file.size();
        modified_ = file.getLastWrite();
        if (++next_reader == 0) ++next_reader;
        slot->id = handle_ = next_reader;
        slot->file = file;
        return true;
    });
    if (!opened) return false;
    session_ = session;
    return true;
#else
    return false;
#endif
}

bool Reader::next(Entry &entry, bool &end) {
    end = false;
#if AB_STORAGE_HAS_SDCARD
    if (!handle_) return false;
    return session_.run([&](fs::FS &) {
        OpenFile *reader = find_reader(handle_);
        if (!reader || !reader->file.isDirectory()) return false;
        fs::File file = reader->file.openNextFile();
        if (!file) { end = true; return true; }
        const char *name = file.name();
        if (!name || strlen(name) >= sizeof(entry.name)) return false;
        strcpy(entry.name, name);
        entry.directory = file.isDirectory();
        entry.size = file.size();
        entry.modified = file.getLastWrite();
        return true;
    });
#else
    return false;
#endif
}

size_t Reader::read(uint8_t *data, size_t length) {
    size_t total = 0;
#if AB_STORAGE_HAS_SDCARD
    if (!*this || !data) return 0;
    while (total < length && offset_ < size_) {
        const size_t count = std::min<size_t>(READ_CHUNK_BYTES,
            std::min<uint64_t>(length - total, size_ - offset_));
        const bool read = session_.run([&](fs::FS &) {
            OpenFile *reader = find_reader(handle_);
            return reader && reader->file.read(data + total, count) == count;
        });
        if (!read) break;
        total += count;
        offset_ += count;
    }
#endif
    return total;
}

void Reader::close() {
#if AB_STORAGE_HAS_SDCARD
    if (handle_) {
        Request request = {};
        request.kind = Request::Close;
        request.reader_id = handle_;
        (void)dispatch(request, true);
    }
#endif
    handle_ = 0;
    size_ = offset_ = 0;
}

}  // namespace SdStorage
