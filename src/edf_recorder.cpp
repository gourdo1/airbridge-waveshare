#include "edf_recorder.h"

#include "board.h"

#if AB_STORAGE_HAS_SDCARD

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "air10_edf.h"
#include "air10_stored.h"
#include "air10_str_timeline.h"
#include "air10_clock.h"
#include "crc.h"
#include "debug_log.h"
#include "edf_catalog.h"
#include "export_sync.h"
#include "live_stream.h"
#include "oxi_arbiter.h"
#include "qframe.h"
#include "sd_storage.h"
#include "uart_arbiter.h"
#include "wifi.h"

namespace EdfRecorder {
namespace {

constexpr uint16_t RAW_QUEUE_CAPACITY_PSRAM = 128;
constexpr uint16_t RAW_QUEUE_CAPACITY_FALLBACK = 48;
constexpr uint16_t RAW_PAYLOAD_MAX = 64;
constexpr uint8_t STREAM_FIELD_MAX = 8;
constexpr uint8_t STREAM_COUNT = 4;
constexpr uint16_t RECORDER_STACK = 8192;
constexpr uint16_t POLL_TIMEOUT_MS = 120;
constexpr uint16_t STORED_TIMEOUT_MS = 2000;
constexpr uint8_t STORED_TRANSFER_ATTEMPTS = 3;
constexpr uint16_t STR_GENERATION_POLL_MS = 100;
constexpr uint16_t MASK_OFF_TOLERANCE_MINUTES = 1;
constexpr uint32_t STR_FINAL_SAVE_TIMEOUT_MS = 60000;
constexpr uint32_t CLOCK_VALID_AFTER = 1700000000UL;
constexpr int16_t EDF_MISSING = -1;
constexpr size_t RECOVERY_HEADER_MAX = 8192;
constexpr size_t RECOVERY_BUFFER_SIZE = 4096;
constexpr size_t IDENTIFICATION_BUFFER_SIZE = 1024;

const char *const MONTH_NAMES[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

struct RawFrame {
    uint32_t captured_ms;
    uint16_t len;
    uint8_t payload[RAW_PAYLOAD_MAX];
};

enum class ControlKind : uint8_t {
    Start,
    Stop,
};

struct ControlEvent {
    ControlKind kind;
    uint32_t epoch;
    uint32_t captured_ms;
    bool trusted_time;
};

struct StreamField {
    char name[4];
    uint8_t width;
};

struct StreamSchema {
    char tag[4];
    StreamField fields[STREAM_FIELD_MAX];
    uint8_t field_count;
    bool from_firmware;
};

struct DecodedFrame {
    uint8_t sequence;
    int32_t values[STREAM_FIELD_MAX];
};

struct OutputFile {
    fs::File file;
    const Air10Edf::Schema *schema;
    uint32_t records;
    uint32_t rest_crc;
    bool created;
    bool failed;
    bool open;
    char partial_path[128];
    char final_path[128];
    char crc_partial_path[128];
    char crc_final_path[128];
};

struct Accumulator {
    OutputFile output;
    int16_t *samples;
    size_t sample_count;
    uint32_t current_record;
    bool initialized;
};

struct WaveClock {
    bool initialized;
    uint8_t last_sequence;
    uint32_t relative_ms;
};

static Status status = {true};
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t raw_queue = nullptr;
static QueueHandle_t control_queue = nullptr;
static StaticQueue_t raw_queue_state;
static uint8_t *raw_queue_storage = nullptr;
static uint16_t raw_queue_capacity = 0;
static TaskHandle_t recorder_task_handle = nullptr;
static uart_frame_listener_t frame_listener = -1;
static volatile bool capture_active = false;
static volatile bool therapy_start_pending = false;

static fs::FS *storage = nullptr;
static StreamSchema stream_schemas[STREAM_COUNT];
static LiveStream::internal_handle_t stream_leases[STREAM_COUNT];
static char wave_tag[4] = "TCE";

static Accumulator brp;
static Accumulator pld;
static Accumulator sad;
static OutputFile eve;
static OutputFile csl;
static uint8_t *header_buffer = nullptr;
static size_t header_capacity = 0;
static uint8_t *record_buffer = nullptr;
static size_t record_capacity = 0;

static uint32_t session_start_capture_ms = 0;
static Air10Clock::Anchor session_clock;
static uint16_t session_native_day = 0;
static char recording_id[81] = {};
static char start_date[9] = {};
static char start_time[9] = {};
static char session_directory[40] = {};
static uint16_t session_epoch_day = 0;
static uint16_t session_mid = 0;
static uint16_t session_vid = 0;
static char session_srn[24] = {};

static WaveClock wave_clock = {};
static uint32_t last_pld_slot = UINT32_MAX;
static uint32_t last_oxi_slot = UINT32_MAX;

static void status_error(const char *message) {
    portENTER_CRITICAL(&status_mux);
    status.write_errors++;
    strncpy(status.last_error, message ? message : "error",
            sizeof(status.last_error) - 1);
    status.last_error[sizeof(status.last_error) - 1] = 0;
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_GENERAL, LOG_ERROR, "[EDF] %s\n",
              message ? message : "error");
}

static void post_error(const char *message) {
    portENTER_CRITICAL(&status_mux);
    status.post_errors++;
    strncpy(status.last_error, message ? message : "post-processing error",
            sizeof(status.last_error) - 1);
    status.last_error[sizeof(status.last_error) - 1] = 0;
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_GENERAL, LOG_ERROR, "[EDF] %s\n",
              message ? message : "post-processing error");
}

static bool post_processing_cancelled() {
    return __atomic_load_n(&therapy_start_pending, __ATOMIC_ACQUIRE);
}

static bool post_processing_delay(uint32_t delay_ms) {
    const uint32_t started = millis();
    while (static_cast<uint32_t>(millis() - started) < delay_ms) {
        if (post_processing_cancelled()) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !post_processing_cancelled();
}

static void *allocate_large(size_t bytes) {
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return ptr;
}

static int hex_nibble_local(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_hex_value(const char *text, size_t width, uint32_t &value) {
    value = 0;
    if (!text || width == 0 || width > 8) return false;
    for (size_t i = 0; i < width; i++) {
        int nibble = hex_nibble_local(text[i]);
        if (nibble < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(nibble);
    }
    return true;
}

static bool parse_decimal_field(const uint8_t *text, size_t width,
                                uint32_t &value) {
    value = 0;
    bool found_digit = false;
    for (size_t i = 0; i < width; i++) {
        if (text[i] == ' ') continue;
        if (text[i] < '0' || text[i] > '9') return false;
        const uint32_t digit = text[i] - '0';
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        found_digit = true;
    }
    return found_digit;
}

static bool has_suffix(const char *text, const char *suffix) {
    if (!text || !suffix) return false;
    const size_t text_len = strlen(text);
    const size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcmp(text + text_len - suffix_len, suffix) == 0;
}

static bool command_value(const char *command, char *out, size_t capacity,
                          uint16_t timeout_ms = POLL_TIMEOUT_MS) {
    if (!command || !out || capacity < 2) return false;
    char response[160] = {};
    uint16_t response_len = sizeof(response);
    if (!Arbiter::send_cmd(command, CMD_SRC_INTERNAL, CMD_PRIO_LOW,
                           response, &response_len, timeout_ms)) {
        return false;
    }
    const char *value = qframe_response_value(response);
    if (!value) return false;
    strncpy(out, value, capacity - 1);
    out[capacity - 1] = 0;
    return true;
}

static bool read_variable(const char *name, char *out, size_t capacity,
                          uint16_t timeout_ms = POLL_TIMEOUT_MS) {
    char command[16];
    snprintf(command, sizeof(command), "G S #%s", name);
    return command_value(command, out, capacity, timeout_ms);
}

struct StoredCapture {
    uint8_t payload[64];
    uint16_t payload_len;
    bool received;
};

static bool stored_value_sink(const qframe_t *frame, void *context) {
    StoredCapture *capture = static_cast<StoredCapture *>(context);
    if (!frame || !capture || frame->type != QFRAME_TYPE_K ||
        frame->payload_len > sizeof(capture->payload)) {
        return frame && frame->type != QFRAME_TYPE_K;
    }
    memcpy(capture->payload, frame->payload, frame->payload_len);
    capture->payload_len = frame->payload_len;
    capture->received = true;
    return true;
}

static bool read_stored_value(const char *tag, uint16_t epoch_day,
                              Air10Stored::Value &value,
                              uint8_t attempts = STORED_TRANSFER_ATTEMPTS) {
    if (!attempts) return false;
    char command[28];
    snprintf(command, sizeof(command), "G V #%s %04X 0", tag, epoch_day);

    uart_response_policy_t policy = {};
    policy.accepted_types = QFRAME_MASK_R | QFRAME_MASK_K | QFRAME_MASK_E;
    policy.terminal_types = QFRAME_MASK_K | QFRAME_MASK_E;
    policy.success_types = QFRAME_MASK_K;
    policy.first_timeout_ms = STORED_TIMEOUT_MS;
    policy.overall_timeout_ms = STORED_TIMEOUT_MS;

    for (uint8_t attempt = 0; attempt < attempts; attempt++) {
        if (post_processing_cancelled()) return false;
        StoredCapture capture = {};
        uart_transaction_result_t result = {};
        if (Arbiter::transact_cmd(command, CMD_SRC_INTERNAL, CMD_PRIO_LOW,
                                  policy, stored_value_sink, &capture,
                                  &result, sizeof(capture)) &&
            result.success && capture.received &&
            Air10Stored::parse_value(capture.payload, capture.payload_len,
                                     value)) {
            return true;
        }
        if (attempt + 1 < attempts) {
            Log::logf(CAT_GENERAL, LOG_DEBUG,
                      "[EDF] STR retry %s day=%04X attempt=%u/%u\n",
                      tag, epoch_day, attempt + 2, attempts);
        }
    }
    return false;
}

static bool read_numeric_variable(const char *name, int16_t &value) {
    char text[24] = {};
    uint32_t parsed = 0;
    if (!read_variable(name, text, sizeof(text)) ||
        !parse_hex_value(text, strlen(text), parsed) || parsed > INT16_MAX) {
        return false;
    }
    value = static_cast<int16_t>(parsed);
    return true;
}

static bool read_u32_variable(const char *name, uint32_t &value) {
    char text[24] = {};
    return read_variable(name, text, sizeof(text)) &&
           parse_hex_value(text, strlen(text), value);
}

static int32_t civil_epoch_day(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                                year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<int>(day_of_era) - 719468;
}

static bool read_native_clock(int64_t &civil, uint32_t &captured_ms) {
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        uint32_t day_before = 0, day_after = 0, uti = 0;
        if (!read_u32_variable("UDT", day_before)) return false;
        const uint32_t started = millis();
        if (!read_u32_variable("UTI", uti)) return false;
        captured_ms = started + static_cast<uint32_t>(millis() - started) / 2;
        if (!read_u32_variable("UDT", day_after)) return false;
        if (day_before == day_after)
            return Air10Clock::decode(day_before, uti, civil);
    }
    return false;
}

static bool expected_mask_off_minute(uint32_t ended_ms,
                                     uint16_t &minute) {
    if (!session_clock.native_valid) return false;
    const int64_t ended = session_clock.native_at(ended_ms);
    const uint16_t ended_day = Air10Clock::therapy_day(ended);
    if (ended_day < session_native_day || ended_day > session_native_day + 1)
        return false;
    if (ended_day != session_native_day) {
        minute = 1440;
        return true;
    }
    minute = static_cast<uint16_t>((ended + 43200) % 86400 / 60);
    return true;
}

static bool wait_for_final_str_save(uint32_t ended_ms) {
    int64_t native_now = 0;
    uint32_t clock_ms = 0;
    if (!read_native_clock(native_now, clock_ms) ||
        !session_clock.stable(native_now, clock_ms)) {
        post_error("STR native clock unavailable or changed during session");
        return false;
    }
    uint16_t expected_off = 0;
    if (!expected_mask_off_minute(ended_ms, expected_off)) {
        post_error("STR end time invalid");
        return false;
    }
    const uint16_t expected_on = static_cast<uint16_t>(
        (session_clock.native_start + 43200) % 86400 / 60);

    const uint32_t started = millis();
    uint32_t observed_generation = 0;
    bool have_generation = read_u32_variable("ZEN", observed_generation);
    bool inspect_record = true;
    uint8_t generation_read_failures = 0;
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] waiting for final STR mask=%u-%u ZEN=%s%lu\n",
              expected_on, expected_off, have_generation ? "" : "?",
              static_cast<unsigned long>(observed_generation));

    while (!post_processing_cancelled() &&
           Arbiter::get_state() == SYS_IDLE &&
           static_cast<uint32_t>(millis() - started) <
               STR_FINAL_SAVE_TIMEOUT_MS) {
        if (inspect_record) {
            uint32_t generation_before = 0;
            const bool have_before =
                read_u32_variable("ZEN", generation_before);
            Air10Stored::Value mask_on = {};
            Air10Stored::Value mask_off = {};
            const bool have_mask_on = read_stored_value(
                "ONT", session_native_day, mask_on);
            const bool have_mask_off = read_stored_value(
                "OFT", session_native_day, mask_off);
            if (have_mask_on && have_mask_off &&
                Air10Stored::contains_interval(
                    mask_on, mask_off, expected_on, expected_off,
                    MASK_OFF_TOLERANCE_MINUTES)) {
                Log::logf(CAT_GENERAL, LOG_INFO,
                          "[EDF] final STR ready mask=%u-%u dt=%lums\n",
                          expected_on, expected_off,
                          static_cast<unsigned long>(millis() - started));
                return true;
            }

            uint32_t generation_after = 0;
            const bool have_after =
                read_u32_variable("ZEN", generation_after);
            if (have_after) {
                observed_generation = generation_after;
                have_generation = true;
                generation_read_failures = 0;
            }
            inspect_record = !have_mask_on || !have_mask_off ||
                             !have_before || !have_after ||
                             generation_before != generation_after;
        }

        if (!post_processing_delay(STR_GENERATION_POLL_MS)) return false;
        uint32_t generation = 0;
        if (read_u32_variable("ZEN", generation)) {
            generation_read_failures = 0;
            if (!have_generation || generation != observed_generation) {
                Log::logf(CAT_GENERAL, LOG_DEBUG,
                          "[EDF] STR ZEN %lu -> %lu\n",
                          static_cast<unsigned long>(observed_generation),
                          static_cast<unsigned long>(generation));
                observed_generation = generation;
                have_generation = true;
                inspect_record = true;
            }
        } else if (++generation_read_failures >= 5) {
            generation_read_failures = 0;
            inspect_record = true;
        }
    }

    if (!post_processing_cancelled() && Arbiter::get_state() == SYS_IDLE)
        post_error("STR final save timeout");
    return false;
}

static void reset_schema(StreamSchema &schema, const char *tag) {
    memset(&schema, 0, sizeof(schema));
    memcpy(schema.tag, tag, 3);
}

static bool add_schema_field(StreamSchema &schema, const char *name,
                             uint8_t width) {
    if (!name || strlen(name) != 3 || width == 0 ||
        schema.field_count >= STREAM_FIELD_MAX) {
        return false;
    }
    StreamField &field = schema.fields[schema.field_count++];
    memcpy(field.name, name, 3);
    field.name[3] = 0;
    field.width = width;
    return true;
}

static bool set_schema(StreamSchema &schema, const char *tag,
                       const char *definition) {
    reset_schema(schema, tag);
    if (!definition || !definition[0]) return false;

    char copy[96];
    strncpy(copy, definition, sizeof(copy) - 1);
    char *save = nullptr;
    for (char *token = strtok_r(copy, " ", &save); token;
         token = strtok_r(nullptr, " ", &save)) {
        char *colon = strchr(token, ':');
        if (!colon || colon - token != 3) return false;
        *colon = 0;
        uint32_t width = 0;
        if (!parse_hex_value(colon + 1, strlen(colon + 1), width) ||
            width == 0 || width > 8 ||
            !add_schema_field(schema, token, static_cast<uint8_t>(width))) {
            return false;
        }
    }
    return schema.field_count > 0;
}

static bool query_schema(StreamSchema &schema, const char *tag) {
    char command[16];
    char value[120] = {};
    snprintf(command, sizeof(command), "G C &%s", tag);
    if (!command_value(command, value, sizeof(value), 300)) return false;

    char *save = nullptr;
    char *count_text = strtok_r(value, " ", &save);
    uint32_t count = 0;
    if (!count_text || !parse_hex_value(count_text, strlen(count_text), count) ||
        count == 0 || count > STREAM_FIELD_MAX) {
        return false;
    }

    reset_schema(schema, tag);
    for (uint32_t i = 0; i < count; i++) {
        char *token = strtok_r(nullptr, " ", &save);
        if (!token) return false;
        char *colon = strchr(token, ':');
        uint32_t width = 0;
        if (!colon || colon - token != 3) return false;
        *colon = 0;
        if (!parse_hex_value(colon + 1, strlen(colon + 1), width) ||
            width == 0 || width > 8 ||
            !add_schema_field(schema, token, static_cast<uint8_t>(width))) {
            return false;
        }
    }
    schema.from_firmware = true;
    return strtok_r(nullptr, " ", &save) == nullptr;
}

static int stream_profile(uint16_t mid, uint16_t vid) {
    if (mid == 0x24) {
        switch (vid) {
            case 1: case 25: case 26: case 34: case 37: case 39: return 1;
            case 2: case 38: return 2;
            case 5: return 3;
            case 7: case 11: case 12: case 28: return 4;
            case 19: return 5;
            case 9: case 27: return 6;
            case 3: case 35: return 7;
            case 36: return 8;
            default: return 0;
        }
    }
    if (mid == 0x28) {
        switch (vid) {
            case 41: case 43: case 45: return 4;
            case 50: case 51: return 5;
            case 44: case 46: case 48: case 49: return 9;
            default: return 0;
        }
    }
    return 0;
}

static void fallback_schemas(uint16_t mid, uint16_t vid) {
    const int profile = stream_profile(mid, vid);
    set_schema(stream_schemas[0], "PMD", "MKP:03 RFL:03 LYK:02");
    set_schema(stream_schemas[1], "TCE",
               profile == 4 || profile == 6 || profile == 9
                   ? "TCV:04 MKP:03 RFL:03 LYK:02"
                   : "MKP:03 RFL:03 LYK:02");
    set_schema(stream_schemas[2], "APN", "AET:04 DUR:02");
    if (profile == 3) {
        reset_schema(stream_schemas[3], "CSN");
    } else {
        set_schema(stream_schemas[3], "CSN",
                   profile == 4 || profile == 5 || profile == 6 || profile == 9
                       ? "CSR:04" : "CET:05 CSR:04");
    }

    if (!profile) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[EDF] unknown MID=%04X VID=%04X, using common AirSense schema\n",
                  mid, vid);
    }
}

