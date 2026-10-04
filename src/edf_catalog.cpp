#include "edf_catalog.h"

#include "board.h"

#if AB_STORAGE_HAS_SDCARD

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

#include "crc.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "sd_storage.h"

namespace EdfCatalog {
namespace {

constexpr const char *CATALOG_DIR = "/airbridge";
constexpr const char *CATALOG_PATH = "/airbridge/catalog.bin";
constexpr const char *CATALOG_PART = "/airbridge/catalog.bin.part";
constexpr const char *CATALOG_BACKUP = "/airbridge/catalog.bin.bak";
constexpr uint16_t CATALOG_VERSION = 1;
constexpr uint16_t HEADER_SIZE = 24;
constexpr uint16_t ENTRY_SIZE = 32;
constexpr uint32_t MAX_ENTRIES = 4096;

struct Header {
    uint32_t count;
    uint32_t generation;
    uint32_t entries_crc;
};

static Status status = {true};
static uint32_t status_revision = 0;
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t changed_indexes[32] = {};
static uint32_t change_count = 0;
static SemaphoreHandle_t mutex = nullptr;
static fs::FS *storage = nullptr;

static void set_error(const char *message) {
    portENTER_CRITICAL(&status_mux);
    status.ready = false;
    strncpy(status.error, message ? message : "catalog error",
            sizeof(status.error) - 1);
    status.error[sizeof(status.error) - 1] = 0;
    __atomic_add_fetch(&status_revision, 1, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_EDF, LOG_ERROR, "catalog: %s\n",
              message ? message : "catalog error");
}

static bool digits_only(const char *text, size_t size) {
    if (!text) return false;
    for (size_t i = 0; i < size; i++)
        if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

static bool valid_digits(const char *text, size_t size) {
    return digits_only(text, size) && text[size] == 0;
}

static bool valid_entry(const Entry &entry) {
    return valid_digits(entry.therapy_day, 8) &&
           strlen(entry.file_prefix) == 15 &&
           digits_only(entry.file_prefix, 8) &&
           entry.file_prefix[8] == '_' &&
           valid_digits(entry.file_prefix + 9, 6);
}

static void encode_header(const Header &header, uint8_t *out) {
    memcpy(out, "ABEC", 4);
    SdStorage::put_le16(out + 4, CATALOG_VERSION);
    SdStorage::put_le16(out + 6, HEADER_SIZE);
    SdStorage::put_le16(out + 8, ENTRY_SIZE);
    SdStorage::put_le16(out + 10, 0);
    SdStorage::put_le32(out + 12, header.count);
    SdStorage::put_le32(out + 16, header.generation);
    SdStorage::put_le32(out + HEADER_SIZE - 4, header.entries_crc);
}

static bool decode_header(const uint8_t *data, Header &header) {
    if (memcmp(data, "ABEC", 4) != 0 ||
        SdStorage::get_le16(data + 4) != CATALOG_VERSION ||
        SdStorage::get_le16(data + 6) != HEADER_SIZE ||
        SdStorage::get_le16(data + 8) != ENTRY_SIZE) {
        return false;
    }
    header.count = SdStorage::get_le32(data + 12);
    header.generation = SdStorage::get_le32(data + 16);
    header.entries_crc = SdStorage::get_le32(data + HEADER_SIZE - 4);
    return header.count <= MAX_ENTRIES;
}

static void encode_entry(const Entry &entry, uint8_t *out) {
    memset(out, 0, ENTRY_SIZE);
    memcpy(out, entry.therapy_day, 8);
    memcpy(out + 8, entry.file_prefix, 15);
    out[23] = entry.flags;
    SdStorage::put_le32(out + 24, entry.finalized_epoch);
    SdStorage::put_le32(out + 28, entry.str_revision);
}

static bool decode_entry(const uint8_t *data, Entry &entry) {
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.therapy_day, data, 8);
    memcpy(entry.file_prefix, data + 8, 15);
    entry.flags = data[23];
    entry.finalized_epoch = SdStorage::get_le32(data + 24);
    entry.str_revision = SdStorage::get_le32(data + 28);
    return valid_entry(entry);
}

static bool read_header(fs::File &file, Header &header) {
    uint8_t bytes[HEADER_SIZE];
    if (!file || file.size() < HEADER_SIZE ||
        file.read(bytes, sizeof(bytes)) != sizeof(bytes) ||
        !decode_header(bytes, header) ||
        file.size() != HEADER_SIZE + header.count * ENTRY_SIZE) {
        return false;
    }

    return true;
}

static bool read_valid_header(fs::File &file, Header &header) {
    if (!read_header(file, header)) return false;
    uint8_t entry[ENTRY_SIZE];
    uint32_t crc = crc32_ieee_initial();
    for (uint32_t i = 0; i < header.count; i++) {
        Entry decoded;
        if (file.read(entry, sizeof(entry)) != sizeof(entry) ||
            !decode_entry(entry, decoded)) {
            return false;
        }
        crc = crc32_ieee_update(crc, entry, sizeof(entry));
    }
    return crc32_ieee_finish(crc) == header.entries_crc;
}

static bool write_empty_catalog() {
    Header header = {0, 1, crc32_ieee(nullptr, 0)};
    uint8_t bytes[HEADER_SIZE];
    encode_header(header, bytes);
    fs::File file = storage->open(CATALOG_PATH, FILE_WRITE);
    if (!file || !SdStorage::write_exact(file, bytes, sizeof(bytes))) {
        if (file) file.close();
        return false;
    }
    file.flush();
    file.close();
    portENTER_CRITICAL(&status_mux);
    status.entries = 0;
    status.generation = header.generation;
    portEXIT_CRITICAL(&status_mux);
    return true;
}

static bool publish_catalog() {
    if (storage->exists(CATALOG_BACKUP) &&
        !storage->remove(CATALOG_BACKUP)) {
        return false;
    }
    if (storage->exists(CATALOG_PATH) &&
        !storage->rename(CATALOG_PATH, CATALOG_BACKUP)) {
        return false;
    }
    if (!storage->rename(CATALOG_PART, CATALOG_PATH)) {
        (void)storage->rename(CATALOG_BACKUP, CATALOG_PATH);
        return false;
    }
    if (storage->exists(CATALOG_BACKUP)) storage->remove(CATALOG_BACKUP);
    return true;
}

static void recover_catalog() {
    if (!storage->exists(CATALOG_PATH) && storage->exists(CATALOG_BACKUP)) {
        if (!storage->rename(CATALOG_BACKUP, CATALOG_PATH)) {
            set_error("backup recovery failed");
            return;
        }
    } else if (storage->exists(CATALOG_PATH) &&
               storage->exists(CATALOG_BACKUP)) {
        storage->remove(CATALOG_BACKUP);
    }
    if (storage->exists(CATALOG_PART)) storage->remove(CATALOG_PART);
}

}  // namespace

void init() {
    if (status.ready) return;
    portENTER_CRITICAL(&status_mux);
    status.error[0] = 0;
    __atomic_add_fetch(&status_revision, 1, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&status_mux);
    storage = SdStorage::filesystem();
    if (!storage) return;
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        set_error("mutex allocation failed");
        return;
    }
    if (!storage->exists(CATALOG_DIR) && !storage->mkdir(CATALOG_DIR)) {
        set_error("directory creation failed");
        return;
    }
    recover_catalog();
    if (status.error[0]) return;
    if (!storage->exists(CATALOG_PATH) && !write_empty_catalog()) {
        set_error("initialization failed");
        return;
    }

