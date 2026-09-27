#include "edf_recorder.h"
#include "hex_util.h"

#include "board.h"

#if AB_STORAGE_HAS_SDCARD

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "air10_edf.h"
#include "air10_stored.h"
#include "air10_str_timeline.h"
#include "air10_clock.h"
#include "edf_pending.h"
#include "crc.h"
#include "debug_log.h"
#include "edf_catalog.h"
#include "export_sync.h"
#include "live_stream.h"
#include "memory_manager.h"
#include "oxi_arbiter.h"
#include "qframe.h"
#include "sd_storage.h"
#include "uart_arbiter.h"

namespace EdfRecorder {
namespace {

constexpr uint16_t RAW_QUEUE_CAPACITY_PSRAM = 128;
constexpr uint16_t RAW_QUEUE_CAPACITY_FALLBACK = 48;
constexpr uint16_t RAW_PAYLOAD_MAX = 64;
constexpr uint8_t STREAM_FIELD_MAX = 8;
constexpr uint8_t STREAM_COUNT = 4;
constexpr uint16_t RECORDER_STACK = 8192;
constexpr uint16_t POLL_TIMEOUT_MS = 120;
constexpr uint16_t RECORDING_STATE_POLL_MS = 1000;
constexpr uint16_t STORED_TIMEOUT_MS = 2000;
constexpr uint8_t STORED_TRANSFER_ATTEMPTS = 3;
constexpr uint16_t STR_GENERATION_POLL_MS = 1000;
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
    uint32_t captured_ms;
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
static volatile bool therapy_wanted = false;
static uint32_t therapy_on_capture_ms = 0;
static uint32_t recording_therapy_on_ms = 0;
static ControlEvent latest_stop = {};
static uint32_t next_start_ms = 0;
static uint32_t next_recording_state_ms = 0;
static bool storage_ready = false;
static uint32_t next_storage_ms = 0;

static fs::FS *storage = nullptr;
static bool recording_storage_owned = false;
static uint32_t next_pending_ms = 0;
static bool pending_scanned = false;
static const char *pending_scan_error = nullptr;
static uint32_t device_generation = 0;
static uint32_t synced_device_generation = 0;
static uint32_t synced_str_generation = 0;
static uint16_t synced_saved_day = 0;
static bool have_synced_generation = false;
static bool summary_export_pending = false;
static EdfCatalog::Entry summary_export_entry = {};
static char pending_cursor[16] = {};
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

static uint32_t session_start_capture_ms = 0;
static uint32_t segment_duration_ms = 0;
static Air10Clock::Anchor session_clock;
static uint16_t session_native_day = 0;
static char recording_id[81] = {};
static char start_date[9] = {};
static char start_time[9] = {};
static char session_directory[40] = {};
static uint16_t session_mid = 0;
static uint16_t session_vid = 0;
static char session_srn[24] = {};
static bool identification_verified = false;
static uint32_t identification_device = 0;
static uint32_t identification_crc = 0;
static uint16_t identification_mid = 0, identification_vid = 0;
static char identification_srn[24] = {};

static WaveClock wave_clock = {};
static uint32_t last_pld_slot = UINT32_MAX;
static uint32_t last_oxi_slot = UINT32_MAX;

static void status_error(const char *message, const char *file = nullptr) {
    portENTER_CRITICAL(&status_mux);
    status.write_errors++;
    strncpy(status.last_error, message ? message : "error",
            sizeof(status.last_error) - 1);
    status.last_error[sizeof(status.last_error) - 1] = 0;
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_EDF, LOG_ERROR, "%s%s%s\n",
              file ? file : "", file ? ": " : "", message ? message : "error");
}

static void post_error(const char *message) {
    portENTER_CRITICAL(&status_mux);
    status.post_errors++;
    strncpy(status.last_error, message ? message : "post-processing error",
            sizeof(status.last_error) - 1);
    status.last_error[sizeof(status.last_error) - 1] = 0;
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_EDF, LOG_ERROR, "%s\n",
              message ? message : "post-processing error");
}

static bool post_processing_cancelled() {
    return !Arbiter::device_standby() ||
           __atomic_load_n(&therapy_start_pending, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE);
}

static bool parse_hex_value(const char *text, size_t width, uint32_t &value) {
    return aircannect::parse_hex(text, width, value);
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
    return Arbiter::get_var(name, CMD_SRC_INTERNAL, CMD_PRIO_LOW,
                            out, capacity, timeout_ms);
}

static bool read_stored_value(const char *tag, uint16_t epoch_day,
                              Air10Stored::Value &value,
                              uint8_t attempts = STORED_TRANSFER_ATTEMPTS) {
    for (uint8_t attempt = 0; attempt < attempts; attempt++) {
        if (post_processing_cancelled()) return false;
        auto result = Air10Stored::read(tag, epoch_day, value, STORED_TIMEOUT_MS, true);
        if (result != Air10Stored::ReadResult::Failed) return true;
        if (attempt + 1 < attempts) {
            Log::logf(CAT_EDF, LOG_DEBUG,
                      "STR retry %s day=%04X attempt=%u/%u\n",
                      tag, epoch_day, attempt + 2, attempts);
        }
    }
    return false;
}

static bool read_numeric_variable(const char *name, int16_t &value) {
    uint32_t parsed = 0;
    if (Arbiter::read_var_hex(name, CMD_SRC_INTERNAL, CMD_PRIO_LOW,
                             parsed, POLL_TIMEOUT_MS) != Arbiter::VarResult::Ok ||
        parsed > INT16_MAX) {
        return false;
    }
    value = static_cast<int16_t>(parsed);
    return true;
}

static bool read_u32_variable(const char *name, uint32_t &value) {
    return Arbiter::read_var_hex(name, CMD_SRC_INTERNAL, CMD_PRIO_LOW,
                                 value, POLL_TIMEOUT_MS) == Arbiter::VarResult::Ok;
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
        Log::logf(CAT_EDF, LOG_WARN,
                  "unknown MID=%04X VID=%04X, using common AirSense schema\n",
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
            Log::logf(CAT_EDF, LOG_INFO,
                      "%s schema from firmware (%u fields)\n",
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
        Log::logf(CAT_EDF, LOG_WARN,
                  "could not remove metadata backup %s\n", backup_path);
    }
    return true;
}

static void pending_path(const char *prefix, char *path, size_t size) {
    snprintf(path, size, "/airbridge/pending/%s.str", prefix);
}

enum class PendingRead { Ready, Invalid, Unavailable };

static PendingRead read_pending(const char *path, EdfPending::Record &record) {
    fs::File file = storage->open(path, FILE_READ);
    if (!file || file.isDirectory()) return PendingRead::Unavailable;
    uint8_t bytes[EdfPending::WIRE_SIZE];
    if (file.size() != sizeof(bytes)) return PendingRead::Invalid;
    if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) return PendingRead::Unavailable;
    return EdfPending::decode(bytes, sizeof(bytes), record)
        ? PendingRead::Ready : PendingRead::Invalid;
}