static StreamSchema *find_schema(const char *tag) {
    for (StreamSchema &schema : stream_schemas) {
        if (schema.field_count && memcmp(schema.tag, tag, 3) == 0)
            return &schema;
    }
    return nullptr;
}

static bool schema_has_field(const StreamSchema &schema, const char *name) {
    for (uint8_t i = 0; i < schema.field_count; i++)
        if (strcmp(schema.fields[i].name, name) == 0) return true;
    return false;
}

static void resolve_schemas(uint16_t mid, uint16_t vid) {
    fallback_schemas(mid, vid);
    static const char *tags[STREAM_COUNT] = {
        "PMD", "TCE", "APN", "CSN",
    };
    for (uint8_t i = 0; i < STREAM_COUNT; i++) {
        StreamSchema queried;
        if (query_schema(queried, tags[i])) {
            stream_schemas[i] = queried;
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[EDF] %s schema from firmware (%u fields)\n",
                      tags[i], queried.field_count);
        }
    }
}

static bool decode_frame(const RawFrame &raw, const StreamSchema &schema,
                         DecodedFrame &decoded) {
    size_t expected = 5;
    for (uint8_t i = 0; i < schema.field_count; i++)
        expected += schema.fields[i].width;
    if (raw.len != expected || memcmp(raw.payload, schema.tag, 3) != 0)
        return false;

    uint32_t sequence = 0;
    if (!parse_hex_value(reinterpret_cast<const char *>(raw.payload + 3),
                         2, sequence)) {
        return false;
    }
    decoded.sequence = static_cast<uint8_t>(sequence);

    size_t offset = 5;
    for (uint8_t i = 0; i < schema.field_count; i++) {
        const StreamField &field = schema.fields[i];
        uint32_t value = 0;
        if (!parse_hex_value(
                reinterpret_cast<const char *>(raw.payload + offset),
                field.width, value)) {
            return false;
        }
        if (strcmp(field.name, "RFL") == 0) {
            const uint8_t bits = field.width * 4;
            if (bits < 32 && (value & (1UL << (bits - 1))))
                value |= ~((1UL << bits) - 1);
        }
        decoded.values[i] = static_cast<int32_t>(value);
        offset += field.width;
    }
    return true;
}

static bool decoded_value(const StreamSchema &schema,
                          const DecodedFrame &decoded,
                          const char *name, int16_t &value) {
    for (uint8_t i = 0; i < schema.field_count; i++) {
        if (strcmp(schema.fields[i].name, name) != 0) continue;
        if (decoded.values[i] < INT16_MIN || decoded.values[i] > INT16_MAX)
            return false;
        value = static_cast<int16_t>(decoded.values[i]);
        return true;
    }
    return false;
}

static bool decoded_value32(const StreamSchema &schema,
                            const DecodedFrame &decoded,
                            const char *name, int32_t &value) {
    for (uint8_t i = 0; i < schema.field_count; i++) {
        if (strcmp(schema.fields[i].name, name) != 0) continue;
        value = decoded.values[i];
        return true;
    }
    return false;
}

static void put_le32(uint8_t *dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
    dst[2] = static_cast<uint8_t>(value >> 16);
    dst[3] = static_cast<uint8_t>(value >> 24);
}

static bool write_exact(fs::File &file, const uint8_t *data, size_t len) {
    return file && file.write(data, len) == len;
}

static uint16_t read_le16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

static uint32_t read_le32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

static void epoch_day_to_civil(uint16_t epoch_day, int &year,
                               unsigned &month, unsigned &day) {
    int z = static_cast<int>(epoch_day) + 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(z - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
         day_of_era / 146096) / 365;
    year = static_cast<int>(year_of_era) + era * 400;
    const unsigned day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 -
                      year_of_era / 100);
    const unsigned month_prime = (5 * day_of_year + 2) / 153;
    day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    month = month_prime + (month_prime < 10 ? 3 : -9);
    year += month <= 2;
}

