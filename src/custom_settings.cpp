#include "custom_settings.h"

#include "custom_settings_protocol.h"
#include "debug_log.h"
#include "qframe.h"
#include "uart_arbiter.h"
#include "clinical_jobs.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace CustomSettings {
namespace {

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t FLAG_EDITABLE = 0x04;
constexpr uint8_t MAX_ENUM_OPTIONS = 32;
constexpr uint32_t RETRY_DELAY_MS = 5000;
constexpr uint16_t INVALID_OFFSET = 0xffff;

enum cache_state_t : uint8_t {
    CACHE_UNKNOWN,
    CACHE_READY,
    CACHE_UNSUPPORTED,
    CACHE_INCOMPATIBLE,
    CACHE_FAILED,
};

enum query_result_t : uint8_t {
    QUERY_OK,
    QUERY_ERROR,
    QUERY_TIMEOUT,
};

struct cached_option_t {
    uint16_t label_offset;
    uint8_t value;
};

struct cached_entry_t {
    uint32_t mop_mask;
    uint32_t minimum;
    uint32_t maximum;
    uint16_t label_offset;
    uint16_t units_offset;
    uint16_t option_start;
    int16_t scale;
    int16_t step;
    char name[4];
    uint8_t category;
    uint8_t flags;
    uint8_t width;
    uint8_t decimals;
    uint8_t option_count;
    kind_t kind;
};

SemaphoreHandle_t cache_mutex = nullptr;
cache_state_t cache_state = CACHE_UNKNOWN;
cached_entry_t *entries = nullptr;
cached_option_t *options = nullptr;
char *text_pool = nullptr;
uint8_t entry_count = 0;
uint16_t option_count = 0;
uint16_t option_capacity = 0;
uint16_t text_size = 0;
uint16_t text_capacity = 0;
uint32_t retry_at_ms = 0;
uint32_t cached_language = 0;
bool language_known = false;
uint32_t invalidation_generation = 1;
uint32_t loaded_generation = 0;

void *cache_alloc(size_t bytes, bool zero = false) {
    if (bytes == 0) return nullptr;
    void *ptr = zero
        ? heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = zero
            ? heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
            : heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

void clear_allocations_locked() {
    if (entries) heap_caps_free(entries);
    if (options) heap_caps_free(options);
    if (text_pool) heap_caps_free(text_pool);
    entries = nullptr;
    options = nullptr;
    text_pool = nullptr;
    entry_count = 0;
    option_count = 0;
    option_capacity = 0;
    text_size = 0;
    text_capacity = 0;
}

void reset_cache_locked(cache_state_t state) {
    clear_allocations_locked();
    cache_state = state;
    retry_at_ms = state == CACHE_FAILED ? millis() + RETRY_DELAY_MS : 0;
}

bool reserve_text_locked(uint16_t required) {
    if (required <= text_capacity) return true;
    uint32_t next = text_capacity ? text_capacity : 256;
    while (next < required) next *= 2;
    if (next > UINT16_MAX) next = UINT16_MAX;
    if (next < required) return false;

    char *replacement = static_cast<char *>(cache_alloc(next));
    if (!replacement) return false;
    if (text_pool && text_size) memcpy(replacement, text_pool, text_size);
    if (text_pool) heap_caps_free(text_pool);
    text_pool = replacement;
    text_capacity = (uint16_t)next;
    return true;
}

uint16_t add_text_locked(const char *text, size_t length) {
    if (!text || length == 0) return 0;
    if (length >= UINT16_MAX || text_size > UINT16_MAX - length - 1)
        return INVALID_OFFSET;
    uint16_t required = (uint16_t)(text_size + length + 1);
    if (!reserve_text_locked(required)) return INVALID_OFFSET;
    uint16_t offset = text_size;
    memcpy(text_pool + text_size, text, length);
    text_size += (uint16_t)length;
    text_pool[text_size++] = '\0';
    return offset;
}

bool reserve_options_locked(uint16_t required) {
    if (required <= option_capacity) return true;
    uint32_t next = option_capacity ? option_capacity * 2u : 16u;
    while (next < required) next *= 2;
    if (next > UINT16_MAX) return false;

    cached_option_t *replacement = static_cast<cached_option_t *>(
        cache_alloc(next * sizeof(cached_option_t)));
    if (!replacement) return false;
    if (options && option_count) {
        memcpy(replacement, options, option_count * sizeof(cached_option_t));
    }
    if (options) heap_caps_free(options);
    options = replacement;
    option_capacity = (uint16_t)next;
    return true;
}

const char *cached_text(uint16_t offset) {
    return text_pool && offset < text_size ? text_pool + offset : "";
}

query_result_t query_value(const char *command, char *response,
                           uint16_t response_size, const char *&value) {
    if (!command || !response || response_size < 2) return QUERY_TIMEOUT;
    uint16_t timeout = ClinicalJobs::timeout_ms();
    if (!timeout || loaded_generation != generation()) return QUERY_TIMEOUT;
    response[0] = '\0';
    uint16_t response_len = response_size;
    bool ok = Arbiter::send_cmd(command, CMD_SRC_TCP, CMD_PRIO_NORMAL,
                                response, &response_len, timeout);
    value = qframe_response_value(response);
    if (ok && value) return QUERY_OK;
    return value ? QUERY_ERROR : QUERY_TIMEOUT;
}

bool error_is(const char *value, const char *code) {
    return value && code && strcmp(value, code) == 0;
}

bool read_language(uint32_t &language) {
    char response[48];
    const char *value = nullptr;
    if (query_value("G S #LAN", response, sizeof(response), value) != QUERY_OK)
        return false;
    return CustomSettingsProtocol::parse_hex_value(value, language);
}

bool read_raw_locked(const cached_entry_t &entry, uint32_t &value) {
    char command[16];
    snprintf(command, sizeof(command), "G S #%s", entry.name);
    char response[64];
    const char *response_value = nullptr;
    return query_value(command, response, sizeof(response), response_value) == QUERY_OK &&
           CustomSettingsProtocol::parse_hex_value(response_value, value);
}

bool add_enum_option_locked(cached_entry_t &entry, uint8_t value,
                            const char *label, size_t label_len) {
    if (entry.option_count >= MAX_ENUM_OPTIONS ||
        !reserve_options_locked((uint16_t)(option_count + 1))) {
        return false;
    }
    uint16_t label_offset = add_text_locked(label, label_len);
    if (label_offset == INVALID_OFFSET) return false;
    options[option_count++] = {label_offset, value};
    entry.option_count++;
    return true;
}

query_result_t query_enum_option(const char *name, uint8_t index,
                                 char *response, uint16_t response_size,
                                 CustomSettingsProtocol::enum_option_t &option,
                                 const char *&error_value) {
    char command[24];
    snprintf(command, sizeof(command), "G C #%s %02X", name, index);
    const char *value = nullptr;
    query_result_t result = query_value(command, response, response_size, value);
    if (result == QUERY_OK) {
        if (!CustomSettingsProtocol::parse_enum_option(value, option))
            return QUERY_TIMEOUT;
        return QUERY_OK;
    }
    error_value = value;
    return result;
}

bool discover_enum_options_locked(cached_entry_t &entry) {
    entry.option_start = option_count;
    entry.option_count = 0;

    uint8_t total = 0;
    bool total_known = false;
    uint32_t current = 0;
    if (read_raw_locked(entry, current) && current <= 0xff) {
        char response[QFRAME_MAX_PAYLOAD + 1];
        CustomSettingsProtocol::enum_option_t option = {};
        const char *error_value = nullptr;
        query_result_t result = query_enum_option(
            entry.name, (uint8_t)current, response, sizeof(response), option,
            error_value);
        if (result == QUERY_OK) {
            total = option.count;
            total_known = true;
        } else if (result != QUERY_ERROR || !error_is(error_value, "6033")) {
            return false;
        }
    }

    if (!total_known) {
        for (uint8_t index = 0; index < MAX_ENUM_OPTIONS; index++) {
            char response[QFRAME_MAX_PAYLOAD + 1];
            CustomSettingsProtocol::enum_option_t option = {};
            const char *error_value = nullptr;
            query_result_t result = query_enum_option(
                entry.name, index, response, sizeof(response), option,
                error_value);
            if (result == QUERY_OK) {
                total = option.count;
                total_known = true;
                break;
            }
            if (result != QUERY_ERROR || !error_is(error_value, "6033"))
                return false;
        }
    }

    if (!total_known) return true;
    if (total > MAX_ENUM_OPTIONS) return false;

    for (uint8_t index = 0; index < total; index++) {
        char response[QFRAME_MAX_PAYLOAD + 1];
        CustomSettingsProtocol::enum_option_t option = {};
        const char *error_value = nullptr;
        query_result_t result = query_enum_option(
            entry.name, index, response, sizeof(response), option, error_value);
        if (result == QUERY_ERROR && error_is(error_value, "6033")) continue;
        if (result != QUERY_OK || option.count != total ||
            !add_enum_option_locked(entry, index, option.label, option.label_len)) {
            return false;
        }
    }
    return true;
}

bool discover_locked() {
    char response[QFRAME_MAX_PAYLOAD + 1];
    const char *value = nullptr;
    query_result_t result = query_value("G C &CSG", response, sizeof(response), value);
    if (result == QUERY_ERROR && error_is(value, "6009")) {
        reset_cache_locked(CACHE_UNSUPPORTED);
        Log::logf(CAT_WEB, LOG_DEBUG,
                  "[SETTINGS] Airbreak custom registry unavailable\n");
        return true;
    }
    if (result != QUERY_OK) return false;

    CustomSettingsProtocol::header_t header = {};
    if (!CustomSettingsProtocol::parse_header(value, header)) return false;
    if (header.version != PROTOCOL_VERSION) {
        reset_cache_locked(CACHE_INCOMPATIBLE);
        Log::logf(CAT_WEB, LOG_WARN,
                  "[SETTINGS] Unsupported custom registry version %u\n",
                  header.version);
        return true;
    }

    if (header.count == 0) {
        reset_cache_locked(CACHE_READY);
        return true;
    }

    entries = static_cast<cached_entry_t *>(
        cache_alloc(header.count * sizeof(cached_entry_t), true));
    if (!entries) return false;
    entry_count = header.count;
    uint32_t initial_text = 1u + (uint32_t)header.count * 64u;
    if (initial_text > UINT16_MAX) initial_text = UINT16_MAX;
    if (!reserve_text_locked((uint16_t)initial_text)) return false;
    text_pool[0] = '\0';
    text_size = 1;

    for (uint16_t index = 0; index < entry_count; index++) {
        char command[20];
        snprintf(command, sizeof(command), "G C &CSG %02X", index);
        if (query_value(command, response, sizeof(response), value) != QUERY_OK)
            return false;

        CustomSettingsProtocol::entry_t record = {};
        if (!CustomSettingsProtocol::parse_entry(value, record)) return false;
        for (uint16_t previous = 0; previous < index; previous++) {
            if (strcmp(entries[previous].name, record.name) == 0) return false;
        }

        cached_entry_t &entry = entries[index];
        memcpy(entry.name, record.name, sizeof(entry.name));
        entry.kind = record.kind == CustomSettingsProtocol::ENTRY_NUMERIC
            ? KIND_NUMERIC : KIND_ENUM;
        entry.category = record.category;
        entry.mop_mask = record.mop_mask;
        entry.scale = record.scale;
        entry.step = record.step;
        entry.decimals = record.decimals;
        entry.label_offset = add_text_locked(record.label, record.label_len);
        entry.units_offset = add_text_locked(record.units, record.units_len);
        if (entry.label_offset == INVALID_OFFSET ||
            entry.units_offset == INVALID_OFFSET) {
            return false;
        }

        snprintf(command, sizeof(command), "G C #%s", entry.name);
        if (query_value(command, response, sizeof(response), value) != QUERY_OK)
            return false;

        if (entry.kind == KIND_NUMERIC) {
            CustomSettingsProtocol::numeric_caps_t caps = {};
            if (!CustomSettingsProtocol::parse_numeric_caps(value, caps))
                return false;
            entry.flags = caps.flags;
            entry.width = caps.width;
            entry.minimum = caps.minimum;
            entry.maximum = caps.maximum;
        } else {
            if (!CustomSettingsProtocol::parse_enum_flags(value, entry.flags))
                return false;
            entry.width = 4;
            if (!discover_enum_options_locked(entry)) return false;
        }
    }

    cache_state = CACHE_READY;
    Log::logf(CAT_WEB, LOG_INFO,
              "[SETTINGS] Cached %u custom settings (%u options, %u text bytes)\n",
              entry_count, option_count, text_size);
    return true;
}

cached_entry_t *find_entry_locked(const char *name) {
    if (!name || cache_state != CACHE_READY) return nullptr;
    for (uint16_t i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    }
    return nullptr;
}

bool ensure_loaded_locked() {
    uint32_t current = generation();
    if (loaded_generation != current) {
        reset_cache_locked(CACHE_UNKNOWN);
        language_known = false;
        loaded_generation = current;
    }
    if (cache_state == CACHE_READY) {
        uint32_t language = 0;
        if (read_language(language)) {
            if (language_known && language != cached_language) {
                Log::logf(CAT_WEB, LOG_INFO,
                          "[SETTINGS] LAN changed, refreshing custom registry\n");
                reset_cache_locked(CACHE_UNKNOWN);
            }
            cached_language = language;
            language_known = true;
        }
        if (cache_state == CACHE_READY) return true;
    } else if (cache_state == CACHE_UNSUPPORTED ||
               cache_state == CACHE_INCOMPATIBLE) {
        return true;
    } else if (cache_state == CACHE_FAILED &&
               (int32_t)(millis() - retry_at_ms) < 0) {
        return false;
    }

    reset_cache_locked(CACHE_UNKNOWN);
    uint32_t language = 0;
    language_known = read_language(language);
    if (language_known) cached_language = language;

    if (!discover_locked()) {
        reset_cache_locked(CACHE_FAILED);
        Log::logf(CAT_WEB, LOG_WARN,
                  "[SETTINGS] Custom registry discovery failed\n");
        return false;
    }
    return true;
}

}  // namespace

void init() {
    if (!cache_mutex) cache_mutex = xSemaphoreCreateMutex();
}

bool ensure_loaded() {
    if (!cache_mutex) init();
    if (!cache_mutex) return false;
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    bool result = ensure_loaded_locked();
    xSemaphoreGive(cache_mutex);
    return result;
}

bool contains(const char *name) {
    if (!cache_mutex) return false;
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    bool result = find_entry_locked(name) != nullptr;
    xSemaphoreGive(cache_mutex);
    return result;
}

void visit_category(uint8_t category, uint8_t mop, entry_visitor_t visitor,
                    void *context) {
    if (!cache_mutex || !visitor || mop >= 32) return;
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    if (cache_state != CACHE_READY) {
        xSemaphoreGive(cache_mutex);
        return;
    }

    for (uint16_t i = 0; i < entry_count; i++) {
        cached_entry_t &cached = entries[i];
        if (cached.category != category ||
            (cached.mop_mask & (1u << mop)) == 0) {
            continue;
        }

        option_view_t option_views[MAX_ENUM_OPTIONS];
        for (uint8_t option = 0; option < cached.option_count; option++) {
            const cached_option_t &stored = options[cached.option_start + option];
            option_views[option] = {stored.value, cached_text(stored.label_offset)};
        }

        entry_view_t view = {};
        view.kind = cached.kind;
        view.category = cached.category;
        view.mop_mask = cached.mop_mask;
        view.name = cached.name;
        view.label = cached_text(cached.label_offset);
        view.units = cached_text(cached.units_offset);
        view.flags = cached.flags;
        view.width = cached.width;
        view.scale = cached.scale;
        view.step = cached.step;
        view.decimals = cached.decimals;
        view.minimum = cached.minimum;
        view.maximum = cached.maximum;
        view.options = option_views;
        view.option_count = cached.option_count;

        uint32_t value = 0;
        bool value_ok = read_raw_locked(cached, value);
        visitor(view, value_ok, value, context);
    }
    xSemaphoreGive(cache_mutex);
}

bool write_raw(const char *name, uint32_t value) {
    if (!cache_mutex) return false;
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    cached_entry_t *entry = find_entry_locked(name);
    if (!entry || (entry->flags & FLAG_EDITABLE) == 0) {
        xSemaphoreGive(cache_mutex);
        return false;
    }

    bool valid = true;
    if (entry->kind == KIND_NUMERIC) {
        valid = value >= entry->minimum && value <= entry->maximum;
    } else {
        valid = false;
        for (uint8_t i = 0; i < entry->option_count; i++) {
            if (options[entry->option_start + i].value == value) {
                valid = true;
                break;
            }
        }
    }

    bool result = false;
    if (valid) {
        char command[32];
        snprintf(command, sizeof(command), "P S #%s %0*lX", entry->name,
                 entry->width, (unsigned long)value);
        char response[64];
        const char *response_value = nullptr;
        result = query_value(command, response, sizeof(response), response_value) ==
                 QUERY_OK;
    }
    xSemaphoreGive(cache_mutex);
    return result;
}

void invalidate(const char *reason) {
    __atomic_add_fetch(&invalidation_generation, 1, __ATOMIC_ACQ_REL);
    Log::logf(CAT_WEB, LOG_INFO, "[SETTINGS] Cache invalidated (%s)\n",
              reason ? reason : "unknown");
}

uint32_t generation() {
    return __atomic_load_n(&invalidation_generation, __ATOMIC_ACQUIRE);
}

}  // namespace CustomSettings