static bool write_pending(const EdfPending::Record &record) {
    if ((!storage->exists("/airbridge") && !storage->mkdir("/airbridge")) ||
        (!storage->exists("/airbridge/pending") && !storage->mkdir("/airbridge/pending")))
        return false;
    char path[80], part[88], backup[88];
    pending_path(record.prefix, path, sizeof(path));
    snprintf(part, sizeof(part), "%s.part", path);
    snprintf(backup, sizeof(backup), "%s.bak", path);
    uint8_t bytes[EdfPending::WIRE_SIZE];
    EdfPending::encode(record, bytes);
    storage->remove(part);
    fs::File file = storage->open(part, FILE_WRITE);
    if (!file || !SdStorage::write_exact(file, bytes, sizeof(bytes))) return false;
    file.flush(); file.close();
    return publish_single_file(part, path, backup);
}

static bool save_pending(const char *prefix = nullptr, const char *day = nullptr) {
    EdfPending::Record pending;
    pending.native_day = session_native_day;
    pending.mid = session_mid; pending.vid = session_vid;
    memcpy(pending.srn, session_srn, sizeof(pending.srn));
    memcpy(pending.prefix, prefix ? prefix : status.file_prefix, sizeof(pending.prefix));
    memcpy(pending.day, day ? day : status.therapy_day, sizeof(pending.day));
    if (!write_pending(pending)) {
        post_error("STR pending journal write failed");
        return false;
    }
    portENTER_CRITICAL(&status_mux);
    status.pending_str++;
    portEXIT_CRITICAL(&status_mux);
    return true;
}

static void recover_pending() {
    fs::File dir = storage->open("/airbridge/pending");
    if (!dir || !dir.isDirectory()) return;
    fs::File item;
    while ((item = dir.openNextFile())) {
        String path = item.path();
        item.close();
        if (!path.endsWith(".part") && !path.endsWith(".bak")) continue;
        String target = path.substring(0, path.lastIndexOf('.'));
        EdfPending::Record record;
        if (read_pending(target.c_str(), record) == PendingRead::Ready) storage->remove(path);
        else if (read_pending(path.c_str(), record) == PendingRead::Ready) {
            storage->remove(target);
            if (!storage->rename(path, target)) post_error("STR journal recovery failed");
        } else post_error("invalid STR pending journal");
    }
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

    // SD lookups may wait for an interrupt; publish only the result under lock.
    const bool identification_ready = storage->exists(TARGET) && storage->exists(CRC);
    portENTER_CRITICAL(&status_mux);
    status.identification_ready = identification_ready;
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

static bool collect_identification(uint8_t *&content, size_t &content_len) {
    static const char *const tags[] = {
        "IMF", "VIR", "RIR", "PVR", "PVD", "CID", "RID", "VID",
        "SRN", "SID", "PNA", "PCD", "PCB", "MID", "FGT", "BID",
    };
    content = static_cast<uint8_t *>(
        aircannect::Memory::alloc_large(IDENTIFICATION_BUFFER_SIZE));
    if (!content) {
        post_error("Identification buffer allocation failed");
        return false;
    }

    content_len = 0;
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
            Log::logf(CAT_EDF, LOG_WARN,
                      "Identification read failed for %s\n", tag);
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
        if (!cancelled) post_error("Identification collection failed");
        return false;
    }
    return !post_processing_cancelled();
}