static bool publish_single_file(const char *partial_path,
                                const char *final_path,
                                const char *backup_path) {
    if (storage->exists(backup_path) && !storage->remove(backup_path))
        return false;

    const bool had_final = storage->exists(final_path);
    if (had_final && !storage->rename(final_path, backup_path)) return false;
    if (!storage->rename(partial_path, final_path)) {
        if (had_final) (void)storage->rename(backup_path, final_path);
        return false;
    }
    if (had_final && !storage->remove(backup_path)) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[EDF] could not remove metadata backup %s\n", backup_path);
    }
    return true;
}

static void recover_identification_files() {
    constexpr const char *TARGET = "/Identification.tgt";
    constexpr const char *CRC = "/Identification.crc";
    constexpr const char *TARGET_PART = "/Identification.tgt.part";
    constexpr const char *CRC_PART = "/Identification.crc.part";
    constexpr const char *TARGET_BAK = "/Identification.tgt.bak";
    constexpr const char *CRC_BAK = "/Identification.crc.bak";

    const bool complete = storage->exists(TARGET) && storage->exists(CRC);
    const bool backup_complete = storage->exists(TARGET_BAK) &&
                                 storage->exists(CRC_BAK);
    if (!complete && backup_complete) {
        if (storage->exists(TARGET)) storage->remove(TARGET);
        if (storage->exists(CRC)) storage->remove(CRC);
        if (!storage->rename(TARGET_BAK, TARGET) ||
            !storage->rename(CRC_BAK, CRC)) {
            status_error("Identification recovery failed");
        }
    } else if (complete) {
        if (storage->exists(TARGET_BAK)) storage->remove(TARGET_BAK);
        if (storage->exists(CRC_BAK)) storage->remove(CRC_BAK);
    } else {
        if (storage->exists(TARGET)) storage->remove(TARGET);
        if (storage->exists(CRC)) storage->remove(CRC);
        if (storage->exists(TARGET_BAK)) storage->remove(TARGET_BAK);
        if (storage->exists(CRC_BAK)) storage->remove(CRC_BAK);
    }
    if (storage->exists(TARGET_PART)) storage->remove(TARGET_PART);
    if (storage->exists(CRC_PART)) storage->remove(CRC_PART);

    portENTER_CRITICAL(&status_mux);
    status.identification_ready = storage->exists(TARGET) &&
                                  storage->exists(CRC);
    portEXIT_CRITICAL(&status_mux);
}

static void recover_str_file() {
    constexpr const char *FINAL = "/STR.edf";
    constexpr const char *PART = "/STR.edf.part";
    constexpr const char *BACKUP = "/STR.edf.bak";

    if (!storage->exists(FINAL) && storage->exists(BACKUP)) {
        if (!storage->rename(BACKUP, FINAL))
            status_error("STR recovery failed");
    } else if (storage->exists(FINAL) && storage->exists(BACKUP)) {
        storage->remove(BACKUP);
    }
    if (storage->exists(PART)) storage->remove(PART);

    uint32_t records = 0;
    fs::File file = storage->open(FINAL, FILE_READ);
    uint8_t fixed[256];
    if (file && file.read(fixed, sizeof(fixed)) == sizeof(fixed)) {
        (void)parse_decimal_field(fixed + 236, 8, records);
    }
    if (file) file.close();
    portENTER_CRITICAL(&status_mux);
    status.str_records = records;
    portEXIT_CRITICAL(&status_mux);
}

static bool publish_identification_pair() {
    constexpr const char *TARGET = "/Identification.tgt";
    constexpr const char *CRC = "/Identification.crc";
    constexpr const char *TARGET_PART = "/Identification.tgt.part";
    constexpr const char *CRC_PART = "/Identification.crc.part";
    constexpr const char *TARGET_BAK = "/Identification.tgt.bak";
    constexpr const char *CRC_BAK = "/Identification.crc.bak";

    const bool had_pair = storage->exists(TARGET) && storage->exists(CRC);
    if (storage->exists(TARGET_BAK)) storage->remove(TARGET_BAK);
    if (storage->exists(CRC_BAK)) storage->remove(CRC_BAK);
    if (had_pair) {
        if (!storage->rename(TARGET, TARGET_BAK)) return false;
        if (!storage->rename(CRC, CRC_BAK)) {
            (void)storage->rename(TARGET_BAK, TARGET);
            return false;
        }
    }

    if (!storage->rename(TARGET_PART, TARGET)) {
        if (had_pair) {
            (void)storage->rename(TARGET_BAK, TARGET);
            (void)storage->rename(CRC_BAK, CRC);
        }
        return false;
    }
    if (!storage->rename(CRC_PART, CRC)) {
        storage->remove(TARGET);
        if (had_pair) {
            (void)storage->rename(TARGET_BAK, TARGET);
            (void)storage->rename(CRC_BAK, CRC);
        }
        return false;
    }

    if (had_pair) {
        storage->remove(TARGET_BAK);
        storage->remove(CRC_BAK);
    }
    return true;
}

static bool write_identification() {
    static const char *const tags[] = {
        "IMF", "VIR", "RIR", "PVR", "PVD", "CID", "RID", "VID",
        "SRN", "SID", "PNA", "PCD", "PCB", "MID", "FGT", "BID",
    };
    uint8_t *content = static_cast<uint8_t *>(
        allocate_large(IDENTIFICATION_BUFFER_SIZE));
    if (!content) {
        post_error("Identification buffer allocation failed");
        return false;
    }

    size_t content_len = 0;
    bool built = true;
    bool cancelled = false;
    for (const char *tag : tags) {
        if (post_processing_cancelled()) {
            cancelled = true;
            built = false;
            break;
        }
        char value[128] = {};
        if (!read_variable(tag, value, sizeof(value), 1000) ||
            strchr(value, '\r') || strchr(value, '\n')) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[EDF] Identification read failed for %s\n", tag);
            built = false;
            break;
        }
        const int line_len = snprintf(
            reinterpret_cast<char *>(content + content_len),
            IDENTIFICATION_BUFFER_SIZE - content_len,
            "\r\n#%s %s\n", tag, value);
        if (line_len <= 0 ||
            static_cast<size_t>(line_len) >=
                IDENTIFICATION_BUFFER_SIZE - content_len) {
            built = false;
            break;
        }
        content_len += static_cast<size_t>(line_len);
    }
    if (!built) {
        heap_caps_free(content);
        if (!cancelled) post_error("Identification collection failed");
        return false;
    }

    if (post_processing_cancelled()) {
        heap_caps_free(content);
        return false;
    }

    const uint32_t crc = crc32_ieee(content, content_len);
    bool unchanged = false;
    fs::File old_crc = storage->open("/Identification.crc", FILE_READ);
    fs::File old_target = storage->open("/Identification.tgt", FILE_READ);
    uint8_t old_crc_value[4];
    if (old_crc && old_target && old_target.size() == content_len &&
        old_crc.read(old_crc_value, sizeof(old_crc_value)) ==
            sizeof(old_crc_value)) {
        unchanged = read_le32(old_crc_value) == crc;
    }
    if (old_crc) old_crc.close();
    if (old_target) old_target.close();
    if (unchanged) {
        heap_caps_free(content);
        portENTER_CRITICAL(&status_mux);
        status.identification_ready = true;
        portEXIT_CRITICAL(&status_mux);
        return true;
    }

    storage->remove("/Identification.tgt.part");
    storage->remove("/Identification.crc.part");
    fs::File target = storage->open("/Identification.tgt.part", FILE_WRITE);
    fs::File crc_file = storage->open("/Identification.crc.part", FILE_WRITE);
    uint8_t crc_bytes[4];
    put_le32(crc_bytes, crc);
    const bool written = target && crc_file &&
                         write_exact(target, content, content_len) &&
                         write_exact(crc_file, crc_bytes, sizeof(crc_bytes));
    if (target) {
        target.flush();
        target.close();
    }
    if (crc_file) {
        crc_file.flush();
        crc_file.close();
    }
    heap_caps_free(content);
    if (!written || !publish_identification_pair()) {
        storage->remove("/Identification.tgt.part");
        storage->remove("/Identification.crc.part");
        post_error("Identification publish failed");
        return false;
    }

    portENTER_CRITICAL(&status_mux);
    status.identification_ready = true;
    portEXIT_CRITICAL(&status_mux);
    return true;
}

static bool fetch_str_record(uint8_t *record, size_t capacity) {
    const Air10Edf::Schema &schema = Air10Edf::str_schema();
    const size_t sample_count = Air10Edf::numeric_sample_count(schema);
    int16_t *samples = static_cast<int16_t *>(
        allocate_large(sample_count * sizeof(int16_t)));
    if (!samples) {
        post_error("STR sample allocation failed");
        return false;
    }
    memset(samples, 0xFF, sample_count * sizeof(int16_t));

    Air10Stored::Value therapy_duration = {};
    if (!read_stored_value("THD", session_native_day, therapy_duration) ||
        !therapy_duration.present || therapy_duration.sample_count != 1 ||
        therapy_duration.samples[0] <= 0) {
        heap_caps_free(samples);
        post_error("STR summary not ready");
        return false;
    }

    size_t offset = 0;
    bool complete = true;
    for (uint8_t signal = 0; signal + 1 < schema.signal_count; signal++) {
        if (post_processing_cancelled()) {
            complete = false;
            break;
        }
        const char *tag = Air10Edf::str_signal_tag(signal);
        Air10Stored::Value value = {};
        if (strcmp(tag, "THD") == 0) {
            value = therapy_duration;
        } else if (!read_stored_value(tag, session_native_day, value)) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[EDF] STR read failed for %s day=%04X\n",
                      tag, session_native_day);
            complete = false;
            break;
        }
        const uint16_t expected = schema.signals[signal].samples_per_record;
        if (offset + expected > sample_count) {
            complete = false;
            break;
        }
        if (!value.present) {
            if (strcmp(tag, "LSD") == 0 || strcmp(tag, "THD") == 0) {
                Log::logf(CAT_GENERAL, LOG_WARN,
                          "[EDF] required STR value missing for %s day=%04X\n",
                          tag, session_native_day);
                complete = false;
                break;
            }
            offset += expected;
            continue;
        }
        if (value.sample_count != expected) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[EDF] STR value invalid for %s day=%04X count=%u\n",
                      tag, session_native_day, value.sample_count);
            complete = false;
            break;
        }
        if (strcmp(tag, "LSD") == 0 &&
            static_cast<uint16_t>(value.samples[0]) != session_native_day) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[EDF] STR date mismatch wanted=%04X got=%04X\n",
                      session_native_day,
                      static_cast<uint16_t>(value.samples[0]));
            complete = false;
            break;
        }
        memcpy(samples + offset, value.samples,
               expected * sizeof(int16_t));
        if (strcmp(tag, "LSD") == 0) {
            samples[offset] = static_cast<int16_t>(session_epoch_day);
        } else if (strcmp(tag, "ONT") == 0 || strcmp(tag, "OFT") == 0) {
            for (uint16_t i = 0; i < expected; i++) {
                uint16_t corrected = 0;
                if (!session_clock.mask_minute(
                        static_cast<uint16_t>(samples[offset + i]), corrected)) {
                    post_error("STR clock correction crosses therapy-day boundary");
                    complete = false;
                    break;
                }
                samples[offset + i] = static_cast<int16_t>(corrected);
            }
            if (!complete) break;
        }
        offset += expected;
    }

    size_t written = 0;
    const bool rendered = complete && offset == sample_count &&
        Air10Edf::render_numeric_record(schema, samples, sample_count,
                                        record, capacity, written) &&
        written == Air10Edf::record_size(schema);
    heap_caps_free(samples);
    if (!rendered && !post_processing_cancelled())
        post_error("STR record collection failed");
    return rendered;
}

