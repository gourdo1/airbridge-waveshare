#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Air10Edf {

enum class FileKind : uint8_t {
    BRP,
    PLD,
    SAD,
    EVE,
    CSL,
    STR,
};

struct SignalSpec {
    const char *label;
    const char *dimension;
    const char *physical_min;
    const char *physical_max;
    const char *digital_min;
    const char *digital_max;
    uint16_t samples_per_record;
};

struct Schema {
    FileKind kind;
    const char *suffix;
    const char *reserved;
    const SignalSpec *signals;
    uint8_t signal_count;
    uint16_t annotation_payload_bytes;
    uint32_t record_duration_seconds;
};

struct HeaderInfo {
    const char *recording_id;
    const char *start_date;
    const char *start_time;
    uint32_t record_count;
};

const Schema &brp_schema(bool include_tcv);
const Schema &pld_schema();
constexpr size_t PLD_MAX_SIGNALS = 16;  // 15 data channels and CRC.
// The caller owns the filtered signal array for the lifetime of the schema.
Schema pld_schema(bool include_int, bool include_ext,
                  SignalSpec (&signals)[PLD_MAX_SIGNALS]);
const Schema &sad_schema();
const Schema &eve_schema();
const Schema &csl_schema();
const Schema &str_schema();
const char *str_signal_tag(size_t signal_index);

size_t header_size(const Schema &schema);
size_t record_size(const Schema &schema);
size_t numeric_sample_count(const Schema &schema);

bool render_header(const Schema &schema, const HeaderInfo &info,
                   uint8_t *dst, size_t capacity, size_t &written);
bool update_record_count(uint8_t *header, size_t header_size,
                         uint32_t record_count);
bool render_numeric_record(const Schema &schema, const int16_t *samples,
                           size_t sample_count, uint8_t *dst,
                           size_t capacity, size_t &written);
bool render_annotation_record(const Schema &schema, uint32_t onset_seconds,
                              uint32_t duration_seconds, const char *label,
                              uint8_t *dst, size_t capacity, size_t &written);

}  // namespace Air10Edf