static bool write_identification(const uint8_t *content, size_t content_len) {
    const uint32_t crc = crc32_ieee(content, content_len);
    bool unchanged = false;
    fs::File old_crc = storage->open("/Identification.crc", FILE_READ);
    fs::File old_target = storage->open("/Identification.tgt", FILE_READ);
    uint8_t old_crc_value[4];
    if (old_crc && old_target && old_target.size() == content_len &&
        old_crc.read(old_crc_value, sizeof(old_crc_value)) ==
            sizeof(old_crc_value)) {
        unchanged = SdStorage::get_le32(old_crc_value) == crc;
    }
    if (old_crc) old_crc.close();
    if (old_target) old_target.close();
    if (unchanged) {
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
    SdStorage::put_le32(crc_bytes, crc);
    const bool written = target && crc_file &&
                         SdStorage::write_exact(target, content, content_len) &&
                         SdStorage::write_exact(crc_file, crc_bytes, sizeof(crc_bytes));
    if (target) {
        target.flush();
        target.close();
    }
    if (crc_file) {
        crc_file.flush();
        crc_file.close();
    }
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

static bool ensure_identification(uint32_t device) {
    if (!SdStorage::try_acquire()) return false;
    const bool current = identification_verified && identification_device == device &&
        identification_mid == session_mid && identification_vid == session_vid &&
        !strcmp(identification_srn, session_srn) &&
        storage->exists("/Identification.tgt") && storage->exists("/Identification.crc");
    SdStorage::release();
    if (current) return true;

    uint8_t *content = nullptr;
    size_t size = 0;
    bool success = collect_identification(content, size) &&
        !post_processing_cancelled() &&
        device == __atomic_load_n(&device_generation, __ATOMIC_ACQUIRE);
    if (success && SdStorage::try_acquire()) {
        success = write_identification(content, size);
        SdStorage::release();
        if (success) {
            identification_device = device;
            identification_crc = crc32_ieee(content, size);
            identification_mid = session_mid;
            identification_vid = session_vid;
            memcpy(identification_srn, session_srn, sizeof(identification_srn));
            identification_verified = true;
        }
    } else {
        success = false;
    }
    aircannect::Memory::free(content);
    return success;
}

static bool fetch_str_record(uint8_t *record, size_t capacity) {
    const Air10Edf::Schema &schema = Air10Edf::str_schema();
    const size_t sample_count = Air10Edf::numeric_sample_count(schema);
    int16_t *samples = static_cast<int16_t *>(
        aircannect::Memory::alloc_large(sample_count * sizeof(int16_t)));
    if (!samples) {
        post_error("STR sample allocation failed");
        return false;
    }
    memset(samples, 0xFF, sample_count * sizeof(int16_t));

    Air10Stored::Value therapy_duration = {};
    if (!read_stored_value("THD", session_native_day, therapy_duration) ||
        !therapy_duration.present || therapy_duration.sample_count != 1) {
        aircannect::Memory::free(samples);
        post_error("STR duration unavailable");
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
            Log::logf(CAT_EDF, LOG_WARN,
                      "STR read failed for %s day=%04X\n",
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
                Log::logf(CAT_EDF, LOG_WARN,
                          "required STR value missing for %s day=%04X\n",
                          tag, session_native_day);
                complete = false;
                break;
            }
            offset += expected;
            continue;
        }
        if (value.sample_count != expected) {
            Log::logf(CAT_EDF, LOG_WARN,
                      "STR value invalid for %s day=%04X count=%u\n",
                      tag, session_native_day, value.sample_count);
            complete = false;
            break;
        }
        if (strcmp(tag, "LSD") == 0 &&
            static_cast<uint16_t>(value.samples[0]) != session_native_day) {
            Log::logf(CAT_EDF, LOG_WARN,
                      "STR date mismatch wanted=%04X got=%04X\n",
                      session_native_day,
                      static_cast<uint16_t>(value.samples[0]));
            complete = false;
            break;
        }
        memcpy(samples + offset, value.samples,
               expected * sizeof(int16_t));
        offset += expected;
    }

    size_t written = 0;
    const bool rendered = complete && offset == sample_count &&
        Air10Edf::render_numeric_record(schema, samples, sample_count,
                                        record, capacity, written) &&
        written == Air10Edf::record_size(schema);
    aircannect::Memory::free(samples);
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
           SdStorage::get_le16(record + size - 2) == crc16_ccitt(record, size - 2);
}

static bool valid_str_record(const uint8_t *record, size_t size,
                             uint16_t expected_day) {
    return record && SdStorage::get_le16(record) == expected_day &&
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
    const int32_t parsed = Air10Clock::civil_epoch_day(year, month, day);
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
    const int32_t parsed = Air10Clock::civil_epoch_day(year, month, day);
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

    uint8_t *record = static_cast<uint8_t *>(aircannect::Memory::alloc_large(record_size));
    bool present = false;
    if (record && file.seek(header_size) &&
        file.read(record, record_size) == record_size) {
        const uint16_t first_day = SdStorage::get_le16(record);
        const uint32_t offset = wanted_day >= first_day
            ? static_cast<uint32_t>(wanted_day - first_day) : UINT32_MAX;
        if (valid_str_record(record, record_size, first_day) &&
            offset < records &&
            file.seek(header_size + static_cast<size_t>(offset) * record_size) &&
            file.read(record, record_size) == record_size) {
            present = valid_str_record(record, record_size, wanted_day);
        }
    }
    aircannect::Memory::free(record);
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

static bool update_str_file(const uint8_t *incoming_record,
                            const EdfCatalog::Entry &latest) {
    constexpr const char *FINAL = "/STR.edf";
    constexpr const char *PART = "/STR.edf.part";
    constexpr const char *BACKUP = "/STR.edf.bak";
    const Air10Edf::Schema &schema = Air10Edf::str_schema();
    const size_t header_size = Air10Edf::header_size(schema);
    const size_t record_size = Air10Edf::record_size(schema);
    if (post_processing_cancelled()) return false;
    if (!valid_str_record(incoming_record, record_size,
                          session_native_day)) {
        post_error("incoming STR record invalid");
        return false;
    }
    uint8_t *header = static_cast<uint8_t *>(aircannect::Memory::alloc_large(header_size));
    uint8_t *work = static_cast<uint8_t *>(aircannect::Memory::alloc_large(record_size));
    if (!header || !work) {
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
        post_error("STR rewrite buffer allocation failed");
        return false;
    }

    fs::File input;
    fs::File output;
    uint32_t existing_records = 0;
    bool valid = true;
    bool cancelled = false;
    bool same_header = false, same_record = false;
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
            !render_str_header(header_start_day, existing_records, header, header_size)) {
            valid = false;
        }
        if (valid) same_header = memcmp(fixed, header, sizeof(fixed)) == 0;

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
                !Air10StrTimeline::scan_record(scan, i, SdStorage::get_le16(work))) {
                valid = false;
            }
            if (valid && SdStorage::get_le16(work) == session_native_day)
                same_record = memcmp(work, incoming_record, record_size) == 0;
        }
        if (!valid) {
            input.close();
            aircannect::Memory::free(header);
            aircannect::Memory::free(work);
            if (!cancelled) post_error("existing STR validation failed");
            return false;
        }
    } else if (!Air10StrTimeline::begin(session_native_day, 0, scan)) {
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
        post_error("STR timeline initialization failed");
        return false;
    }

    Air10StrTimeline::Plan plan;
    if (!Air10StrTimeline::make_plan(scan, session_native_day, plan) ||
        !render_str_header(plan.start_day, plan.record_count,
                           header, header_size)) {
        if (input) input.close();
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
        post_error("STR timeline range invalid");
        return false;
    }

    if (same_header && same_record && scan.continuous &&
        plan.start_day == scan.header_start_day && plan.record_count == existing_records) {
        input.close();
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
        return true;
    }

    const size_t timeline_size =
        static_cast<size_t>(plan.record_count) * record_size;
    uint8_t *timeline = static_cast<uint8_t *>(aircannect::Memory::alloc_large(timeline_size));
    uint8_t *present = static_cast<uint8_t *>(
        aircannect::Memory::alloc_large(plan.record_count));
    if (!timeline || !present || post_processing_cancelled()) {
        if (input) input.close();
        aircannect::Memory::free(timeline);
        aircannect::Memory::free(present);
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
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
            !Air10StrTimeline::record_day(scan, i, SdStorage::get_le16(work), day) ||
            !set_str_record_day(day, work, record_size) ||
            !Air10StrTimeline::place_record(plan, timeline_buffer, day, work,
                                            record_size, build_stats)) {
            valid = false;
        }
    }
    if (valid &&
        (!Air10StrTimeline::place_record(
             plan, timeline_buffer, session_native_day, incoming_record,
             record_size, build_stats) ||
         !Air10StrTimeline::fill_missing(
             plan, timeline_buffer, record_size, render_empty_str_record,
             build_stats))) {
        valid = false;
    }

    if (input) input.close();
    if (post_processing_cancelled()) {
        aircannect::Memory::free(timeline);
        aircannect::Memory::free(present);
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
        return false;
    }
    storage->remove(PART);
    output = storage->open(PART, FILE_WRITE);
    if (!valid || !output || !SdStorage::write_exact(output, header, header_size))
        valid = false;

    for (uint32_t i = 0; valid && i < plan.record_count; i++) {
        if (post_processing_cancelled()) {
            valid = false;
            cancelled = true;
            break;
        }
        const uint8_t *record = timeline + static_cast<size_t>(i) * record_size;
        if (!SdStorage::write_exact(output, record, record_size)) valid = false;
    }
    if (output) {
        output.flush();
        output.close();
    }

    if (cancelled) {
        storage->remove(PART);
        aircannect::Memory::free(timeline);
        aircannect::Memory::free(present);
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
        return false;
    }

    // Preserve the day across a reset between STR publication and catalog commit.
    char pending[80];
    pending_path(latest.file_prefix, pending, sizeof(pending));
    if (valid && !storage->exists(pending))
        valid = save_pending(latest.file_prefix, latest.therapy_day);
    if (!valid || !publish_single_file(PART, FINAL, BACKUP)) {
        storage->remove(PART);
        aircannect::Memory::free(timeline);
        aircannect::Memory::free(present);
        aircannect::Memory::free(header);
        aircannect::Memory::free(work);
        post_error("STR publish failed");
        return false;
    }
    Log::logf(CAT_EDF, LOG_INFO,
              "STR timeline %04X-%04X records=%u fillers=%u "
              "replaced=%u discarded=%u\n",
              plan.start_day, plan.end_day, plan.record_count,
              build_stats.filler_records, build_stats.replaced_records,
              build_stats.discarded_records);
    aircannect::Memory::free(timeline);
    aircannect::Memory::free(present);
    aircannect::Memory::free(header);
    aircannect::Memory::free(work);
    portENTER_CRITICAL(&status_mux);
    status.str_records = plan.record_count;
    portEXIT_CRITICAL(&status_mux);
    return true;
}