static bool render_str_header(uint16_t first_day, uint32_t records,
                              uint8_t *header, size_t capacity) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    epoch_day_to_civil(first_day, year, month, day);
    if (month < 1 || month > 12) return false;

    char str_recording[81];
    char str_date[9];
    snprintf(str_recording, sizeof(str_recording),
             "Startdate %02u-%s-%04d X X X SRN=%s  MID=%u  VID=%u",
             day, MONTH_NAMES[month - 1], year, session_srn,
             session_mid, session_vid);
    snprintf(str_date, sizeof(str_date), "%02u.%02u.%02u",
             day, month, year % 100);
    Air10Edf::HeaderInfo info = {
        str_recording,
        str_date,
        "12.00.00",
        records,
    };
    size_t written = 0;
    return Air10Edf::render_header(Air10Edf::str_schema(), info,
                                   header, capacity, written) &&
           written == Air10Edf::header_size(Air10Edf::str_schema());
}

static bool valid_str_record(const uint8_t *record, size_t size) {
    return record && size >= 4 &&
           read_le16(record + size - 2) == crc16_ccitt(record, size - 2);
}

static bool valid_str_record(const uint8_t *record, size_t size,
                             uint16_t expected_day) {
    return record && read_le16(record) == expected_day &&
           valid_str_record(record, size);
}

static bool parse_edf_start_day(const uint8_t *text, uint16_t &epoch_day) {
    if (!text || text[2] != '.' || text[5] != '.') return false;
    const uint8_t digit_offsets[] = {0, 1, 3, 4, 6, 7};
    for (uint8_t offset : digit_offsets)
        if (text[offset] < '0' || text[offset] > '9') return false;

    const unsigned day = (text[0] - '0') * 10 + text[1] - '0';
    const unsigned month = (text[3] - '0') * 10 + text[4] - '0';
    const unsigned short_year = (text[6] - '0') * 10 + text[7] - '0';
    const int year = short_year >= 85 ? 1900 + short_year
                                      : 2000 + short_year;
    const int32_t parsed = civil_epoch_day(year, month, day);
    int check_year = 0;
    unsigned check_month = 0;
    unsigned check_day = 0;
    if (month < 1 || month > 12 || day < 1 || day > 31 || parsed < 0 ||
        parsed >= UINT16_MAX) {
        return false;
    }
    epoch_day_to_civil(parsed, check_year, check_month, check_day);
    if (check_year != year || check_month != month || check_day != day)
        return false;
    epoch_day = static_cast<uint16_t>(parsed);
    return true;
}

static bool parse_therapy_day(const char *text, uint16_t &epoch_day) {
    if (!text || strlen(text) != 8) return false;
    for (size_t i = 0; i < 8; i++)
        if (text[i] < '0' || text[i] > '9') return false;
    const int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                     (text[2] - '0') * 10 + text[3] - '0';
    const unsigned month = (text[4] - '0') * 10 + text[5] - '0';
    const unsigned day = (text[6] - '0') * 10 + text[7] - '0';
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    const int32_t parsed = civil_epoch_day(year, month, day);
    if (parsed < 0 || parsed > UINT16_MAX) return false;
    epoch_day = static_cast<uint16_t>(parsed);
    return true;
}

static bool str_contains_day(const char *therapy_day) {
    uint16_t wanted_day = 0;
    if (!parse_therapy_day(therapy_day, wanted_day)) return false;

    const Air10Edf::Schema &schema = Air10Edf::str_schema();
    const size_t header_size = Air10Edf::header_size(schema);
    const size_t record_size = Air10Edf::record_size(schema);
    fs::File file = storage->open("/STR.edf", FILE_READ);
    uint8_t fixed[256];
    uint32_t stored_header_size = 0;
    uint32_t records = 0;
    uint32_t signal_count = 0;
    if (!file || file.read(fixed, sizeof(fixed)) != sizeof(fixed) ||
        !parse_decimal_field(fixed + 184, 8, stored_header_size) ||
        !parse_decimal_field(fixed + 236, 8, records) ||
        !parse_decimal_field(fixed + 252, 4, signal_count) || records == 0 ||
        stored_header_size != header_size ||
        signal_count != schema.signal_count ||
        file.size() != header_size + records * record_size) {
        if (file) file.close();
        return false;
    }

    uint8_t *record = static_cast<uint8_t *>(allocate_large(record_size));
    bool present = false;
    if (record && file.seek(header_size) &&
        file.read(record, record_size) == record_size) {
        const uint16_t first_day = read_le16(record);
        const uint32_t offset = wanted_day >= first_day
            ? static_cast<uint32_t>(wanted_day - first_day) : UINT32_MAX;
        if (valid_str_record(record, record_size, first_day) &&
            offset < records &&
            file.seek(header_size + static_cast<size_t>(offset) * record_size) &&
            file.read(record, record_size) == record_size) {
            present = valid_str_record(record, record_size, wanted_day);
        }
    }
    if (record) heap_caps_free(record);
    file.close();
    return present;
}

static bool set_str_record_day(uint16_t epoch_day, uint8_t *record,
                               size_t size) {
    if (!record || size < 4 || epoch_day == UINT16_MAX) return false;
    record[0] = static_cast<uint8_t>(epoch_day);
    record[1] = static_cast<uint8_t>(epoch_day >> 8);
    const uint16_t crc = crc16_ccitt(record, size - 2);
    record[size - 2] = static_cast<uint8_t>(crc);
    record[size - 1] = static_cast<uint8_t>(crc >> 8);
    return true;
}

static bool render_empty_str_record(uint16_t epoch_day, uint8_t *record,
                                    size_t size) {
    if (!record || size < 4) return false;
    memset(record, 0xFF, size);
    return set_str_record_day(epoch_day, record, size);
}

static bool update_str_file(const uint8_t *incoming_record) {
    constexpr const char *FINAL = "/STR.edf";
    constexpr const char *PART = "/STR.edf.part";
    constexpr const char *BACKUP = "/STR.edf.bak";
    const Air10Edf::Schema &schema = Air10Edf::str_schema();
    const size_t header_size = Air10Edf::header_size(schema);
    const size_t record_size = Air10Edf::record_size(schema);
    if (post_processing_cancelled()) return false;
    if (!valid_str_record(incoming_record, record_size,
                          session_epoch_day)) {
        post_error("incoming STR record invalid");
        return false;
    }
    uint8_t *header = static_cast<uint8_t *>(allocate_large(header_size));
    uint8_t *work = static_cast<uint8_t *>(allocate_large(record_size));
    if (!header || !work) {
        if (header) heap_caps_free(header);
        if (work) heap_caps_free(work);
        post_error("STR rewrite buffer allocation failed");
        return false;
    }

    fs::File input;
    fs::File output;
    uint32_t existing_records = 0;
    bool valid = true;
    bool cancelled = false;
    Air10StrTimeline::Scan scan;
    if (storage->exists(FINAL)) {
        input = storage->open(FINAL, FILE_READ);
        uint8_t fixed[256];
        uint32_t stored_header_size = 0;
        uint32_t signal_count = 0;
        uint16_t header_start_day = 0;
        if (!input || input.read(fixed, sizeof(fixed)) != sizeof(fixed) ||
            !parse_decimal_field(fixed + 184, 8, stored_header_size) ||
            !parse_decimal_field(fixed + 236, 8, existing_records) ||
            !parse_decimal_field(fixed + 252, 4, signal_count) ||
            !parse_edf_start_day(fixed + 168, header_start_day) ||
            stored_header_size != header_size ||
            signal_count != schema.signal_count ||
            existing_records > Air10StrTimeline::RECORD_LIMIT ||
            input.size() != header_size + existing_records * record_size ||
            !Air10StrTimeline::begin(header_start_day, existing_records,
                                     scan) ||
            !render_str_header(session_epoch_day, 0, header, header_size)) {
            valid = false;
        }

        size_t compared = 256;
        while (valid && compared < header_size) {
            if (post_processing_cancelled()) {
                valid = false;
                cancelled = true;
                break;
            }
            const size_t chunk = min(record_size, header_size - compared);
            if (input.read(work, chunk) != chunk ||
                memcmp(work, header + compared, chunk) != 0) {
                valid = false;
                break;
            }
            compared += chunk;
        }
        for (uint32_t i = 0; valid && i < existing_records; i++) {
            if (post_processing_cancelled()) {
                valid = false;
                cancelled = true;
                break;
            }
            if (input.read(work, record_size) != record_size) {
                valid = false;
                break;
            }
            if (!valid_str_record(work, record_size) ||
                !Air10StrTimeline::scan_record(scan, i, read_le16(work))) {
                valid = false;
            }
        }
        if (!valid) {
            input.close();
            heap_caps_free(header);
            heap_caps_free(work);
            if (!cancelled) post_error("existing STR validation failed");
            return false;
        }
    } else if (!Air10StrTimeline::begin(session_epoch_day, 0, scan)) {
        heap_caps_free(header);
        heap_caps_free(work);
        post_error("STR timeline initialization failed");
        return false;
    }

    Air10StrTimeline::Plan plan;
    if (!Air10StrTimeline::make_plan(scan, session_epoch_day, plan) ||
        !render_str_header(plan.start_day, plan.record_count,
                           header, header_size)) {
        if (input) input.close();
        heap_caps_free(header);
        heap_caps_free(work);
        post_error("STR timeline range invalid");
        return false;
    }

    const size_t timeline_size =
        static_cast<size_t>(plan.record_count) * record_size;
    uint8_t *timeline = static_cast<uint8_t *>(allocate_large(timeline_size));
    uint8_t *present = static_cast<uint8_t *>(
        allocate_large(plan.record_count));
    if (!timeline || !present || post_processing_cancelled()) {
        if (input) input.close();
        if (timeline) heap_caps_free(timeline);
        if (present) heap_caps_free(present);
        heap_caps_free(header);
        heap_caps_free(work);
        if (!post_processing_cancelled())
            post_error("STR timeline allocation failed");
        return false;
    }
    memset(present, 0, plan.record_count);
    Air10StrTimeline::Buffer timeline_buffer = {
        timeline, present, plan.record_count,
    };
    Air10StrTimeline::BuildStats build_stats;

    if (input && !input.seek(header_size)) valid = false;
    for (uint32_t i = 0; valid && i < existing_records; i++) {
        if (post_processing_cancelled()) {
            valid = false;
            cancelled = true;
            break;
        }
        uint16_t day = 0;
        if (input.read(work, record_size) != record_size ||
            !valid_str_record(work, record_size) ||
            !Air10StrTimeline::record_day(scan, i, read_le16(work), day) ||
            !set_str_record_day(day, work, record_size) ||
            !Air10StrTimeline::place_record(plan, timeline_buffer, day, work,
                                            record_size, build_stats)) {
            valid = false;
        }
    }
    if (valid &&
        (!Air10StrTimeline::place_record(
             plan, timeline_buffer, session_epoch_day, incoming_record,
             record_size, build_stats) ||
         !Air10StrTimeline::fill_missing(
             plan, timeline_buffer, record_size, render_empty_str_record,
             build_stats))) {
        valid = false;
    }

    if (input) input.close();
    if (post_processing_cancelled()) {
        heap_caps_free(timeline);
        heap_caps_free(present);
        heap_caps_free(header);
        heap_caps_free(work);
        return false;
    }
    storage->remove(PART);
    output = storage->open(PART, FILE_WRITE);
    if (!valid || !output || !write_exact(output, header, header_size))
        valid = false;

    for (uint32_t i = 0; valid && i < plan.record_count; i++) {
        if (post_processing_cancelled()) {
            valid = false;
            cancelled = true;
            break;
        }
        const uint8_t *record = timeline + static_cast<size_t>(i) * record_size;
        if (!write_exact(output, record, record_size)) valid = false;
    }
    if (output) {
        output.flush();
        output.close();
    }

    if (cancelled) {
        storage->remove(PART);
        heap_caps_free(timeline);
        heap_caps_free(present);
        heap_caps_free(header);
        heap_caps_free(work);
        return false;
    }

    if (!valid || !publish_single_file(PART, FINAL, BACKUP)) {
        storage->remove(PART);
        heap_caps_free(timeline);
        heap_caps_free(present);
        heap_caps_free(header);
        heap_caps_free(work);
        post_error("STR publish failed");
        return false;
    }
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] STR timeline %04X-%04X records=%u fillers=%u "
              "replaced=%u discarded=%u\n",
              plan.start_day, plan.end_day, plan.record_count,
              build_stats.filler_records, build_stats.replaced_records,
              build_stats.discarded_records);
    heap_caps_free(timeline);
    heap_caps_free(present);
    heap_caps_free(header);
    heap_caps_free(work);
    portENTER_CRITICAL(&status_mux);
    status.str_records = plan.record_count;
    portEXIT_CRITICAL(&status_mux);
    return true;
}

