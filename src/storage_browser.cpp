#include "storage_browser.h"
#include "board.h"

#if AB_STORAGE_HAS_SDCARD
#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>
#include <time.h>
#include "crc.h"
#include "json_util.h"
#include "large_text_buffer.h"
#include "memory_manager.h"
#include "sd_storage.h"

namespace StorageBrowser {
namespace {
using aircannect::Memory::free;
constexpr size_t RING_BYTES = 16 * 1024;
constexpr size_t INTERNAL_RING_BYTES = 4096;
constexpr uint32_t CONSUMER_TIMEOUT_MS = 30000;
std::atomic<bool> busy{false};

bool valid_path(const char *path) {
    if (!path || path[0] != '/') return false;
    if (!path[1]) return true;
    const char *segment = path + 1;
    for (const char *p = segment;; p++) {
        if (*p && (uint8_t(*p) < 32 || *p == '\\' || *p == 127)) return false;
        if (!*p || *p == '/') {
            const size_t size = p - segment;
            if (!size || (size == 1 && segment[0] == '.') ||
                (size == 2 && segment[0] == '.' && segment[1] == '.')) return false;
            if (!*p) return true;
            segment = p + 1;
        }
    }
}

struct ZipEntry {
    char path[256];
    uint32_t size;
    uint32_t crc;
    uint32_t offset;
    uint32_t modified;
    bool directory;
};

class Job final : public Transfer {
public:
    Request request;
    Ready ready;
    SdStorage::Session session;
    uint8_t *ring = nullptr;
    size_t capacity = 0;
    std::atomic<uint32_t> produced{0}, consumed{0};
    std::atomic<bool> cancelled{false}, done{false}, error{false};
    ZipEntry *entries = nullptr;
    size_t entry_count = 0, entry_capacity = 0;

    ~Job() override {
        free(entries);
        free(ring);
        busy.store(false);
    }

    bool failed() const override { return error.load(); }
    bool finished() const override { return done.load() && produced.load() == consumed.load(); }
    void cancel() override { cancelled.store(true); }

    size_t read(uint8_t *out, size_t length) override {
        const uint32_t tail = consumed.load();
        length = std::min<size_t>(length, produced.load() - tail);
        const size_t first = std::min<size_t>(length, capacity - tail % capacity);
        memcpy(out, ring + tail % capacity, first);
        memcpy(out + first, ring, length - first);
        consumed.store(tail + length);
        return length;
    }