static bool collect_str_summary(uint8_t *&record, uint32_t generation) {
    record = nullptr;
    Air10Stored::Value date = {};
    if (!read_stored_value("LSD", session_native_day, date)) return false;
    uint32_t after = 0;
    if (!date.present) {
        return read_u32_variable("ZEN", after) && after == generation &&
               !post_processing_cancelled();
    }
    const size_t record_size =
        Air10Edf::record_size(Air10Edf::str_schema());
    record = static_cast<uint8_t *>(aircannect::Memory::alloc_large(record_size));
    if (!record) {
        post_error("STR record allocation failed");
        return false;
    }
    return fetch_str_record(record, record_size) &&
                         read_u32_variable("ZEN", after) && generation == after &&
                         !post_processing_cancelled();
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
        Log::logf(CAT_EDF, LOG_WARN,
                  "keeping partial with final-file collision: %s\n",
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

        header = static_cast<uint8_t *>(aircannect::Memory::alloc_large(header_size));
        buffer = static_cast<uint8_t *>(aircannect::Memory::alloc_large(RECOVERY_BUFFER_SIZE));
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
        if (!output || !SdStorage::write_exact(output, header, header_size)) break;
        uint32_t rest_crc = crc32_ieee_update(
            crc32_ieee_initial(), header + 256, header_size - 256);
        size_t remaining = complete_bytes;
        while (remaining) {
            const size_t chunk = remaining < RECOVERY_BUFFER_SIZE
                                     ? remaining : RECOVERY_BUFFER_SIZE;
            if (input.read(buffer, chunk) != chunk ||
                !SdStorage::write_exact(output, buffer, chunk)) {
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
        SdStorage::put_le32(sidecar, crc32_ieee(header, 256));
        SdStorage::put_le32(sidecar + 4, crc32_ieee_finish(rest_crc));
        crc_output = storage->open(crc_recovery_path, FILE_WRITE);
        if (!crc_output || !SdStorage::write_exact(crc_output, sidecar, sizeof(sidecar)))
            break;
        crc_output.flush();
        crc_output.close();

        if (!storage->rename(crc_recovery_path, crc_path)) break;
        if (!storage->rename(edf_recovery_path, final_path)) {
            storage->remove(crc_path);
            break;
        }
        if (!storage->remove(partial_path)) {
            Log::logf(CAT_EDF, LOG_WARN,
                      "recovered but could not remove %s\n", partial_path);
        }
        recovered = true;
    } while (false);

    if (input) input.close();
    if (output) output.close();
    if (crc_output) crc_output.close();
    aircannect::Memory::free(header);
    aircannect::Memory::free(buffer);
    if (!recovered) {
        storage->remove(edf_recovery_path);
        storage->remove(crc_recovery_path);
        status_error("partial EDF recovery failed");
        return false;
    }

    portENTER_CRITICAL(&status_mux);
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_EDF, LOG_INFO,
              "recovered %s (%u records)\n", final_path, records);
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

static bool reconcile_catalog() {
    if (!storage->exists("/DATALOG")) return true;
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
            known_prefixes = static_cast<char *>(aircannect::Memory::alloc_large(
                bytes, bytes <= 16 * 1024));
        }
        if (catalog.entries == 0) {
            known_count = 0;
        } else if (!known_prefixes ||
                   !EdfCatalog::snapshot_prefixes(
                       known_prefixes, bytes, known_count)) {
            aircannect::Memory::free(known_prefixes);
            known_prefixes = nullptr;
            known_count = 0;
        }
    }

    fs::File root = storage->open("/DATALOG", FILE_READ);
    if (!root || !root.isDirectory()) {
        aircannect::Memory::free(known_prefixes);
        return false;
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
            char pending[80];
            pending_path(prefix, pending, sizeof(pending));
            if (!storage->exists(pending) && str_contains_day(day_name))
                entry.flags |= EdfCatalog::ENTRY_STR_READY;
            entry.finalized_epoch = finalized_epoch
                ? finalized_epoch : static_cast<uint32_t>(time(nullptr));
            if (!EdfCatalog::commit(entry)) {
                status_error("catalog reconciliation failed");
                day.close();
                root.close();
                aircannect::Memory::free(known_prefixes);
                return false;
            }
            if (known_prefixes && known_count < prefix_capacity) {
                memcpy(known_prefixes + static_cast<size_t>(known_count) * 16,
                       entry.file_prefix, 16);
                known_count++;
            }
            Log::logf(CAT_EDF, LOG_INFO,
                      "catalog recovered %s/%s\n",
                      entry.therapy_day, entry.file_prefix);
        }
        day.close();
    }
    root.close();
    aircannect::Memory::free(known_prefixes);
    return true;
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

static void output_path(const OutputFile &output, const char *extension,
                         char (&path)[128]) {
    snprintf(path, sizeof(path), "%s/%s_%s.%s", session_directory,
             status.file_prefix, output.schema->suffix, extension);
}

static bool open_output(OutputFile &output, const Air10Edf::Schema &schema) {
    output = {};
    output.schema = &schema;

    char path[128];
    for (const char *extension : {"edf.part", "edf", "crc.part", "crc"}) {
        output_path(output, extension, path);
        if (storage->exists(path)) return false;
    }
    output_path(output, "edf.part", path);
    output.file = storage->open(path, FILE_WRITE);
    if (!output.file) return false;
    output.created = true;

    size_t header_len = 0;
    if (!render_output_header(output, 0, header_len) ||
        !SdStorage::write_exact(output.file, header_buffer, header_len)) {
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
    if (!SdStorage::write_exact(output.file, data, len)) {
        output.failed = true;
        output.file.close();
        output.open = false;
        status_error("SD record write failed", output.schema->suffix);
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
        !SdStorage::write_exact(output.file, header_buffer, header_len)) {
        status_error("EDF header finalization failed", output.schema->suffix);
        output.file.close();
        output.open = false;
        return false;
    }
    output.file.flush();
    output.file.close();
    output.open = false;

    uint8_t sidecar[8];
    SdStorage::put_le32(sidecar, crc32_ieee(header_buffer, 256));
    SdStorage::put_le32(sidecar + 4, crc32_ieee_finish(output.rest_crc));
    char partial_path[128], final_path[128];
    output_path(output, "crc.part", partial_path);
    output_path(output, "crc", final_path);
    fs::File crc_file = storage->open(partial_path, FILE_WRITE);
    if (!crc_file || !SdStorage::write_exact(crc_file, sidecar, sizeof(sidecar))) {
        if (crc_file) crc_file.close();
        status_error("CRC sidecar write failed", output.schema->suffix);
        return false;
    }
    crc_file.flush();
    crc_file.close();
    if (!storage->rename(partial_path, final_path)) {
        status_error("CRC sidecar rename failed", output.schema->suffix);
        return false;
    }
    output_path(output, "edf.part", partial_path);
    output_path(output, "edf", final_path);
    if (!storage->rename(partial_path, final_path)) {
        output_path(output, "crc", final_path);
        storage->remove(final_path);
        status_error("EDF rename failed", output.schema->suffix);
        return false;
    }
    return true;
}

static void close_output(OutputFile &output, bool remove_partial) {
    if (output.file) output.file.close();
    if (remove_partial && output.created && storage) {
        char path[128];
        output_path(output, "edf.part", path);
        storage->remove(path);
    }
    output = {};
}

static bool initialize_accumulator(Accumulator &accumulator,
                                   const Air10Edf::Schema &schema) {
    accumulator = {};
    accumulator.sample_count = Air10Edf::numeric_sample_count(schema);
    accumulator.samples = static_cast<int16_t *>(
        aircannect::Memory::alloc_large((accumulator.sample_count + 1) * sizeof(int16_t)));
    if (!accumulator.samples) return false;
    memset(accumulator.samples, 0xFF,
           accumulator.sample_count * sizeof(int16_t));
    accumulator.initialized = true;
    return open_output(accumulator.output, schema);
}

static void release_accumulator(Accumulator &accumulator, bool remove_partial) {
    close_output(accumulator.output, remove_partial);
    aircannect::Memory::free(accumulator.samples);
    accumulator = {};
}

static bool write_current_record(Accumulator &accumulator) {
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                  "EDF accumulator writes require little-endian samples");
    if (!accumulator.initialized) return false;
    const size_t bytes = accumulator.sample_count * sizeof(int16_t);
    uint8_t *record = reinterpret_cast<uint8_t *>(accumulator.samples);
    const uint16_t crc = crc16_ccitt(record, bytes);
    memcpy(record + bytes, &crc, sizeof(crc));
    if (!append_output_record(accumulator.output, record, bytes + sizeof(crc)))
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

static bool advance_segment(uint32_t captured_ms);

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
    if (wave_clock.relative_ms >= segment_duration_ms)
        wave_clock.relative_ms = sample_ms;

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
        {"AlvMinVent.2s", "AAV"}, {"CLRatio.2s", "RCR"},
        {"TRRatio.2s", "RTR"},
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
    uint8_t record[40];
    size_t written = 0;
    if (!Air10Edf::render_annotation_record(
            *output.schema, onset, duration, label,
            record, sizeof(record), written)) {
        status_error("annotation encoding failed");
        return false;
    }
    return append_output_record(output, record, written);
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
    // CET counts seconds from noon; the calendar anchor starts at midnight.
    int32_t candidate = static_cast<int32_t>(event_time) -
                        static_cast<int32_t>((session_clock.native_start + 43200) % 86400);
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
    if (raw.len < 5 || !status.active || !advance_segment(raw.captured_ms)) return;
    if (int32_t(raw.captured_ms - session_start_capture_ms) < 0) {
        portENTER_CRITICAL(&status_mux);
        status.raw_dropped++;
        portEXIT_CRITICAL(&status_mux);
        return;
    }
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
        Log::logf(CAT_EDF, LOG_WARN, "%s subscribe failed\n", tag);
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
    aircannect::Memory::free(header_buffer);
    header_buffer = nullptr;
    header_capacity = 0;
}