static bool update_str_summary() {
    const size_t record_size =
        Air10Edf::record_size(Air10Edf::str_schema());
    uint8_t *record = static_cast<uint8_t *>(allocate_large(record_size));
    if (!record) {
        post_error("STR record allocation failed");
        return false;
    }
    const bool success = fetch_str_record(record, record_size) &&
                         !post_processing_cancelled() &&
                         update_str_file(record);
    heap_caps_free(record);
    return success;
}

static bool session_files_complete_at(const char *directory,
                                      const char *file_prefix) {
    static const char *const suffixes[] = {
        "BRP", "PLD", "SAD", "EVE", "CSL",
    };
    char path[128];
    for (const char *suffix : suffixes) {
        snprintf(path, sizeof(path), "%s/%s_%s.edf",
                 directory, file_prefix, suffix);
        if (!storage->exists(path)) return false;
        snprintf(path, sizeof(path), "%s/%s_%s.crc",
                 directory, file_prefix, suffix);
        if (!storage->exists(path)) return false;
    }
    return true;
}

static bool session_files_complete() {
    return session_files_complete_at(session_directory, status.file_prefix);
}

static bool commit_session_catalog(bool identification_ready,
                                   bool str_ready,
                                   EdfCatalog::Entry &entry) {
    if (!session_files_complete()) {
        post_error("session file set is incomplete");
        return false;
    }
    entry = {};
    strncpy(entry.therapy_day, status.therapy_day,
            sizeof(entry.therapy_day) - 1);
    strncpy(entry.file_prefix, status.file_prefix,
            sizeof(entry.file_prefix) - 1);
    entry.flags = EdfCatalog::ENTRY_LIVE_COMPLETE;
    if (identification_ready)
        entry.flags |= EdfCatalog::ENTRY_IDENTIFICATION_READY;
    if (str_ready)
        entry.flags |= EdfCatalog::ENTRY_STR_READY;
    entry.finalized_epoch = static_cast<uint32_t>(time(nullptr));
    if (!EdfCatalog::commit(entry)) {
        post_error("session catalog commit failed");
        return false;
    }
    return true;
}

static bool recover_partial_output(const char *partial_path) {
    if (!partial_path || !has_suffix(partial_path, ".edf.part")) return false;

    char final_path[128];
    char crc_path[128];
    char crc_partial_path[128];
    char edf_recovery_path[128];
    char crc_recovery_path[128];
    const size_t partial_len = strlen(partial_path);
    if (partial_len >= sizeof(final_path) || partial_len <= 5) return false;
    memcpy(final_path, partial_path, partial_len - 5);
    final_path[partial_len - 5] = 0;
    const size_t final_len = strlen(final_path);
    if (final_len < 4 || !has_suffix(final_path, ".edf")) return false;
    memcpy(crc_path, final_path, final_len + 1);
    memcpy(crc_path + final_len - 3, "crc", 4);
    if (snprintf(crc_partial_path, sizeof(crc_partial_path),
                 "%s.part", crc_path) >=
            static_cast<int>(sizeof(crc_partial_path)) ||
        snprintf(edf_recovery_path, sizeof(edf_recovery_path),
                 "%s.recover", final_path) >=
            static_cast<int>(sizeof(edf_recovery_path)) ||
        snprintf(crc_recovery_path, sizeof(crc_recovery_path),
                 "%s.recover", crc_path) >=
            static_cast<int>(sizeof(crc_recovery_path))) {
        return false;
    }
    if (storage->exists(final_path)) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[EDF] keeping partial with final-file collision: %s\n",
                  partial_path);
        return false;
    }
    // A sidecar without its EDF is an interrupted final rename from this
    // recorder. Recovery recalculates it from the complete records.
    if (storage->exists(crc_path)) storage->remove(crc_path);
    if (storage->exists(crc_partial_path)) storage->remove(crc_partial_path);
    if (storage->exists(edf_recovery_path))
        storage->remove(edf_recovery_path);
    if (storage->exists(crc_recovery_path))
        storage->remove(crc_recovery_path);

    fs::File input = storage->open(partial_path, FILE_READ);
    fs::File output;
    fs::File crc_output;
    uint8_t *header = nullptr;
    uint8_t *buffer = nullptr;
    uint32_t records = 0;
    bool recovered = false;

    do {
        uint8_t fixed_header[256];
        if (!input || input.size() < sizeof(fixed_header) ||
            input.read(fixed_header, sizeof(fixed_header)) !=
                sizeof(fixed_header)) {
            break;
        }

        uint32_t header_size = 0;
        uint32_t signal_count = 0;
        if (!parse_decimal_field(fixed_header + 184, 8, header_size) ||
            !parse_decimal_field(fixed_header + 252, 4, signal_count) ||
            signal_count == 0 || header_size > RECOVERY_HEADER_MAX ||
            header_size != 256 + signal_count * 256 ||
            input.size() < header_size) {
            break;
        }

        header = static_cast<uint8_t *>(allocate_large(header_size));
        buffer = static_cast<uint8_t *>(allocate_large(RECOVERY_BUFFER_SIZE));
        if (!header || !buffer) break;
        memcpy(header, fixed_header, sizeof(fixed_header));
        if (input.read(header + sizeof(fixed_header),
                       header_size - sizeof(fixed_header)) !=
            header_size - sizeof(fixed_header)) {
            break;
        }

        const size_t samples_offset = 256 + signal_count * 216;
        uint32_t samples_per_record = 0;
        for (uint32_t i = 0; i < signal_count; i++) {
            uint32_t samples = 0;
            if (!parse_decimal_field(header + samples_offset + i * 8,
                                     8, samples) ||
                samples_per_record > UINT32_MAX - samples) {
                samples_per_record = 0;
                break;
            }
            samples_per_record += samples;
        }
        const uint32_t record_size = samples_per_record * 2;
        if (samples_per_record == 0 || record_size / 2 != samples_per_record ||
            record_size > 65536)
            break;

        const size_t data_bytes = input.size() - header_size;
        records = data_bytes / record_size;
        const size_t complete_bytes = static_cast<size_t>(records) * record_size;
        if (!Air10Edf::update_record_count(header, header_size, records))
            break;

        output = storage->open(edf_recovery_path, FILE_WRITE);
        if (!output || !write_exact(output, header, header_size)) break;
        uint32_t rest_crc = crc32_ieee_update(
            crc32_ieee_initial(), header + 256, header_size - 256);
        size_t remaining = complete_bytes;
        while (remaining) {
            const size_t chunk = remaining < RECOVERY_BUFFER_SIZE
                                     ? remaining : RECOVERY_BUFFER_SIZE;
            if (input.read(buffer, chunk) != chunk ||
                !write_exact(output, buffer, chunk)) {
                remaining = SIZE_MAX;
                break;
            }
            rest_crc = crc32_ieee_update(rest_crc, buffer, chunk);
            remaining -= chunk;
        }
        if (remaining != 0) break;
        output.flush();
        output.close();

        uint8_t sidecar[8];
        put_le32(sidecar, crc32_ieee(header, 256));
        put_le32(sidecar + 4, crc32_ieee_finish(rest_crc));
        crc_output = storage->open(crc_recovery_path, FILE_WRITE);
        if (!crc_output || !write_exact(crc_output, sidecar, sizeof(sidecar)))
            break;
        crc_output.flush();
        crc_output.close();

        if (!storage->rename(crc_recovery_path, crc_path)) break;
        if (!storage->rename(edf_recovery_path, final_path)) {
            storage->remove(crc_path);
            break;
        }
        if (!storage->remove(partial_path)) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[EDF] recovered but could not remove %s\n", partial_path);
        }
        recovered = true;
    } while (false);

    if (input) input.close();
    if (output) output.close();
    if (crc_output) crc_output.close();
    if (header) heap_caps_free(header);
    if (buffer) heap_caps_free(buffer);
    if (!recovered) {
        storage->remove(edf_recovery_path);
        storage->remove(crc_recovery_path);
        status_error("partial EDF recovery failed");
        return false;
    }

    portENTER_CRITICAL(&status_mux);
    status.recovered_files++;
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] recovered %s (%u records)\n", final_path, records);
    return true;
}

static void recover_partial_outputs() {
    if (!storage->exists("/DATALOG")) return;
    fs::File root = storage->open("/DATALOG", FILE_READ);
    if (!root || !root.isDirectory()) return;

    fs::File day;
    while ((day = root.openNextFile(FILE_READ))) {
        if (!day.isDirectory()) {
            day.close();
            continue;
        }
        char partial_paths[8][128] = {};
        uint8_t partial_count = 0;
        fs::File entry;
        while ((entry = day.openNextFile(FILE_READ))) {
            if (!entry.isDirectory() && partial_count < 8 &&
                has_suffix(entry.path(), ".edf.part")) {
                strncpy(partial_paths[partial_count], entry.path(),
                        sizeof(partial_paths[partial_count]) - 1);
                partial_count++;
            }
            entry.close();
        }
        day.close();
        for (uint8_t i = 0; i < partial_count; i++)
            (void)recover_partial_output(partial_paths[i]);
    }
    root.close();
    SdStorage::refresh_usage();
}

static bool parse_brp_file_prefix(const char *path, char *prefix,
                                  size_t prefix_size) {
    if (!path || !prefix || prefix_size < 16) return false;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strlen(name) != 23 || strcmp(name + 15, "_BRP.edf") != 0 ||
        name[8] != '_') {
        return false;
    }
    for (size_t i = 0; i < 15; i++) {
        if (i == 8) continue;
        if (name[i] < '0' || name[i] > '9') return false;
    }
    memcpy(prefix, name, 15);
    prefix[15] = 0;
    return true;
}