    bool emit(const uint8_t *data, size_t length) {
        uint32_t progress = millis();
        while (length) {
            if (cancelled.load() || !session.valid()) return false;
            const uint32_t head = produced.load();
            const size_t room = capacity - (head - consumed.load());
            if (!room) {
                if (uint32_t(millis() - progress) >= CONSUMER_TIMEOUT_MS) return false;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const size_t count = std::min(length, std::min<size_t>(room, capacity - head % capacity));
            memcpy(ring + head % capacity, data, count);
            produced.store(head + count);
            data += count;
            length -= count;
            progress = millis();
        }
        return true;
    }

    bool add(const char *path, uint64_t size, bool directory, int64_t modified = 0) {
        if (size > UINT32_MAX || entry_count == UINT16_MAX || strlen(path) >= 256)
            return false;
        if (entry_count == entry_capacity) {
            const size_t next = std::min<size_t>(UINT16_MAX, entry_capacity ? entry_capacity * 2 : 16);
            void *grown = aircannect::Memory::realloc_large(entries,
                next * sizeof(ZipEntry), next * sizeof(ZipEntry) <= 16384);
            if (!grown) return false;
            entries = static_cast<ZipEntry *>(grown);
            entry_capacity = next;
        }
        ZipEntry &entry = entries[entry_count++];
        entry = {};
        strcpy(entry.path, path);
        entry.size = size;
        entry.directory = directory;
        time_t stamp = modified;
        struct tm date = {};
        entry.modified = 33u << 16;
        if (modified > 0 && localtime_r(&stamp, &date) && date.tm_year >= 80 && date.tm_year <= 207)
            entry.modified = uint32_t(date.tm_year - 80) << 25 | uint32_t(date.tm_mon + 1) << 21 |
                uint32_t(date.tm_mday) << 16 | uint32_t(date.tm_hour) << 11 |
                uint32_t(date.tm_min) << 5 | uint32_t(date.tm_sec / 2);
        return true;
    }
};

bool list(Job &job, aircannect::LargeTextBuffer &json) {
    SdStorage::Reader dir;
    if (!dir.open(job.session, job.request.path, true)) return false;
    json = "{\"ok\":true,";
    aircannect::json_add_string(json, "path", job.request.path);
    json += "\"entries\":[";
    uint32_t seen = 0, count = 0;
    bool end = false;
    SdStorage::Reader::Entry entry;
    while (!end) {
        if (job.cancelled.load() || !dir.next(entry, end)) return false;
        if (end) break;
        if (seen++ < job.request.offset) continue;
        if (count == 32) break;
        if (count++) json += ',';
        json += '{';
        aircannect::json_add_string(json, "name", entry.name);
        aircannect::json_add_uint64(json, "size", entry.size);
        aircannect::json_add_uint64(json, "modified", entry.modified > 0 ? entry.modified : 0);
        aircannect::json_add_bool(json, "directory", entry.directory, false);
        json += '}';
    }
    json += "],";
    aircannect::json_add_bool(json, "more", !end, false);
    json += '}';
    return !json.overflowed();
}

bool collect(Job &job) {
    const char *selection = job.request.selection;
    if (!*selection) {
        SdStorage::Reader file;
        if (file.open(job.session, job.request.path))
            return job.add(job.request.path, file.size(), false, file.modified());
        if (!job.add(job.request.path, 0, true)) return false;
    } else {
        while (*selection) {
            const char *end = strchr(selection, '\n');
            const size_t length = end ? size_t(end - selection) : strlen(selection);
            if (!length || memchr(selection, '/', length)) return false;
            char path[256];
            const int written = snprintf(path, sizeof(path), "%s%s%.*s", job.request.path,
                strcmp(job.request.path, "/") ? "/" : "", int(length), selection);
            if (written < 0 || size_t(written) >= sizeof(path) || !valid_path(path)) return false;
            SdStorage::Reader file;
            const bool regular = file.open(job.session, path);
            if (!job.add(path, regular ? file.size() : 0, !regular,
                         regular ? file.modified() : 0)) return false;
            selection = end ? end + 1 : selection + length;
        }
    }
    // Breadth-first traversal holds one directory handle, not one per level.
    for (size_t i = 0; i < job.entry_count; i++) {
        if (!job.entries[i].directory) continue;
        char parent[256];
        strcpy(parent, job.entries[i].path);
        SdStorage::Reader dir;
        if (!dir.open(job.session, parent, true)) return false;
        bool end = false;
        SdStorage::Reader::Entry entry;
        while (!end) {
            if (job.cancelled.load() || !dir.next(entry, end)) return false;
            if (end) break;
            char path[256];
            const int written = snprintf(path, sizeof(path), "%s%s%s", parent,
                strcmp(parent, "/") ? "/" : "", entry.name);
            if (written < 0 || size_t(written) >= sizeof(path) ||
                !valid_path(path) || !job.add(path, entry.size, entry.directory, entry.modified)) return false;
        }
    }
    return true;
}

constexpr size_t COPY_BYTES = 1024;

uint64_t zip_data_size(uint32_t size) {
    return uint64_t(size) + 5 * (size ? (uint64_t(size) + COPY_BYTES - 1) / COPY_BYTES : 1);
}

bool copy_file(Job &job, SdStorage::Reader &file, uint32_t *checksum = nullptr) {
    uint8_t buffer[COPY_BYTES];
    uint64_t remaining = file.size();
    uint32_t crc = crc32_ieee_initial();
    do {
        const size_t count = std::min<uint64_t>(sizeof(buffer), remaining);
        if (checksum) {
            // Raw DEFLATE stored block: one pass, no compression workspace.
            uint8_t block[5];
            block[0] = remaining <= count ? 1 : 0;
            SdStorage::put_le16(block + 1, count);
            SdStorage::put_le16(block + 3, uint16_t(~count));
            if (!job.emit(block, sizeof(block))) return false;
        }
        if (file.read(buffer, count) != count || !job.emit(buffer, count)) return false;
        if (checksum) crc = crc32_ieee_update(crc, buffer, count);
        remaining -= count;
    } while (remaining);
    if (checksum) *checksum = crc32_ieee_finish(crc);
    return true;
}

bool archive(Job &job) {
    using SdStorage::put_le16;
    using SdStorage::put_le32;
    uint64_t position = 0;
    uint16_t files = 0;
    for (size_t i = 0; i < job.entry_count; i++) {
        ZipEntry &entry = job.entries[i];
        if (entry.directory) continue;
        const char *name = entry.path + 1;
        const size_t length = strlen(name);
        const uint64_t compressed_size = zip_data_size(entry.size);
        if (position + 30 + length + compressed_size + 16 > UINT32_MAX) return false;
        entry.offset = position;
        uint8_t header[30] = {};
        put_le32(header, 0x04034b50);
        put_le16(header + 4, 20);
        put_le16(header + 6, 0x0808); // UTF-8 names, trailing data descriptor.
        put_le16(header + 8, 8);
        put_le32(header + 10, entry.modified);
        put_le16(header + 26, length);
        if (!job.emit(header, sizeof(header)) ||
            !job.emit(reinterpret_cast<const uint8_t *>(name), length)) return false;
        SdStorage::Reader file;
        if (!file.open(job.session, entry.path) || file.size() != entry.size ||
            !copy_file(job, file, &entry.crc)) return false;
        uint8_t descriptor[16];
        put_le32(descriptor, 0x08074b50);
        put_le32(descriptor + 4, entry.crc);
        put_le32(descriptor + 8, compressed_size);
        put_le32(descriptor + 12, entry.size);
        if (!job.emit(descriptor, sizeof(descriptor))) return false;
        position += sizeof(header) + length + compressed_size + sizeof(descriptor);
        files++;
    }
    const uint32_t central_offset = position;
    for (size_t i = 0; i < job.entry_count; i++) {
        const ZipEntry &entry = job.entries[i];
        if (entry.directory) continue;
        const char *name = entry.path + 1;
        const size_t length = strlen(name);
        if (position + 46 + length + 22 > UINT32_MAX) return false;
        uint8_t header[46] = {};
        put_le32(header, 0x02014b50);
        put_le16(header + 4, 20);
        put_le16(header + 6, 20);
        put_le16(header + 8, 0x0808);
        put_le16(header + 10, 8);
        put_le32(header + 12, entry.modified);
        put_le32(header + 16, entry.crc);
        put_le32(header + 20, zip_data_size(entry.size));
        put_le32(header + 24, entry.size);
        put_le16(header + 28, length);
        put_le32(header + 42, entry.offset);
        if (!job.emit(header, sizeof(header)) ||
            !job.emit(reinterpret_cast<const uint8_t *>(name), length)) return false;
        position += sizeof(header) + length;
    }
    uint8_t end[22] = {};
    put_le32(end, 0x06054b50);
    put_le16(end + 8, files);
    put_le16(end + 10, files);
    put_le32(end + 12, position - central_offset);
    put_le32(end + 16, central_offset);
    return job.emit(end, sizeof(end));
}

void produce(void *context) {
    auto job = std::move(*static_cast<std::shared_ptr<Job> *>(context));
    delete static_cast<std::shared_ptr<Job> *>(context);
    bool success = false;
    if (!job->session.begin()) {
        job->ready(409, "storage_busy", nullptr, 0);
    } else if (job->request.kind == Kind::List) {
        aircannect::LargeTextBuffer json;
        if (list(*job, json)) {
            job->ready(200, nullptr, job, json.length());
            success = job->emit(reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
        } else job->ready(409, "listing_failed", nullptr, 0);
    } else if (job->request.kind == Kind::File) {
        SdStorage::Reader file;
        if (file.open(job->session, job->request.path)) {
            job->ready(200, nullptr, job, file.size());
            success = copy_file(*job, file);
        } else job->ready(409, "file_unavailable", nullptr, 0);
    } else if (collect(*job)) {
        job->ready(200, nullptr, job, 0);
        success = archive(*job);
    } else job->ready(409, "archive_unavailable", nullptr, 0);
    job->ready = nullptr;
    job->session.end();
    job->error.store(!success);
    job->done.store(true);
    job.reset();
    vTaskDeleteWithCaps(nullptr);
}
}  // namespace

StartResult start(const Request &request, Ready ready, std::weak_ptr<Transfer> &active) {
    if (!memchr(request.path, 0, sizeof(request.path)) || !valid_path(request.path) ||
        !memchr(request.selection, 0, sizeof(request.selection)))
        return StartResult::BadRequest;
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return StartResult::Busy;
    void *memory = aircannect::Memory::alloc_large(sizeof(Job));
    if (!memory) { busy.store(false); return StartResult::Unavailable; }
    auto job = std::shared_ptr<Job>(new(memory) Job, [](Job *value) {
        value->~Job();
        free(value);
    });
    job->request = request;
    active = job;
    job->ready = std::move(ready);
    job->capacity = RING_BYTES;
    job->ring = static_cast<uint8_t *>(aircannect::Memory::alloc_large(RING_BYTES, false));
    if (!job->ring) {
        job->capacity = INTERNAL_RING_BYTES;
        job->ring = static_cast<uint8_t *>(aircannect::Memory::alloc_large(INTERNAL_RING_BYTES));
    }
    if (!job->ring) return StartResult::Unavailable;
    auto *context = new(std::nothrow) std::shared_ptr<Job>(job);
    if (!context) return StartResult::Unavailable;
    BaseType_t created = pdFAIL;
    if (aircannect::Memory::psram_available())
        created = xTaskCreatePinnedToCoreWithCaps(produce, "sd_browser", 6144,
            context, 1, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS)
        created = xTaskCreatePinnedToCoreWithCaps(produce, "sd_browser", 6144,
            context, 1, nullptr, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) { delete context; return StartResult::Unavailable; }
    return StartResult::Started;
}
}  // namespace StorageBrowser
#endif