static bool anchor_session_clock(const ControlEvent &event) {
    session_clock = {};
    session_clock.captured_ms = event.captured_ms;
    Air10Clock::Calendar native_now;
    uint32_t clock_ms = 0;
    session_clock.native_valid = Air10Clock::read(native_now, POLL_TIMEOUT_MS, &clock_ms);
    if (!session_clock.native_valid) {
        status_error("native clock unavailable");
        return false;
    }
    session_clock.native_start = Air10Clock::civil_seconds(native_now) -
        static_cast<uint32_t>(clock_ms - event.captured_ms) / 1000;
    const int64_t day = (session_clock.native_start - 43200) / 86400;
    if (day < 0x1000 || day >= 0xffff) {
        status_error("therapy day is outside Air10 range");
        return false;
    }
    session_native_day = static_cast<uint16_t>(day);
    return true;
}

static bool make_paths_and_metadata(const ControlEvent &event,
                                    uint16_t &mid, uint16_t &vid,
                                    bool rollover = false) {
    if (!rollover && !anchor_session_clock(event)) return false;
    const time_t civil = static_cast<time_t>(session_clock.native_start);
    struct tm start_tm;
    gmtime_r(&civil, &start_tm);
    snprintf(start_date, sizeof(start_date), "%02d.%02d.%02d",
             start_tm.tm_mday, start_tm.tm_mon + 1,
             (start_tm.tm_year + 1900) % 100);
    snprintf(start_time, sizeof(start_time), "%02d.%02d.%02d",
             start_tm.tm_hour, start_tm.tm_min, start_tm.tm_sec);

    const time_t day_civil = int64_t(Air10Clock::therapy_day(civil)) * 86400;
    struct tm therapy_tm;
    gmtime_r(&day_civil, &therapy_tm);
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
    if (rollover) {
        memcpy(srn, session_srn, sizeof(srn));
        mid = session_mid;
        vid = session_vid;
    } else {
        if (!read_variable("SRN", srn, sizeof(srn)) ||
            !read_variable("MID", mid_text, sizeof(mid_text)) ||
            !read_variable("VID", vid_text, sizeof(vid_text))) {
            status_error("device identity unavailable");
            return false;
        }
        uint32_t parsed = 0;
        mid = parse_hex_value(mid_text, strlen(mid_text), parsed)
                  ? static_cast<uint16_t>(parsed) : 0;
        vid = parse_hex_value(vid_text, strlen(vid_text), parsed)
                  ? static_cast<uint16_t>(parsed) : 0;
    }
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
    header_buffer = static_cast<uint8_t *>(aircannect::Memory::alloc_large(header_capacity));

    if (!header_buffer) return false;

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

static void discard_pending() {
    char path[80], part[88], backup[88];
    pending_path(status.file_prefix, path, sizeof(path));
    snprintf(part, sizeof(part), "%s.part", path);
    snprintf(backup, sizeof(backup), "%s.bak", path);
    if (storage->remove(path)) {
        portENTER_CRITICAL(&status_mux);
        if (status.pending_str) status.pending_str--;
        portEXIT_CRITICAL(&status_mux);
    }
    storage->remove(part);
    storage->remove(backup);
}

static bool begin_segment_files(const Air10Edf::Schema &brp_schema,
                                 const Air10Edf::Schema &pld_schema) {
    char path[80], part[88], backup[88];
    pending_path(status.file_prefix, path, sizeof(path));
    snprintf(part, sizeof(part), "%s.part", path);
    snprintf(backup, sizeof(backup), "%s.bak", path);
    if (storage->exists(path) || storage->exists(part) || storage->exists(backup))
        return false;
    // Persist intent before any EDF can be recovered and catalogued after reset.
    if (!save_pending()) {
        discard_pending();
        return false;
    }
    if (allocate_session_buffers(brp_schema, pld_schema)) return true;
    clear_session_memory(true);
    discard_pending();
    return false;
}

static void reset_session_state(const ControlEvent &event, bool rollover = false) {
    session_start_capture_ms = event.captured_ms;
    segment_duration_ms = Air10Clock::milliseconds_to_noon(session_clock.native_start);
    wave_clock = {};
    last_pld_slot = UINT32_MAX;
    last_oxi_slot = UINT32_MAX;
    if (!rollover) xQueueReset(raw_queue);
}

static void start_session(const ControlEvent &event) {
    __atomic_store_n(&therapy_start_pending, false, __ATOMIC_RELEASE);
    next_start_ms = millis() + 5000;
    if (status.active || !storage_ready || !__atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE) ||
        Arbiter::get_state() != SYS_THERAPY || Arbiter::get_cached_rop() != 1) return;
    portENTER_CRITICAL(&status_mux);
    const uint32_t mask_on_ms = therapy_on_capture_ms;
    portEXIT_CRITICAL(&status_mux);
    if (int32_t(event.captured_ms - mask_on_ms) < 0) return;
    if (!SdStorage::acquire()) {
        status_error("storage busy at recording start");
        return;
    }
    recording_storage_owned = true;
    uint16_t mid = 0;
    uint16_t vid = 0;
    portENTER_CRITICAL(&status_mux);
    status.last_error[0] = 0;
    portEXIT_CRITICAL(&status_mux);
    if (!make_paths_and_metadata(event, mid, vid)) {
        recording_storage_owned = false;
        SdStorage::release();
        return;
    }
    resolve_schemas(mid, vid);

    const StreamSchema *tce_schema = find_schema("TCE");
    const bool tcv = tce_schema && schema_has_field(*tce_schema, "TCV");
    const Air10Edf::Schema &brp_layout = Air10Edf::brp_schema(tcv);
    const Air10Edf::Schema &pld_layout = Air10Edf::pld_schema();

    reset_session_state(event);
    if (!begin_segment_files(brp_layout, pld_layout)) {
        status_error("session buffer or file initialization failed");
        clear_session_memory(true);
        recording_storage_owned = false;
        SdStorage::release();
        return;
    }

    portENTER_CRITICAL(&status_mux);
    const bool same_therapy = mask_on_ms == therapy_on_capture_ms;
    portEXIT_CRITICAL(&status_mux);
    if (!same_therapy || !__atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE) ||
        Arbiter::get_state() != SYS_THERAPY || Arbiter::get_cached_rop() != 1) {
        clear_session_memory(true);
        discard_pending();
        recording_storage_owned = false;
        SdStorage::release();
        return;
    }

    portENTER_CRITICAL(&status_mux);
    recording_therapy_on_ms = mask_on_ms;
    status.active = true;
    status.post_processing = false;
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
    Log::logf(CAT_EDF, LOG_INFO,
              "recording %s/%s MID=%u VID=%u\n",
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

static void close_segment() {
    bool complete = write_current_record(brp);
    complete = write_current_record(pld) && complete;
    complete = write_current_record(sad) && complete;
    update_record_status();

    // Attempt every finalization even after an earlier output failed.
    for (OutputFile *output : {&brp.output, &pld.output, &sad.output, &eve, &csl})
        complete = finalize_output(*output) && complete;
    update_record_status();
    Status finished;
    portENTER_CRITICAL(&status_mux);
    finished = status;
    portEXIT_CRITICAL(&status_mux);
    complete = complete && !finished.write_errors;
    clear_session_memory(false);

    EdfCatalog::Entry catalog_entry;
    complete = commit_session_catalog(false, false, catalog_entry) && complete;
    Log::logf(CAT_EDF, !complete ? LOG_ERROR : finished.raw_dropped ? LOG_WARN : LOG_INFO,
              "%s %s drops=%u\n",
              !complete ? "incomplete" : finished.raw_dropped ? "complete with gaps" : "complete",
              finished.file_prefix, finished.raw_dropped);
    Log::logf(CAT_EDF, LOG_DEBUG, "records BRP=%u PLD=%u SAD=%u EVE=%u CSL=%u\n",
              finished.brp_records, finished.pld_records,
              finished.sad_records, finished.eve_records, finished.csl_records);
}

static bool advance_segment(uint32_t captured_ms) {
    if (int32_t(captured_ms - session_start_capture_ms) < 0) return true;
    if (relative_ms(captured_ms) < segment_duration_ms) return true;

    const uint32_t boundary = session_start_capture_ms + segment_duration_ms;
    const uint32_t seconds = segment_duration_ms / 1000;
    close_segment();
    session_clock.native_start += seconds;
    session_clock.captured_ms = boundary;
    session_native_day = Air10Clock::therapy_day(session_clock.native_start);
    ControlEvent event = {ControlKind::Start, boundary};
    uint16_t mid = session_mid, vid = session_vid;
    const StreamSchema *schema = find_schema("TCE");
    const bool tcv = schema && schema_has_field(*schema, "TCV");
    const bool opened = make_paths_and_metadata(event, mid, vid, true) &&
        begin_segment_files(Air10Edf::brp_schema(tcv), Air10Edf::pld_schema());
    if (!opened) {
        capture_active = false;
        release_streams();
        clear_session_memory(true);
        portENTER_CRITICAL(&status_mux);
        status.active = false;
        portEXIT_CRITICAL(&status_mux);
        if (recording_storage_owned) SdStorage::release();
        recording_storage_owned = false;
        status_error("noon segment initialization failed");
        return false;
    }
    reset_session_state(event, true);
    Log::logf(CAT_EDF, LOG_INFO, "noon rollover %s/%s\n",
              status.therapy_day, status.file_prefix);
    return true;
}

static void stop_session(const ControlEvent &event) {
    if (!status.active) return;
    capture_active = false;
    release_streams();
    RawFrame raw;
    while (xQueueReceive(raw_queue, &raw, 0) == pdTRUE)
        process_raw_frame(raw);
    if (status.active) {
        // Do not create an empty next-day segment for a stop exactly at noon.
        if (relative_ms(event.captured_ms) > segment_duration_ms)
            (void)advance_segment(event.captured_ms - 1);
        if (status.active) close_segment();
    }

    portENTER_CRITICAL(&status_mux);
    status.active = false;
    // Keep clock synchronization out of the gap before the first STR check.
    status.post_processing = true;
    portEXIT_CRITICAL(&status_mux);
    SdStorage::refresh_usage();
    if (recording_storage_owned) SdStorage::release();
    recording_storage_owned = false;
    next_pending_ms = 0;
    next_start_ms = 0;
}

static bool refresh_str_day(uint16_t day, uint32_t generation, uint32_t device,
                             bool required) {
    const time_t civil = int64_t(day) * 86400;
    struct tm date;
    gmtime_r(&civil, &date);
    char day_text[9];
    snprintf(day_text, sizeof(day_text), "%04d%02d%02d",
             date.tm_year + 1900, date.tm_mon + 1, date.tm_mday);

    if (!SdStorage::try_acquire()) return false;
    EdfCatalog::Entry *entries = nullptr;
    uint32_t count = 0;
    const bool valid = EdfCatalog::snapshot_day(day_text, entries, count);
    SdStorage::release();
    if (!valid || !count) {
        aircannect::Memory::free(entries);
        return valid && !required;
    }
    uint32_t latest_index = 0;
    for (uint32_t i = 1; i < count; i++)
        if (strcmp(entries[i].file_prefix, entries[latest_index].file_prefix) > 0)
            latest_index = i;
    EdfCatalog::Entry &latest = entries[latest_index];

    session_native_day = day;
    uint8_t *record = nullptr;
    bool success = ensure_identification(device);
    bool summary_ready = success && collect_str_summary(record, generation);
    success = success && !post_processing_cancelled() &&
        device == __atomic_load_n(&device_generation, __ATOMIC_ACQUIRE);
    if (success && SdStorage::try_acquire()) {
        uint32_t revision = 0;
        bool catalog_changed = false;
        if (summary_ready && record) {
            summary_ready = update_str_file(record, latest);
            // A content token survives retries after STR was published but the
            // catalog was not. Consumers compare revisions, never order them.
            uint8_t identity[4];
            SdStorage::put_le32(identity, identification_crc);
            uint32_t crc = crc32_ieee_update(crc32_ieee_initial(), record,
                Air10Edf::record_size(Air10Edf::str_schema()));
            revision = crc32_ieee_finish(crc32_ieee_update(crc, identity, sizeof(identity)));
        }
        for (uint32_t i = 0; success && i < count; i++) {
            success = !post_processing_cancelled();
            if (!success) break;
            EdfCatalog::Entry &entry = entries[i];
            const uint8_t old_flags = entry.flags;
            const uint32_t old_revision = entry.str_revision;
            entry.flags |= EdfCatalog::ENTRY_IDENTIFICATION_READY;
            if (summary_ready && record) {
                entry.flags |= EdfCatalog::ENTRY_STR_READY;
                if (i == latest_index) entry.str_revision = revision;
            }
            if (old_flags != entry.flags || old_revision != entry.str_revision) {
                success = EdfCatalog::commit(entry);
                if (success) catalog_changed = true;
            }
        }
        for (uint32_t i = 0; success && summary_ready && i < count; i++) {
            char path[80];
            pending_path(entries[i].file_prefix, path, sizeof(path));
            if (storage->exists(path)) {
                success = storage->remove(path);
                if (success) {
                    portENTER_CRITICAL(&status_mux);
                    if (status.pending_str) status.pending_str--;
                    portEXIT_CRITICAL(&status_mux);
                }
            }
        }
        SdStorage::release();
        if (catalog_changed && summary_ready && record) {
            summary_export_entry = latest;
            summary_export_pending = true;
        }
        if (success && summary_ready && !record)
            Log::logf(CAT_EDF, LOG_INFO, "no stored STR for day=%04X ZEN=%lu\n",
                      day, static_cast<unsigned long>(generation));
    } else {
        success = false;
    }
    aircannect::Memory::free(entries);
    aircannect::Memory::free(record);
    return success && summary_ready;
}

// Caller holds storage access. This inventory does not depend on the catalog or UART.
static bool scan_pending(EdfPending::Record *selected = nullptr) {
    if (selected) *selected = {};
    uint32_t count = 0;
    const char *error = nullptr;
    errno = 0;
    fs::File dir = storage->open("/airbridge/pending");
    bool complete = dir ? dir.isDirectory() : errno == ENOENT;
    if (!complete) error = "STR pending directory unavailable";
    if (dir && dir.isDirectory()) {
        while (true) {
            errno = 0;
            fs::File item = dir.openNextFile();
            if (!item) {
                if (errno) {
                    complete = false;
                    error = "STR pending directory read failed";
                }
                break;
            }
            String path = item.path();
            item.close();
            if (!path.endsWith(".str")) continue;
            EdfPending::Record candidate;
            const PendingRead result = read_pending(path.c_str(), candidate);
            if (result == PendingRead::Invalid) {
                String rejected = path;
                rejected += ".rejected";
                if (!storage->exists(rejected) && storage->rename(path, rejected)) {
                    Log::logf(CAT_EDF, LOG_WARN, "STR journal rejected: %s -> %s\n",
                              path.c_str(), rejected.c_str());
                    continue;
                }
                count++;
                error = "STR journal quarantine failed";
                continue;
            }
            count++;
            if (result == PendingRead::Unavailable) {
                error = "STR pending journal read failed";
                continue;
            }
            if (!selected) continue;
            if (strcmp(candidate.prefix, pending_cursor) <= 0) continue;
            if (!selected->prefix[0] || strcmp(candidate.prefix, selected->prefix) < 0)
                *selected = candidate;
        }
    }
    dir.close();
    portENTER_CRITICAL(&status_mux);
    if (complete) status.pending_str = count;
    pending_scanned = complete;
    const bool new_error = error && error != pending_scan_error;
    pending_scan_error = error;
    portEXIT_CRITICAL(&status_mux);
    if (new_error) post_error(error);
    return complete;
}

static void request_summary_export() {
    if (!status.pending_str && summary_export_pending &&
        ExportSync::request_post_therapy(summary_export_entry))
        summary_export_pending = false;
}

static void sync_pending() {
    EdfPending::Record selected;
    if (!pending_scanned || status.pending_str) {
        if (!SdStorage::try_acquire()) return;
        const bool scanned = scan_pending(&selected);
        SdStorage::release();
        if (!scanned) return;
    }
    const uint32_t device = __atomic_load_n(&device_generation, __ATOMIC_ACQUIRE);
    uint32_t generation = 0, saved_day = 0, after = 0;
    if (!read_u32_variable("ZEN", generation)) return;
    const bool changed = !have_synced_generation || device != synced_device_generation ||
                         generation != synced_str_generation;
    if (!changed && !status.pending_str) {
        request_summary_export();
        return;
    }
    if (!read_u32_variable("SSD", saved_day) ||
        !read_u32_variable("ZEN", after) || after != generation) return;

    uint32_t mid = 0, vid = 0;
    bool success = read_variable("SRN", session_srn, sizeof(session_srn)) &&
                   read_u32_variable("MID", mid) && mid <= UINT16_MAX &&
                   read_u32_variable("VID", vid) && vid <= UINT16_MAX;
    session_mid = mid;
    session_vid = vid;
    if (selected.prefix[0]) {
        memcpy(pending_cursor, selected.prefix, sizeof(pending_cursor));
        if (!success || strcmp(session_srn, selected.srn)) {
            post_error("STR pending device unavailable or different");
            success = false;
        } else {
            success = refresh_str_day(selected.native_day, generation, device, true);
        }
    } else if (status.pending_str) {
        pending_cursor[0] = 0;
    }
    if (success && changed && saved_day >= 0x1000 && saved_day < 0xffff) {
        // SSD names the completed write. Revisit the preceding day only after
        // startup/restart, a day change or saves missed while UART was busy.
        const bool catch_up = !have_synced_generation || device != synced_device_generation ||
            saved_day != synced_saved_day || uint32_t(generation - synced_str_generation) != 1;
        const uint16_t days[] = {uint16_t(saved_day), uint16_t(saved_day - 1)};
        for (unsigned i = 0; i < (catch_up ? 2u : 1u); i++) {
            const uint16_t day = days[i];
            if (selected.prefix[0] && day == selected.native_day) continue;
            if (!refresh_str_day(day, generation, device, false)) {
                success = false;
                break;
            }
        }
    }
    success = success && read_u32_variable("ZEN", after) && after == generation &&
              device == __atomic_load_n(&device_generation, __ATOMIC_ACQUIRE) &&
              !post_processing_cancelled();
    if (success) {
        synced_str_generation = generation;
        synced_device_generation = device;
        synced_saved_day = saved_day;
        have_synced_generation = true;
    }
    if (success) request_summary_export();
}

static void process_pending() {
    if (status.active || post_processing_cancelled() ||
        (next_pending_ms && int32_t(millis() - next_pending_ms) < 0)) return;
    next_pending_ms = millis() + STR_GENERATION_POLL_MS;
    portENTER_CRITICAL(&status_mux);
    status.post_processing = true;
    portEXIT_CRITICAL(&status_mux);
    sync_pending();
    portENTER_CRITICAL(&status_mux);
    status.post_processing = false;
    portEXIT_CRITICAL(&status_mux);
}

static bool prepare_storage() {
    if (storage_ready) return true;
    if (next_storage_ms && int32_t(millis() - next_storage_ms) < 0) return false;
    const system_state_t state = Arbiter::get_state();
    if (state != SYS_IDLE && state != SYS_THERAPY) return false;
    next_storage_ms = millis() + 5000;
    if (!SdStorage::mounted()) SdStorage::init();
    if (!SdStorage::try_acquire()) return false;
    storage = SdStorage::filesystem();
    if (storage) {
        recover_pending();
        (void)scan_pending();
        recover_partial_outputs();
        recover_identification_files();
        recover_str_file();
        EdfCatalog::init();
        EdfCatalog::Status catalog;
        EdfCatalog::get_status(catalog);
        if (catalog.ready) {
            storage_ready = reconcile_catalog();
        }
    }
    SdStorage::release();
    if (storage_ready) ExportSync::init();
    return storage_ready;
}

static void retry_recording() {
    if (status.active || !storage_ready ||
        !__atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE) ||
        Arbiter::get_state() != SYS_THERAPY || Arbiter::get_cached_rop() != 1 ||
        (next_start_ms && int32_t(millis() - next_start_ms) < 0)) return;
    const ControlEvent event = {ControlKind::Start, millis()};
    start_session(event);
}