static uint32_t count_catalog_candidates() {
    fs::File root = storage->open("/DATALOG", FILE_READ);
    if (!root || !root.isDirectory()) return 0;
    uint32_t count = 0;
    fs::File day;
    while ((day = root.openNextFile(FILE_READ))) {
        if (!day.isDirectory()) {
            day.close();
            continue;
        }
        const char *day_name = strrchr(day.path(), '/');
        day_name = day_name ? day_name + 1 : day.path();
        uint16_t ignored_day = 0;
        if (!parse_therapy_day(day_name, ignored_day)) {
            day.close();
            continue;
        }
        fs::File file;
        while ((file = day.openNextFile(FILE_READ))) {
            char prefix[16];
            if (!file.isDirectory() &&
                parse_brp_file_prefix(file.path(), prefix, sizeof(prefix)) &&
                count < UINT32_MAX) {
                count++;
            }
            file.close();
        }
        day.close();
    }
    root.close();
    return count;
}

static bool prefix_in_snapshot(const char *prefixes, uint32_t count,
                               const char *prefix) {
    if (!prefixes || !prefix) return false;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(prefixes + static_cast<size_t>(i) * 16, prefix) == 0)
            return true;
    }
    return false;
}

static void reconcile_catalog() {
    if (!storage->exists("/DATALOG")) return;
    EdfCatalog::Status catalog;
    EdfCatalog::get_status(catalog);
    const uint32_t candidate_count = count_catalog_candidates();
    const size_t prefix_capacity =
        static_cast<size_t>(catalog.entries) + candidate_count;
    char *known_prefixes = nullptr;
    uint32_t known_count = 0;
    if (prefix_capacity <= SIZE_MAX / 16) {
        const size_t bytes = prefix_capacity * 16;
        if (bytes) {
            known_prefixes = static_cast<char *>(heap_caps_malloc(
                bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!known_prefixes && bytes <= 16 * 1024) {
                known_prefixes = static_cast<char *>(heap_caps_malloc(
                    bytes, MALLOC_CAP_8BIT));
            }
        }
        if (catalog.entries == 0) {
            known_count = 0;
        } else if (!known_prefixes ||
                   !EdfCatalog::snapshot_prefixes(
                       known_prefixes, bytes, known_count)) {
            if (known_prefixes) heap_caps_free(known_prefixes);
            known_prefixes = nullptr;
            known_count = 0;
        }
    }

    fs::File root = storage->open("/DATALOG", FILE_READ);
    if (!root || !root.isDirectory()) {
        if (known_prefixes) heap_caps_free(known_prefixes);
        return;
    }

    fs::File day;
    while ((day = root.openNextFile(FILE_READ))) {
        if (!day.isDirectory()) {
            day.close();
            continue;
        }
        char directory[40];
        strncpy(directory, day.path(), sizeof(directory) - 1);
        directory[sizeof(directory) - 1] = 0;
        const char *day_name = strrchr(directory, '/');
        day_name = day_name ? day_name + 1 : directory;
        uint16_t ignored_day = 0;
        if (!parse_therapy_day(day_name, ignored_day)) {
            day.close();
            continue;
        }

        fs::File file;
        while ((file = day.openNextFile(FILE_READ))) {
            char prefix[16];
            const bool candidate = !file.isDirectory() &&
                parse_brp_file_prefix(file.path(), prefix, sizeof(prefix));
            const uint32_t finalized_epoch = candidate
                ? static_cast<uint32_t>(file.getLastWrite()) : 0;
            file.close();
            if (!candidate ||
                !session_files_complete_at(directory, prefix)) {
                continue;
            }

            EdfCatalog::Entry entry;
            const bool known = known_prefixes
                ? prefix_in_snapshot(known_prefixes, known_count, prefix)
                : EdfCatalog::find(prefix, entry);
            if (known) continue;
            entry = {};
            strncpy(entry.therapy_day, day_name,
                    sizeof(entry.therapy_day) - 1);
            strncpy(entry.file_prefix, prefix,
                    sizeof(entry.file_prefix) - 1);
            entry.flags = EdfCatalog::ENTRY_LIVE_COMPLETE;
            if (status.identification_ready)
                entry.flags |= EdfCatalog::ENTRY_IDENTIFICATION_READY;
            if (str_contains_day(day_name))
                entry.flags |= EdfCatalog::ENTRY_STR_READY;
            entry.finalized_epoch = finalized_epoch
                ? finalized_epoch : static_cast<uint32_t>(time(nullptr));
            if (!EdfCatalog::commit(entry)) {
                status_error("catalog reconciliation failed");
                day.close();
                root.close();
                if (known_prefixes) heap_caps_free(known_prefixes);
                return;
            }
            if (known_prefixes && known_count < prefix_capacity) {
                memcpy(known_prefixes + static_cast<size_t>(known_count) * 16,
                       entry.file_prefix, 16);
                known_count++;
            }
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[EDF] catalog recovered %s/%s\n",
                      entry.therapy_day, entry.file_prefix);
        }
        day.close();
    }
    root.close();
    if (known_prefixes) heap_caps_free(known_prefixes);
}

static bool render_output_header(OutputFile &output, uint32_t records,
                                 size_t &written) {
    Air10Edf::HeaderInfo info = {
        recording_id,
        start_date,
        start_time,
        records,
    };
    return Air10Edf::render_header(*output.schema, info, header_buffer,
                                   header_capacity, written);
}

static bool open_output(OutputFile &output, const Air10Edf::Schema &schema) {
    output = {};
    output.schema = &schema;
    snprintf(output.partial_path, sizeof(output.partial_path), "%s/%s_%s.edf.part",
             session_directory, status.file_prefix, schema.suffix);
    snprintf(output.final_path, sizeof(output.final_path), "%s/%s_%s.edf",
             session_directory, status.file_prefix, schema.suffix);
    snprintf(output.crc_partial_path, sizeof(output.crc_partial_path),
             "%s/%s_%s.crc.part", session_directory, status.file_prefix,
             schema.suffix);
    snprintf(output.crc_final_path, sizeof(output.crc_final_path),
             "%s/%s_%s.crc", session_directory, status.file_prefix,
             schema.suffix);

    if (storage->exists(output.partial_path) ||
        storage->exists(output.final_path) ||
        storage->exists(output.crc_partial_path) ||
        storage->exists(output.crc_final_path)) {
        return false;
    }
    output.file = storage->open(output.partial_path, FILE_WRITE);
    if (!output.file) return false;
    output.created = true;

    size_t header_len = 0;
    if (!render_output_header(output, 0, header_len) ||
        !write_exact(output.file, header_buffer, header_len)) {
        output.file.close();
        return false;
    }
    output.rest_crc = crc32_ieee_update(
        crc32_ieee_initial(), header_buffer + 256, header_len - 256);
    output.open = true;
    return true;
}

static bool append_output_record(OutputFile &output,
                                 const uint8_t *data, size_t len) {
    if (output.failed || !output.open) return false;
    if (!write_exact(output.file, data, len)) {
        output.failed = true;
        output.file.close();
        output.open = false;
        status_error("SD record write failed");
        return false;
    }
    output.rest_crc = crc32_ieee_update(output.rest_crc, data, len);
    output.records++;
    output.file.flush();
    return true;
}

static bool finalize_output(OutputFile &output) {
    if (output.failed || !output.open) return false;
    size_t header_len = 0;
    if (!render_output_header(output, output.records, header_len) ||
        !output.file.seek(0) ||
        !write_exact(output.file, header_buffer, header_len)) {
        status_error("EDF header finalization failed");
        output.file.close();
        output.open = false;
        return false;
    }
    output.file.flush();
    output.file.close();
    output.open = false;

    uint8_t sidecar[8];
    put_le32(sidecar, crc32_ieee(header_buffer, 256));
    put_le32(sidecar + 4, crc32_ieee_finish(output.rest_crc));
    fs::File crc_file = storage->open(output.crc_partial_path, FILE_WRITE);
    if (!crc_file || !write_exact(crc_file, sidecar, sizeof(sidecar))) {
        if (crc_file) crc_file.close();
        status_error("CRC sidecar write failed");
        return false;
    }
    crc_file.flush();
    crc_file.close();
    if (!storage->rename(output.crc_partial_path, output.crc_final_path)) {
        status_error("CRC sidecar rename failed");
        return false;
    }
    if (!storage->rename(output.partial_path, output.final_path)) {
        storage->remove(output.crc_final_path);
        status_error("EDF rename failed");
        return false;
    }
    return true;
}

static void close_output(OutputFile &output, bool remove_partial) {
    if (output.file) output.file.close();
    if (remove_partial && output.created && storage && output.partial_path[0])
        storage->remove(output.partial_path);
    output.open = false;
}

static bool initialize_accumulator(Accumulator &accumulator,
                                   const Air10Edf::Schema &schema) {
    accumulator = {};
    accumulator.sample_count = Air10Edf::numeric_sample_count(schema);
    accumulator.samples = static_cast<int16_t *>(
        allocate_large(accumulator.sample_count * sizeof(int16_t)));
    if (!accumulator.samples) return false;
    memset(accumulator.samples, 0xFF,
           accumulator.sample_count * sizeof(int16_t));
    accumulator.initialized = true;
    return open_output(accumulator.output, schema);
}

static void release_accumulator(Accumulator &accumulator, bool remove_partial) {
    close_output(accumulator.output, remove_partial);
    if (accumulator.samples) heap_caps_free(accumulator.samples);
    accumulator = {};
}

static bool write_current_record(Accumulator &accumulator) {
    size_t written = 0;
    if (!Air10Edf::render_numeric_record(
            *accumulator.output.schema, accumulator.samples,
            accumulator.sample_count, record_buffer, record_capacity,
            written)) {
        status_error("numeric record encoding failed");
        return false;
    }
    if (!append_output_record(accumulator.output, record_buffer, written))
        return false;
    memset(accumulator.samples, 0xFF,
           accumulator.sample_count * sizeof(int16_t));
    return true;
}

static bool advance_record(Accumulator &accumulator, uint32_t target) {
    if (!accumulator.initialized || target < accumulator.current_record)
        return false;
    if (target - accumulator.current_record > 1440) {
        status_error("record timeline jump too large");
        return false;
    }
    while (accumulator.current_record < target) {
        if (!write_current_record(accumulator)) return false;
        accumulator.current_record++;
    }
    return true;
}

static size_t signal_sample_offset(const Air10Edf::Schema &schema,
                                   uint8_t signal) {
    size_t offset = 0;
    for (uint8_t i = 0; i < signal; i++)
        offset += schema.signals[i].samples_per_record;
    return offset;
}

static int signal_index(const Air10Edf::Schema &schema, const char *label) {
    for (uint8_t i = 0; i + 1 < schema.signal_count; i++)
        if (strcmp(schema.signals[i].label, label) == 0) return i;
    return -1;
}

static void store_sample(Accumulator &accumulator, const char *label,
                         uint32_t record, uint16_t slot, int16_t value) {
    if (!advance_record(accumulator, record)) return;
    const Air10Edf::Schema &schema = *accumulator.output.schema;
    const int signal = signal_index(schema, label);
    if (signal < 0 || slot >= schema.signals[signal].samples_per_record) return;
    accumulator.samples[signal_sample_offset(schema, signal) + slot] = value;
}

static uint32_t relative_ms(uint32_t captured_ms) {
    return static_cast<uint32_t>(captured_ms - session_start_capture_ms);
}