    fs::File file = storage->open(CATALOG_PATH, FILE_READ);
    Header header;
    if (!read_valid_header(file, header)) {
        if (file) file.close();
        set_error("validation failed");
        return;
    }
    file.close();
    portENTER_CRITICAL(&status_mux);
    status.ready = true;
    change_count = 0;
    status.entries = header.count;
    status.generation = header.generation;
    status.error[0] = 0;
    __atomic_add_fetch(&status_revision, 1, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_EDF, LOG_INFO,
              "catalog ready entries=%u generation=%u\n",
              status.entries, status.generation);
}

bool commit(const Entry &entry) {
    if (!status.ready || !mutex || !valid_entry(entry) ||
        xSemaphoreTake(mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return false;
    }

    fs::File input = storage->open(CATALOG_PATH, FILE_READ);
    Header old_header = {};
    bool success = read_valid_header(input, old_header) && input.seek(HEADER_SIZE);
    bool found = false;
    uint32_t changed_index = old_header.count;
    uint8_t bytes[ENTRY_SIZE];
    if (success) {
        for (uint32_t i = 0; i < old_header.count; i++) {
            Entry decoded;
            if (input.read(bytes, sizeof(bytes)) != sizeof(bytes) ||
                !decode_entry(bytes, decoded)) {
                success = false;
                break;
            }
            if (strcmp(decoded.file_prefix, entry.file_prefix) == 0) {
                found = true;
                changed_index = i;
                break;
            }
        }
    }
    if (success && !found && old_header.count >= MAX_ENTRIES) success = false;
    if (success) success = input.seek(HEADER_SIZE);

    storage->remove(CATALOG_PART);
    fs::File output = success ? storage->open(CATALOG_PART, FILE_WRITE)
                              : fs::File();
    Header new_header = {
        old_header.count + (found ? 0u : 1u),
        old_header.generation + 1,
        0,
    };
    uint8_t header_bytes[HEADER_SIZE] = {};
    if (!output || !SdStorage::write_exact(output, header_bytes, sizeof(header_bytes)))
        success = false;

    uint32_t crc = crc32_ieee_initial();
    for (uint32_t i = 0; success && i < old_header.count; i++) {
        Entry decoded;
        if (input.read(bytes, sizeof(bytes)) != sizeof(bytes) ||
            !decode_entry(bytes, decoded)) {
            success = false;
            break;
        }
        if (strcmp(decoded.file_prefix, entry.file_prefix) == 0)
            encode_entry(entry, bytes);
        if (!SdStorage::write_exact(output, bytes, sizeof(bytes))) {
            success = false;
            break;
        }
        crc = crc32_ieee_update(crc, bytes, sizeof(bytes));
    }
    if (success && !found) {
        encode_entry(entry, bytes);
        success = SdStorage::write_exact(output, bytes, sizeof(bytes));
        if (success) crc = crc32_ieee_update(crc, bytes, sizeof(bytes));
    }
    new_header.entries_crc = crc32_ieee_finish(crc);
    encode_header(new_header, header_bytes);
    if (success && (!output.seek(0) ||
                    !SdStorage::write_exact(output, header_bytes, sizeof(header_bytes)))) {
        success = false;
    }
    if (output) {
        output.flush();
        output.close();
    }
    if (input) input.close();
    if (success) success = publish_catalog();
    if (!success) storage->remove(CATALOG_PART);

    if (success) {
        portENTER_CRITICAL(&status_mux);
        status.entries = new_header.count;
        status.generation = new_header.generation;
        changed_indexes[status.generation % 32] = changed_index;
        if (change_count < 32) ++change_count;
        __atomic_add_fetch(&status_revision, 1, __ATOMIC_RELEASE);
        portEXIT_CRITICAL(&status_mux);
        Log::logf(CAT_EDF, LOG_INFO,
                  "catalog commit %s entries=%u generation=%u\n",
                  entry.file_prefix, status.entries, status.generation);
    }
    xSemaphoreGive(mutex);
    return success;
}

bool read(uint32_t index, Entry &entry) {
    if (!status.ready || !mutex ||
        xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    if (index >= status.entries) {
        xSemaphoreGive(mutex);
        return false;
    }
    fs::File file = storage->open(CATALOG_PATH, FILE_READ);
    const size_t offset = HEADER_SIZE + static_cast<size_t>(index) * ENTRY_SIZE;
    uint8_t bytes[ENTRY_SIZE];
    const bool success = file && file.seek(offset) &&
                         file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
                         decode_entry(bytes, entry);
    if (file) file.close();
    xSemaphoreGive(mutex);
    return success;
}

bool find(const char *file_prefix, Entry &entry) {
    if (!status.ready || !mutex || !file_prefix ||
        strlen(file_prefix) != 15 ||
        xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    fs::File file = storage->open(CATALOG_PATH, FILE_READ);
    Header header = {};
    bool found = false;
    uint8_t bytes[ENTRY_SIZE];
    if (read_valid_header(file, header) && file.seek(HEADER_SIZE)) {
        for (uint32_t i = 0; i < header.count; i++) {
            Entry decoded;
            if (file.read(bytes, sizeof(bytes)) != sizeof(bytes) ||
                !decode_entry(bytes, decoded)) {
                break;
            }
            if (strcmp(decoded.file_prefix, file_prefix) == 0) {
                entry = decoded;
                found = true;
                break;
            }
        }
    }
    if (file) file.close();
    xSemaphoreGive(mutex);
    return found;
}

bool snapshot_prefixes(char *out, size_t out_size, uint32_t &count) {
    count = 0;
    if (!status.ready || !mutex ||
        xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    fs::File file = storage->open(CATALOG_PATH, FILE_READ);
    Header header = {};
    bool success = read_valid_header(file, header) &&
                   header.count <= out_size / 16 &&
                   (header.count == 0 || out) && file.seek(HEADER_SIZE);
    uint8_t bytes[ENTRY_SIZE];
    for (uint32_t i = 0; success && i < header.count; i++) {
        Entry decoded;
        if (file.read(bytes, sizeof(bytes)) != sizeof(bytes) ||
            !decode_entry(bytes, decoded)) {
            success = false;
            break;
        }
        memcpy(out + static_cast<size_t>(i) * 16,
               decoded.file_prefix, 16);
    }
    if (success) count = header.count;
    if (file) file.close();
    xSemaphoreGive(mutex);
    return success;
}

bool snapshot_day(const char *day, Entry *&out, uint32_t &count) {
    out = nullptr;
    count = 0;
    if (!status.ready || !mutex || !day || !valid_digits(day, 8) ||
        xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    fs::File file = storage->open(CATALOG_PATH, FILE_READ);
    Header header = {};
    bool success = read_header(file, header);
    uint32_t capacity = 0;
    uint32_t crc = crc32_ieee_initial();
    uint8_t bytes[ENTRY_SIZE];
    for (uint32_t i = 0; success && i < header.count; i++) {
        Entry entry;
        success = file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
                  decode_entry(bytes, entry);
        if (!success) break;
        crc = crc32_ieee_update(crc, bytes, sizeof(bytes));
        if (strcmp(entry.therapy_day, day)) continue;
        if (count == capacity) {
            const uint32_t next = capacity ? capacity * 2 : 4;
            // A typical day needs only a few entries; large days require PSRAM.
            void *grown = aircannect::Memory::realloc_large(
                out, next * sizeof(Entry), next <= 32);
            if (!grown) { success = false; break; }
            out = static_cast<Entry *>(grown);
            capacity = next;
        }
        out[count++] = entry;
    }
    success = success && crc32_ieee_finish(crc) == header.entries_crc;
    if (file) file.close();
    xSemaphoreGive(mutex);
    if (!success) {
        aircannect::Memory::free(out);
        out = nullptr;
        count = 0;
    }
    return success;
}

void get_status(Status &out) {
    portENTER_CRITICAL(&status_mux);
    out = status;
    portEXIT_CRITICAL(&status_mux);
}

uint32_t revision() {
    return __atomic_load_n(&status_revision, __ATOMIC_ACQUIRE);
}

bool changes_since(uint32_t generation, Changes &out) {
    portENTER_CRITICAL(&status_mux);
    out.catalog = status;
    const uint32_t distance = status.generation - generation;
    const bool complete = status.ready && distance <= change_count;
    out.count = complete ? distance : 0;
    for (uint32_t i = 0; i < out.count; ++i)
        out.indexes[i] = changed_indexes[(generation + i + 1) % 32];
    portEXIT_CRITICAL(&status_mux);
    return complete;
}

}  // namespace EdfCatalog

#else

namespace EdfCatalog {

void init() {}
uint32_t revision() { return 0; }
bool commit(const Entry &) { return false; }
bool changes_since(uint32_t, Changes &out) { out = {}; return false; }
bool read(uint32_t, Entry &) { return false; }
bool find(const char *, Entry &) { return false; }
bool snapshot_prefixes(char *, size_t, uint32_t &count) {
    count = 0;
    return false;
}
bool snapshot_day(const char *, Entry *&out, uint32_t &count) {
    out = nullptr;
    count = 0;
    return false;
}

void get_status(Status &out) {
    out = {};
    out.supported = false;
}

}  // namespace EdfCatalog

#endif