static void poll_recording_state() {
    const bool wanted = __atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE);
    if (Arbiter::get_state() != SYS_THERAPY || Arbiter::get_cached_rop() != 1) {
        next_recording_state_ms = 0;
        if (wanted) request_stop();
        return;
    }
    if (next_recording_state_ms && int32_t(millis() - next_recording_state_ms) < 0)
        return;
    next_recording_state_ms = millis() + RECORDING_STATE_POLL_MS;
    uint32_t zle = 0;
    if (!read_u32_variable("ZLE", zle) || zle > 1) return;
    // ROP may have changed while the queued read was in flight.
    if (Arbiter::get_state() != SYS_THERAPY || Arbiter::get_cached_rop() != 1)
        return;
    if (zle && !wanted) therapy_started();
    else if (!zle && wanted) request_stop();
}

static void recorder_task(void *) {
    while (true) {
        const bool have_storage = prepare_storage();
        if (have_storage) poll_recording_state();
        ControlEvent control;
        while (xQueueReceive(control_queue, &control, 0) == pdTRUE) {
            if (control.kind == ControlKind::Start) {
                if (have_storage) start_session(control);
            }
            else stop_session(control);
        }

        // The latest desired state survives a full control queue.
        if (status.active && !__atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE)) {
            portENTER_CRITICAL(&status_mux);
            control = latest_stop;
            portEXIT_CRITICAL(&status_mux);
            stop_session(control);
        }
        if (status.active) {
            portENTER_CRITICAL(&status_mux);
            const bool restarted = recording_therapy_on_ms != therapy_on_capture_ms;
            control = latest_stop;
            portEXIT_CRITICAL(&status_mux);
            if (restarted) stop_session(control);
        }
        retry_recording();

        RawFrame raw;
        if (xQueueReceive(raw_queue, &raw, pdMS_TO_TICKS(20)) == pdTRUE) {
            uint16_t processed = 0;
            do {
                if (status.active) process_raw_frame(raw);
            } while (++processed < raw_queue_capacity &&
                     __atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE) &&
                     xQueueReceive(raw_queue, &raw, 0) == pdTRUE);
        }
        if (uxQueueMessagesWaiting(raw_queue) == 0) {
            if (status.active) (void)advance_segment(millis());
            sample_pld();
            sample_oximetry();
        }
        if (status.active) update_record_status();
        else if (have_storage) process_pending();
    }
}

}  // namespace