static void process_wave(const RawFrame &raw, const StreamSchema &schema,
                         const DecodedFrame &decoded) {
    uint32_t sample_ms = relative_ms(raw.captured_ms);
    if (!wave_clock.initialized) {
        wave_clock.initialized = true;
        wave_clock.relative_ms = sample_ms;
    } else {
        const uint8_t delta = static_cast<uint8_t>(
            decoded.sequence - wave_clock.last_sequence);
        const uint32_t predicted = wave_clock.relative_ms +
                                   static_cast<uint32_t>(delta) * 40;
        const int32_t drift = static_cast<int32_t>(sample_ms - predicted);
        if (delta == 0) return;
        if (delta > 32 || drift > 400 || drift < -400)
            wave_clock.relative_ms = sample_ms;
        else
            wave_clock.relative_ms = predicted;
    }
    wave_clock.last_sequence = decoded.sequence;

    const uint32_t record = wave_clock.relative_ms / 60000;
    const uint16_t slot = (wave_clock.relative_ms % 60000) / 40;
    int16_t value;
    if (decoded_value(schema, decoded, "RFL", value))
        store_sample(brp, "Flow.40ms", record, slot, value);
    if (decoded_value(schema, decoded, "MKP", value)) {
        store_sample(brp, "Press.40ms", record, slot, value);
    }
    if (decoded_value(schema, decoded, "TCV", value))
        store_sample(brp, "TrigCycEvt.40ms", record, slot, value);
}

static void sample_pld() {
    if (!status.active) return;
    static const struct {
        const char *label;
        const char *tag;
    } sources[] = {
        {"MaskPress.2s", "MKF"}, {"Press.2s", "MKI"},
        {"EprPress.2s", "MKE"},  {"Leak.2s", "LKF"},
        {"RespRate.2s", "RRR"},  {"TidVol.2s", "TDD"},
        {"MinVent.2s", "MV5"},   {"TgtVent.2s", "TGT"},
        {"IERatio.2s", "IER"},   {"Snore.2s", "SNI"},
        {"FlowLim.2s", "FFL"},   {"B5ITime.2s", "IN5"},
        {"B5ETime.2s", "EX5"},   {"Ti.2s", "INT"},
    };

    const uint32_t elapsed = relative_ms(millis());
    const uint32_t absolute_slot = elapsed / 2000;
    if (absolute_slot == last_pld_slot) return;
    last_pld_slot = absolute_slot;
    const uint32_t record = absolute_slot / 30;
    const uint16_t slot = absolute_slot % 30;

    for (const auto &source : sources) {
        int16_t value = EDF_MISSING;
        (void)read_numeric_variable(source.tag, value);
        store_sample(pld, source.label, record, slot, value);
    }
}

static bool append_annotation(OutputFile &output, uint32_t onset,
                              uint32_t duration, const char *label) {
    size_t written = 0;
    if (!Air10Edf::render_annotation_record(
            *output.schema, onset, duration, label,
            record_buffer, record_capacity, written)) {
        status_error("annotation encoding failed");
        return false;
    }
    return append_output_record(output, record_buffer, written);
}

static void process_apnea(const RawFrame &raw, const StreamSchema &schema,
                          const DecodedFrame &decoded) {
    int16_t event_type = 0;
    int16_t duration = 0;
    if (!decoded_value(schema, decoded, "AET", event_type) || event_type == 0)
        return;
    (void)decoded_value(schema, decoded, "DUR", duration);
    const char *label = nullptr;
    switch (event_type) {
        case 1: label = "Hypopnea"; break;
        case 2: label = "Central Apnea"; break;
        case 3: label = "Obstructive Apnea"; break;
        case 4: label = "Apnea"; break;
        case 5: label = "Arousal"; break;
        default: return;
    }
    const uint32_t end = relative_ms(raw.captured_ms) / 1000;
    const uint32_t safe_duration = duration > 0 ? duration : 0;
    append_annotation(eve, end > safe_duration ? end - safe_duration : 0,
                      safe_duration, label);
}

static uint32_t csr_onset(const RawFrame &raw, int32_t event_time) {
    const uint32_t arrival = relative_ms(raw.captured_ms) / 1000;
    if (!session_clock.native_valid || event_time < 0 || event_time >= 86400)
        return arrival;
    int32_t candidate = static_cast<int32_t>(event_time) -
                        static_cast<int32_t>(session_clock.native_start % 86400);
    while (candidate < 0) candidate += 86400;
    while (candidate + 43200 < static_cast<int32_t>(arrival)) candidate += 86400;
    while (candidate > static_cast<int32_t>(arrival) + 43200) candidate -= 86400;
    return candidate >= 0 ? static_cast<uint32_t>(candidate) : arrival;
}

static void process_csr(const RawFrame &raw, const StreamSchema &schema,
                        const DecodedFrame &decoded) {
    int16_t event_type = 0;
    int32_t event_time = -1;
    if (!decoded_value(schema, decoded, "CSR", event_type)) return;
    (void)decoded_value32(schema, decoded, "CET", event_time);
    const char *label = event_type == 1 ? "CSR Start" :
                        event_type == 2 ? "CSR End" : nullptr;
    if (label) append_annotation(csl, csr_onset(raw, event_time), 0, label);
}

static void process_raw_frame(const RawFrame &raw) {
    if (raw.len < 5) return;
    char tag[4] = {
        static_cast<char>(raw.payload[0]),
        static_cast<char>(raw.payload[1]),
        static_cast<char>(raw.payload[2]),
        0,
    };
    StreamSchema *schema = find_schema(tag);
    DecodedFrame decoded = {};
    if (!schema || !decode_frame(raw, *schema, decoded)) return;

    portENTER_CRITICAL(&status_mux);
    status.raw_frames++;
    portEXIT_CRITICAL(&status_mux);

    if (strcmp(tag, wave_tag) == 0) process_wave(raw, *schema, decoded);
    else if (strcmp(tag, "APN") == 0) process_apnea(raw, *schema, decoded);
    else if (strcmp(tag, "CSN") == 0) process_csr(raw, *schema, decoded);
}

static void sample_oximetry() {
    if (!status.active) return;
    const uint32_t elapsed = relative_ms(millis());
    const uint32_t absolute_slot = elapsed / 1000;
    if (absolute_slot == last_oxi_slot) return;
    last_oxi_slot = absolute_slot;
    const uint32_t record = absolute_slot / 60;
    const uint16_t slot = absolute_slot % 60;

    oxi_reading_t reading;
    OxiArbiter::snapshot(reading);
    const bool fresh = reading.valid &&
                       static_cast<uint32_t>(millis() - reading.timestamp_ms) <= 10000;
    store_sample(sad, "Pulse.1s", record, slot,
                 fresh ? reading.pulse_bpm : EDF_MISSING);
    store_sample(sad, "SpO2.1s", record, slot,
                 fresh ? reading.spo2 : EDF_MISSING);
}

static bool frame_sink(const qframe_t *frame, void *) {
    if (!capture_active || !frame || frame->type != QFRAME_TYPE_L ||
        frame->payload_len < 3 || frame->payload_len > RAW_PAYLOAD_MAX) {
        return true;
    }

    const bool wanted = memcmp(wave_tag, frame->payload, 3) == 0 ||
                        memcmp("APN", frame->payload, 3) == 0 ||
                        memcmp("CSN", frame->payload, 3) == 0;
    if (!wanted) return true;

    RawFrame raw = {};
    raw.captured_ms = millis();
    raw.len = frame->payload_len;
    memcpy(raw.payload, frame->payload, frame->payload_len);
    if (xQueueSend(raw_queue, &raw, 0) != pdTRUE) {
        portENTER_CRITICAL(&status_mux);
        status.raw_dropped++;
        portEXIT_CRITICAL(&status_mux);
    }
    return true;
}

static void release_streams() {
    for (LiveStream::internal_handle_t &lease : stream_leases) {
        if (lease >= 0) LiveStream::release_internal(lease);
        lease = -1;
    }
}

static void acquire_stream(uint8_t slot, const char *tag) {
    StreamSchema *schema = find_schema(tag);
    if (!schema || !schema->field_count) return;
    stream_leases[slot] = LiveStream::acquire_internal(tag);
    if (stream_leases[slot] < 0)
        Log::logf(CAT_GENERAL, LOG_WARN, "[EDF] %s subscribe failed\n", tag);
}

static void acquire_streams() {
    for (LiveStream::internal_handle_t &lease : stream_leases) lease = -1;
    acquire_stream(0, "TCE");
    if (stream_leases[0] < 0) {
        strcpy(wave_tag, "PMD");
        acquire_stream(0, "PMD");
    }
    acquire_stream(1, "APN");
    acquire_stream(2, "CSN");
}

static void clear_session_memory(bool remove_partial) {
    release_accumulator(brp, remove_partial);
    release_accumulator(pld, remove_partial);
    release_accumulator(sad, remove_partial);
    close_output(eve, remove_partial);
    close_output(csl, remove_partial);
    if (header_buffer) heap_caps_free(header_buffer);
    if (record_buffer) heap_caps_free(record_buffer);
    header_buffer = nullptr;
    record_buffer = nullptr;
    header_capacity = 0;
    record_capacity = 0;
}

static bool make_paths_and_metadata(const ControlEvent &event,
                                    uint16_t &mid, uint16_t &vid) {
    session_clock = {};
    session_clock.captured_ms = event.captured_ms;
    int64_t native_now = 0;
    uint32_t clock_ms = 0;
    session_clock.native_valid = read_native_clock(native_now, clock_ms);
    if (session_clock.native_valid) {
        session_clock.native_start = native_now -
            static_cast<uint32_t>(clock_ms - event.captured_ms) / 1000;
        session_native_day = Air10Clock::therapy_day(session_clock.native_start);
    }
    const bool trusted_time = event.trusted_time && event.epoch >= CLOCK_VALID_AFTER;
    if (!trusted_time && !session_clock.native_valid && event.epoch < CLOCK_VALID_AFTER) {
        status_error("clock is not set");
        return false;
    }
    time_t start_epoch = event.epoch;
    struct tm start_tm;
    localtime_r(&start_epoch, &start_tm);
    if (!trusted_time && session_clock.native_valid) {
        start_epoch = static_cast<time_t>(session_clock.native_start);
        gmtime_r(&start_epoch, &start_tm);
    }
    session_clock.export_start = static_cast<int64_t>(civil_epoch_day(
        start_tm.tm_year + 1900, start_tm.tm_mon + 1, start_tm.tm_mday)) * 86400 +
        start_tm.tm_hour * 3600 + start_tm.tm_min * 60 + start_tm.tm_sec;
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] clock=%s native=%u correction=%llds\n",
              trusted_time ? "NTP" : session_clock.native_valid ? "ResMed" : "ESP fallback",
              session_clock.native_valid,
              session_clock.native_valid
                  ? session_clock.export_start - session_clock.native_start : 0LL);
    snprintf(start_date, sizeof(start_date), "%02d.%02d.%02d",
             start_tm.tm_mday, start_tm.tm_mon + 1,
             (start_tm.tm_year + 1900) % 100);
    snprintf(start_time, sizeof(start_time), "%02d.%02d.%02d",
             start_tm.tm_hour, start_tm.tm_min, start_tm.tm_sec);

    struct tm therapy_tm = start_tm;
    if (therapy_tm.tm_hour < 12) {
        therapy_tm.tm_mday--;
        therapy_tm.tm_isdst = -1;
        (void)mktime(&therapy_tm);
    }
    char therapy_day[9];
    snprintf(therapy_day, sizeof(therapy_day), "%04d%02d%02d",
             therapy_tm.tm_year + 1900, therapy_tm.tm_mon + 1,
             therapy_tm.tm_mday);
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%04d%02d%02d_%02d%02d%02d",
             start_tm.tm_year + 1900, start_tm.tm_mon + 1, start_tm.tm_mday,
             start_tm.tm_hour, start_tm.tm_min, start_tm.tm_sec);

    if (!storage->exists("/DATALOG") && !storage->mkdir("/DATALOG")) {
        status_error("cannot create DATALOG");
        return false;
    }
    snprintf(session_directory, sizeof(session_directory),
             "/DATALOG/%s", therapy_day);
    if (!storage->exists(session_directory) &&
        !storage->mkdir(session_directory)) {
        status_error("cannot create therapy-day directory");
        return false;
    }

    portENTER_CRITICAL(&status_mux);
    strncpy(status.therapy_day, therapy_day, sizeof(status.therapy_day));
    strncpy(status.file_prefix, prefix, sizeof(status.file_prefix));
    portEXIT_CRITICAL(&status_mux);

    char srn[24] = "0";
    char mid_text[16] = "0";
    char vid_text[16] = "0";
    (void)read_variable("SRN", srn, sizeof(srn));
    (void)read_variable("MID", mid_text, sizeof(mid_text));
    (void)read_variable("VID", vid_text, sizeof(vid_text));
    uint32_t parsed = 0;
    mid = parse_hex_value(mid_text, strlen(mid_text), parsed)
              ? static_cast<uint16_t>(parsed) : 0;
    vid = parse_hex_value(vid_text, strlen(vid_text), parsed)
              ? static_cast<uint16_t>(parsed) : 0;
    const int32_t epoch_day = civil_epoch_day(therapy_tm.tm_year + 1900,
                                               therapy_tm.tm_mon + 1,
                                               therapy_tm.tm_mday);
    if (epoch_day < 0 || epoch_day > UINT16_MAX) {
        status_error("therapy day is outside Air10 range");
        return false;
    }
    session_epoch_day = static_cast<uint16_t>(epoch_day);
    session_mid = mid;
    session_vid = vid;
    strncpy(session_srn, srn, sizeof(session_srn) - 1);
    session_srn[sizeof(session_srn) - 1] = 0;
    snprintf(recording_id, sizeof(recording_id),
             "Startdate %02d-%s-%04d X X X SRN=%s  MID=%u  VID=%u",
             start_tm.tm_mday, MONTH_NAMES[start_tm.tm_mon],
             start_tm.tm_year + 1900, srn, mid, vid);
    return true;
}