void init() {
    if (status.ready) return;
    raw_queue_capacity = RAW_QUEUE_CAPACITY_PSRAM;
    raw_queue_storage = static_cast<uint8_t *>(aircannect::Memory::alloc_large(
        sizeof(RawFrame) * raw_queue_capacity, false));
    if (!raw_queue_storage) {
        raw_queue_capacity = RAW_QUEUE_CAPACITY_FALLBACK;
        raw_queue_storage = static_cast<uint8_t *>(aircannect::Memory::alloc_large(
            sizeof(RawFrame) * raw_queue_capacity));
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
        if (raw_queue) vQueueDelete(raw_queue);
        if (control_queue) vQueueDelete(control_queue);
        raw_queue = control_queue = nullptr;
        aircannect::Memory::free(raw_queue_storage);
        raw_queue_storage = nullptr;
        return;
    }
    for (LiveStream::internal_handle_t &lease : stream_leases) lease = -1;
    frame_listener = Arbiter::add_frame_listener(QFRAME_MASK_L,
                                                  frame_sink, nullptr);
    if (frame_listener < 0) {
        status_error("UART listener allocation failed");
        vQueueDelete(raw_queue); vQueueDelete(control_queue);
        raw_queue = control_queue = nullptr;
        aircannect::Memory::free(raw_queue_storage);
        raw_queue_storage = nullptr;
        return;
    }
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        recorder_task, "edf_rec", RECORDER_STACK, nullptr, 2,
        &recorder_task_handle, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS)
        created = xTaskCreatePinnedToCore(recorder_task, "edf_rec", RECORDER_STACK,
                                         nullptr, 2, &recorder_task_handle, 1);
    if (created != pdPASS) {
        Arbiter::remove_frame_listener(frame_listener);
        frame_listener = -1;
        status_error("task creation failed");
        vQueueDelete(raw_queue); vQueueDelete(control_queue);
        raw_queue = control_queue = nullptr;
        aircannect::Memory::free(raw_queue_storage);
        raw_queue_storage = nullptr;
        return;
    }

    portENTER_CRITICAL(&status_mux);
    status.ready = true;
    portEXIT_CRITICAL(&status_mux);
    Log::logf(CAT_EDF, LOG_INFO,
              "recorder ready queue=%u (%s)\n",
              raw_queue_capacity,
              raw_queue_capacity == RAW_QUEUE_CAPACITY_PSRAM ? "PSRAM" : "internal");
}