static bool allocate_session_buffers(const Air10Edf::Schema &brp_schema,
                                     const Air10Edf::Schema &pld_schema) {
    header_capacity = Air10Edf::header_size(pld_schema);
    const size_t brp_header = Air10Edf::header_size(brp_schema);
    if (brp_header > header_capacity) header_capacity = brp_header;
    header_buffer = static_cast<uint8_t *>(allocate_large(header_capacity));

    record_capacity = Air10Edf::record_size(brp_schema);
    const size_t pld_record = Air10Edf::record_size(pld_schema);
    if (pld_record > record_capacity) record_capacity = pld_record;
    record_buffer = static_cast<uint8_t *>(allocate_large(record_capacity));
    if (!header_buffer || !record_buffer) return false;

    if (!initialize_accumulator(brp, brp_schema) ||
        !initialize_accumulator(pld, pld_schema) ||
        !initialize_accumulator(sad, Air10Edf::sad_schema()) ||
        !open_output(eve, Air10Edf::eve_schema()) ||
        !open_output(csl, Air10Edf::csl_schema())) {
        return false;
    }
    return append_annotation(eve, 0, 0, "Recording starts") &&
           append_annotation(csl, 0, 0, "Recording starts");
}

static void reset_session_state(const ControlEvent &event) {
    session_start_capture_ms = event.captured_ms;
    wave_clock = {};
    last_pld_slot = UINT32_MAX;
    last_oxi_slot = UINT32_MAX;
    xQueueReset(raw_queue);
}

static void start_session(const ControlEvent &event) {
    __atomic_store_n(&therapy_start_pending, false, __ATOMIC_RELEASE);
    if (status.active) return;
    uint16_t mid = 0;
    uint16_t vid = 0;
    portENTER_CRITICAL(&status_mux);
    status.last_error[0] = 0;
    portEXIT_CRITICAL(&status_mux);
    if (!make_paths_and_metadata(event, mid, vid)) return;
    resolve_schemas(mid, vid);

    const StreamSchema *tce_schema = find_schema("TCE");
    const bool tcv = tce_schema && schema_has_field(*tce_schema, "TCV");
    const Air10Edf::Schema &brp_layout = Air10Edf::brp_schema(tcv);
    const Air10Edf::Schema &pld_layout = Air10Edf::pld_schema();

    reset_session_state(event);
    if (!allocate_session_buffers(brp_layout, pld_layout)) {
        status_error("session buffer or file initialization failed");
        clear_session_memory(true);
        return;
    }

    portENTER_CRITICAL(&status_mux);
    status.active = true;
    status.started_epoch = event.epoch;
    status.raw_frames = 0;
    status.raw_dropped = 0;
    status.write_errors = 0;
    status.post_errors = 0;
    status.brp_records = 0;
    status.pld_records = 0;
    status.sad_records = 0;
    status.eve_records = 1;
    status.csl_records = 1;
    portEXIT_CRITICAL(&status_mux);

    strcpy(wave_tag, "TCE");
    capture_active = true;
    acquire_streams();
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] recording %s/%s MID=%u VID=%u\n",
              status.therapy_day, status.file_prefix, mid, vid);
}

static void update_record_status() {
    portENTER_CRITICAL(&status_mux);
    status.brp_records = brp.output.records;
    status.pld_records = pld.output.records;
    status.sad_records = sad.output.records;
    status.eve_records = eve.records;
    status.csl_records = csl.records;
    portEXIT_CRITICAL(&status_mux);
}

static void stop_session(const ControlEvent &event) {
    if (!status.active) return;
    capture_active = false;
    release_streams();

    RawFrame raw;
    while (xQueueReceive(raw_queue, &raw, 0) == pdTRUE)
        process_raw_frame(raw);
    sample_oximetry();

    (void)write_current_record(brp);
    (void)write_current_record(pld);
    (void)write_current_record(sad);
    update_record_status();

    (void)finalize_output(brp.output);
    (void)finalize_output(pld.output);
    (void)finalize_output(sad.output);
    (void)finalize_output(eve);
    (void)finalize_output(csl);
    update_record_status();
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] complete %s BRP=%u PLD=%u SAD=%u EVE=%u CSL=%u drops=%u\n",
              status.file_prefix, status.brp_records, status.pld_records,
              status.sad_records, status.eve_records, status.csl_records,
              status.raw_dropped);
    clear_session_memory(false);
    portENTER_CRITICAL(&status_mux);
    status.active = false;
    status.post_processing = true;
    portEXIT_CRITICAL(&status_mux);

    const bool identification_ready = !post_processing_cancelled() &&
                                      write_identification();
    const bool str_ready = !post_processing_cancelled() &&
                           wait_for_final_str_save(event.captured_ms) &&
                           update_str_summary();
    const bool interrupted = post_processing_cancelled();
    EdfCatalog::Entry catalog_entry;
    const bool catalog_ready = commit_session_catalog(
        identification_ready, str_ready, catalog_entry);
    SdStorage::refresh_usage();

    portENTER_CRITICAL(&status_mux);
    status.post_processing = false;
    portEXIT_CRITICAL(&status_mux);
    if (catalog_ready && !interrupted)
        (void)ExportSync::request_post_therapy(catalog_entry);
    if (interrupted) {
        Log::logf(CAT_GENERAL, LOG_INFO,
                  "[EDF] post-processing preempted by therapy start\n");
    }
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] post-processing complete STR=%u errors=%u\n",
              status.str_records, status.post_errors);
}

static void recorder_task(void *) {
    while (true) {
        ControlEvent control;
        while (xQueueReceive(control_queue, &control, 0) == pdTRUE) {
            if (control.kind == ControlKind::Start) start_session(control);
            else stop_session(control);
        }

        RawFrame raw;
        if (xQueueReceive(raw_queue, &raw, pdMS_TO_TICKS(20)) == pdTRUE &&
            status.active) {
            process_raw_frame(raw);
        }
        sample_pld();
        sample_oximetry();
        if (status.active) update_record_status();
    }
}

}  // namespace

void init() {
    if (status.ready || !SdStorage::mounted()) return;
    storage = SdStorage::filesystem();
    if (!storage) return;
    recover_partial_outputs();
    recover_identification_files();
    recover_str_file();
    EdfCatalog::init();
    reconcile_catalog();

    raw_queue_capacity = RAW_QUEUE_CAPACITY_PSRAM;
    raw_queue_storage = static_cast<uint8_t *>(heap_caps_malloc(
        sizeof(RawFrame) * raw_queue_capacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw_queue_storage) {
        raw_queue_capacity = RAW_QUEUE_CAPACITY_FALLBACK;
        raw_queue_storage = static_cast<uint8_t *>(heap_caps_malloc(
            sizeof(RawFrame) * raw_queue_capacity, MALLOC_CAP_8BIT));
    }
    if (!raw_queue_storage) {
        status_error("raw queue allocation failed");
        return;
    }

    raw_queue = xQueueCreateStatic(raw_queue_capacity, sizeof(RawFrame),
                                   raw_queue_storage, &raw_queue_state);
    control_queue = xQueueCreate(4, sizeof(ControlEvent));
    if (!raw_queue || !control_queue) {
        status_error("queue initialization failed");
        return;
    }
    for (LiveStream::internal_handle_t &lease : stream_leases) lease = -1;
    frame_listener = Arbiter::add_frame_listener(QFRAME_MASK_L,
                                                  frame_sink, nullptr);
    if (frame_listener < 0) {
        status_error("UART listener allocation failed");
        return;
    }
    if (xTaskCreatePinnedToCore(recorder_task, "edf_rec", RECORDER_STACK,
                                nullptr, 2, &recorder_task_handle, 1) != pdPASS) {
        Arbiter::remove_frame_listener(frame_listener);
        frame_listener = -1;
        status_error("task creation failed");
        return;
    }

    portENTER_CRITICAL(&status_mux);
    status.ready = true;
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[EDF] recorder ready queue=%u (%s)\n",
              raw_queue_capacity,
              raw_queue_capacity == RAW_QUEUE_CAPACITY_PSRAM ? "PSRAM" : "internal");
}

void therapy_started() {
    if (!status.ready || !control_queue) return;
    __atomic_store_n(&therapy_start_pending, true, __ATOMIC_RELEASE);
    const bool trusted_time = WiFiSetup::time_synced();
    ControlEvent event = {ControlKind::Start,
                          static_cast<uint32_t>(time(nullptr)), millis(),
                          trusted_time};
    if (xQueueSend(control_queue, &event, 0) != pdTRUE) {
        __atomic_store_n(&therapy_start_pending, false, __ATOMIC_RELEASE);
        status_error("control queue full at therapy start");
    }
}

void therapy_ended() {
    if (!status.ready || !control_queue) return;
    const bool trusted_time = WiFiSetup::time_synced();
    ControlEvent event = {ControlKind::Stop,
                          static_cast<uint32_t>(time(nullptr)), millis(),
                          trusted_time};
    if (xQueueSend(control_queue, &event, 0) != pdTRUE)
        status_error("control queue full at therapy stop");
}

void get_status(Status &out) {
    portENTER_CRITICAL(&status_mux);
    out = status;
    portEXIT_CRITICAL(&status_mux);
}

}  // namespace EdfRecorder

#else

namespace EdfRecorder {

void init() {}
void therapy_started() {}
void therapy_ended() {}

void get_status(Status &out) {
    out = {};
    out.supported = false;
}

}  // namespace EdfRecorder

#endif