void therapy_started() {
    const uint32_t captured_ms = millis();
    portENTER_CRITICAL(&status_mux);
    therapy_on_capture_ms = captured_ms;
    __atomic_store_n(&therapy_wanted, true, __ATOMIC_RELEASE);
    __atomic_store_n(&therapy_start_pending, true, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&status_mux);
    if (!status.ready || !control_queue) return;
    ControlEvent event = {ControlKind::Start, captured_ms};
    if (xQueueSend(control_queue, &event, 0) != pdTRUE) {
        status_error("control queue full at therapy start");
    }
}

void request_stop() {
    ControlEvent event = {ControlKind::Stop, millis()};
    portENTER_CRITICAL(&status_mux);
    if (!__atomic_load_n(&therapy_wanted, __ATOMIC_ACQUIRE)) {
        portEXIT_CRITICAL(&status_mux);
        return;
    }
    latest_stop = event;
    __atomic_store_n(&therapy_start_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&therapy_wanted, false, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&status_mux);
    if (!status.ready || !control_queue) return;
    if (xQueueSend(control_queue, &event, 0) != pdTRUE)
        status_error("control queue full at therapy stop");
}

void device_restarted() {
    __atomic_add_fetch(&device_generation, 1, __ATOMIC_RELEASE);
    request_stop();
}

bool clock_write_allowed(const char **reason) {
    const bool mounted = SdStorage::mounted();
    portENTER_CRITICAL(&status_mux);
    const char *blocked = status.active ? "EDF recording active" :
        mounted && pending_scan_error ? pending_scan_error :
        mounted && !pending_scanned ? "STR pending journal not checked" :
        status.pending_str ? "STR pending" :
        status.post_processing ? "STR collection in progress" : nullptr;
    portEXIT_CRITICAL(&status_mux);
    if (reason) *reason = blocked;
    return !blocked;
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
void request_stop() {}
void device_restarted() {}
bool clock_write_allowed(const char **reason) {
    if (reason) *reason = nullptr;
    return true;
}

void get_status(Status &out) {
    out = {};
    out.supported = false;
}

}  // namespace EdfRecorder

#endif
