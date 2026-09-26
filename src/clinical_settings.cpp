#include "clinical_settings.h"
#include "hex_util.h"
#include <limits.h>
#include "json_util.h"
#include "clinical_jobs.h"
#include "settings_defs.h"
#include "uart_arbiter.h"
#include "memory_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ClinicalSettings {
namespace {
constexpr uint8_t VALID = 1, CUSTOM = 2;
const char *const GROUPS[] = {"therapy", "comfort", "accessories", "options", "configuration"};

struct ReportField {
    const char *cmd;
    const char *label;
    int16_t divisor;  // -61 minutes H:MM, -60 hours, -3 days/period, -1 I:E
    uint8_t decimals;
    const char *unit;
    bool summary;
};

const ReportField REPORT_FIELDS[] = {
    {"UQD", "Usage", -61, 0, "", false},
    {"OND", "Mask On Duration", -61, 0, "", false},
    {"AQD", "Events/hr", 10, 1, "/hr", false},
    {"MSP", "Median Pressure", 50, 1, "cmH2O", false},
    {"AIS", "AI (All)", 10, 1, "/hr", false},
    {"PM9", "Pressure P95", 50, 1, "cmH2O", false},
    {"OPI", "Central AI", 10, 1, "/hr", false},
    {"PMA", "Max Pressure", 50, 1, "cmH2O", false},
    {"CLI", "Obstructive AI", 10, 1, "/hr", false},
    {"AEP", "Avg EPR Pressure", 50, 1, "cmH2O", false},
    {"HIS", "Hypopnea Index", 10, 1, "/hr", false},
    {"LKM", "Leak Median", 50, 2, "L/s", false},
    {"UAI", "Unknown AI", 10, 1, "/hr", false},
    {"LK9", "Leak P95", 50, 2, "L/s", false},
    {"RIN", "RERA Index", 10, 1, "/hr", false},
    {"PHM", "Total Used Hrs", 0, 0, "hrs", true},
    {"LRD", "Leak", 10, 0, "L/min", true},
    {"DRD", "Days Used", -3, 0, "", true},
    {"ZAV", "Vt", 0, 0, "ml", true},
    {"VRD", "Days 4hrs+", -3, 0, "", true},
    {"ZAR", "RR", 5, 0, "bpm", true},
    {"WRD", "Avg. Usage", -60, 1, "hrs", true},
    {"ZAM", "MV", 8, 1, "L/min", true},
    {"XRD", "Used Hrs", -60, 1, "hrs", true},
    {"ZA2", "TgMV", 8, 1, "L/min", true},
    {"ZAI", "Pressure", 50, 1, "cmH2O", true},
    {"ZA3", "Va", 8, 1, "L/min", true},
    {"ZAE", "Exp. Pressure", 50, 1, "cmH2O", true},
    {"ZAZ", "Ti", 50, 2, "s", true},
    {"ARD", "AHI", 10, 1, "/hr", true},
    {"ZA1", "I:E", -1, 0, "", true},
    {"TRD", "Total AI", 10, 1, "/hr", true},
    {"ZAS", "Spont Trig", 2, 1, "%", true},
    {"CRD", "Central AI", 10, 1, "/hr", true},
    {"ZAY", "Spont Cyc", 2, 1, "%", true},
};

enum Stage : uint8_t {
    ARRAY_START, ENTRY_START, LABEL, GROUP, GROUP_PATH_START, GROUP_PATH_ITEM,
    VALUE, EDITABLE, SOURCE, TYPE,
    OPTIONS_START, OPTION_START, OPTION_VALUE, OPTION_LABEL, OPTION_END,
    SCALE, RAW_STEP, DECIMALS, RAW_MIN, RAW_MAX, UNITS, DISPLAY_VALUE, MINIMUM,
    MAXIMUM, STEP, ENTRY_END, DONE
};

}

Snapshot::~Snapshot() { reset(); }

void Snapshot::reset() {
    aircannect::Memory::free(values_);
    values_ = nullptr;
    count_ = 0;
    length_ = 0;
    error_ = nullptr;
    report_ = false;
    period_ = 0;
    metadata_.reset();
}

bool read_raw(const char *cmd, int &value) {
    uint16_t timeout = ClinicalJobs::timeout_ms();
    if (!timeout) return false;
    char text[9];
    uint32_t raw;
    if (!Arbiter::get_var(cmd, CMD_SRC_TCP, CMD_PRIO_NORMAL,
                          text, sizeof(text), timeout) ||
        !aircannect::parse_hex(text, strlen(text), raw) || raw > INT_MAX) return false;
    value = static_cast<int>(raw);
    return true;
}

bool known_stock(const char *cmd) { return var_lookup(cmd) != nullptr; }

int collect(Snapshot &snapshot, bool report) {
    snapshot.reset();
    snapshot.report_ = report;
    auto fail = [&](const char *error) {
        snapshot.reset();
        snapshot.error_ = error;
        return 503;
    };
    if (!report && !snapshot.metadata_.acquire())
        return fail("{\"error\":\"settings_metadata_unavailable\"}");
    int mop = 0;
    if (!report && !read_raw("MOP", mop)) return fail("{\"error\":\"settings_mode_unavailable\"}");
    if (mop < 0 || mop >= MODE_COUNT) mop = 0;

    // Count the selected layout first, then allocate exactly its compact values.
    auto layout = [&](bool read) {
        uint16_t count = 0;
        auto stock = [&](const char *name, uint8_t group) {
            if (snapshot.metadata_.find(name) >= 0) return;
            const var_def_t *v = var_lookup(name);
            if (!v) return;
            if (read) {
                int raw = mop;
                bool ok = strcmp(name, "MOP") == 0 || read_raw(name, raw);
                snapshot.values_[count] = {(uint32_t)raw, (uint16_t)(v - VAR_CATALOG),
                                          group, (uint8_t)(ok ? VALID : 0)};
            }
            count++;
        };
        auto list = [&](const char *const *names, uint8_t group) {
            for (; *names; names++) stock(*names, group);
        };
        for (uint8_t group = 0; group < 5; group++) {
            switch (group) {
                case 0: stock("MOP", 0); list(MODE_LAYOUT[mop], 0); break;
                case 1: list(COMFORT_LAYOUT[mop], 1); list(EPR_VARS, 1); break;
                case 2: list(ACCESSORY_VARS, 2); break;
                case 3: list(OPTION_VARS, 3); break;
                case 4: list(CONFIGURATION_VARS, 4); break;
            }
            for (uint16_t i = 0; i < snapshot.metadata_.count(); i++) {
                CustomSettings::entry_view_t entry;
                snapshot.metadata_.entry(i, entry);
                if (entry.category != group || !(entry.mop_mask & (1u << mop))) continue;
                if (read) {
                    uint32_t raw = 0;
                    bool ok = snapshot.metadata_.read_raw(i, raw);
                    snapshot.values_[count] = {raw, i, group, (uint8_t)(CUSTOM | (ok ? VALID : 0))};
                }
                count++;
            }
        }
        return count;
    };
    snapshot.count_ = report ? sizeof(REPORT_FIELDS) / sizeof(REPORT_FIELDS[0]) : layout(false);
    size_t bytes = snapshot.storage_bytes();
    if (bytes) {
        snapshot.values_ = static_cast<Value *>(aircannect::Memory::alloc_large(bytes));
        if (!snapshot.values_) return fail("{\"error\":\"settings_allocation_failed\"}");
    }
    if (report) {
        read_raw("URD", snapshot.period_);
        for (uint16_t i = 0; i < snapshot.count_; i++) {
            int raw = 0;
            bool ok = read_raw(REPORT_FIELDS[i].cmd, raw);
            snapshot.values_[i] = {(uint32_t)raw, i, 0, (uint8_t)(ok ? VALID : 0)};
        }
    } else layout(true);
    if (!report && snapshot.metadata_.generation() != CustomSettings::generation())
        return fail("{\"error\":\"settings_invalidated\"}");

    Cursor counter;
    size_t count;
    while ((count = counter.read(snapshot, nullptr, 512))) snapshot.length_ += count;
    return 200;
}

void Cursor::token(const char *text, bool quoted, size_t length) {
    text_ = text;
    length_ = length == SIZE_MAX ? strlen(text) : length;
    offset_ = 0;
    quoted_ = quoted;
    quote_phase_ = 0;
}

void Cursor::field(const char *key, const char *text, bool quoted) {
    token(key);
    pending_ = text;
    pending_quoted_ = quoted;
}

void Cursor::integer(const char *key, int64_t value) {
    snprintf(number_, sizeof(number_), "%lld", (long long)value);
    field(key, number_, false);
}

void Cursor::decimal(const char *key, int64_t raw, int16_t scale, uint8_t places) {
    uint64_t magnitude = raw < 0 ? -raw : raw;
    uint32_t divisor = scale > 0 ? scale : 1;
    if (scale < 0) magnitude *= -(int32_t)scale;
    size_t length = snprintf(number_, sizeof(number_), "%s%llu",
                              raw < 0 ? "-" : "",
                              (unsigned long long)(magnitude / divisor));
    uint32_t remainder = magnitude % divisor;
    if (places) number_[length++] = '.';
    uint8_t written = 0;
    while (written < places && length < sizeof(number_) - 2) {
        remainder *= 10;
        number_[length++] = '0' + remainder / divisor;
        remainder %= divisor;
        written++;
    }
    // Round exact rational values to nearest, ties to even, without newlib's
    // heap-backed floating-point formatting. Keep the bounded text buffer.
    if (written == places && (remainder * 2 > divisor ||
        (remainder * 2 == divisor && ((number_[length - 1] - '0') & 1)))) {
        size_t first = raw < 0 ? 1 : 0;
        size_t digit = length;
        bool carry = true;
        while (digit > first && carry) {
            char &c = number_[--digit];
            if (c == '.') continue;
            if (c == '9') c = '0';
            else { c++; carry = false; }
        }
        if (carry) {
            memmove(number_ + first + 1, number_ + first, length - first);
            number_[first] = '1';
            length++;
        }
    }
    number_[length] = '\0';
    field(key, number_);
}

bool Cursor::next(const Snapshot &snapshot) {
    if (pending_) {
        token(pending_, pending_quoted_);
        pending_ = nullptr;
        return true;
    }
    if (stage_ == ARRAY_START) { stage_ = ENTRY_START; token("["); return true; }
    if (row_ == snapshot.count_) {
        if (stage_ == DONE) return false;
        stage_ = DONE;
        token("]");
        return true;
    }

    const Value &value = snapshot.values_[row_];
    if (snapshot.report_) {
        const ReportField &v = REPORT_FIELDS[row_];
        int raw = value.flags & VALID ? (int)value.raw : -1;
        switch (stage_) {
            case ENTRY_START:
                stage_ = LABEL;
                field(row_ ? ",{\"cmd\":" : "{\"cmd\":", v.cmd); return true;
            case LABEL:
                stage_ = VALUE; field(",\"label\":", v.label); return true;
            case VALUE:
                stage_ = DISPLAY_VALUE; integer(",\"raw\":", raw); return true;
            case DISPLAY_VALUE:
                stage_ = UNITS;
                if (raw < 0 || (v.divisor == -1 && raw == 0)) {
                    field(",\"value\":", "--");
                } else if (v.divisor == -61 || v.divisor == -3) {
                    if (v.divisor == -61)
                        snprintf(number_, sizeof(number_), "%d:%02d", raw / 60, raw % 60);
                    else
                        snprintf(number_, sizeof(number_), "%d/%d", raw, snapshot.period_);
                    field(",\"value\":", number_);
                } else if (v.divisor == -1) {
                    decimal(",\"value\":", raw >= 100 ? raw : 100, raw >= 100 ? 100 : raw, 1);
                    if (raw >= 100) strcat(number_, ":1");
                    else {
                        memmove(number_ + 2, number_, strlen(number_) + 1);
                        memcpy(number_, "1:", 2);
                    }
                } else {
                    decimal(",\"value\":", raw,
                            v.divisor == -60 ? 60 : v.divisor > 0 ? v.divisor : 1,
                            v.decimals);
                }
                return true;
            case UNITS:
                stage_ = GROUP; field(",\"unit\":", v.unit); return true;
            case GROUP:
                stage_ = ENTRY_END;
                field(",\"section\":", v.summary ? "summary" : "session"); return true;
            case ENTRY_END:
                row_++; stage_ = ENTRY_START; token("}"); return true;
            default: return false;
        }
    }
    bool custom = value.flags & CUSTOM, valid = value.flags & VALID;
    CustomSettings::entry_view_t entry = {};
    const var_def_t *stock = nullptr;
    if (custom) snapshot.metadata_.entry(value.descriptor, entry);
    else stock = &VAR_CATALOG[value.descriptor];
    bool enumerated = custom ? entry.kind == CustomSettings::KIND_ENUM : stock->type == SET_ENUM;
    bool scaled = !custom && stock->type == SET_SCALED && stock->scale_div > 1;
    uint8_t decimals = custom ? entry.decimals : stock->decimals;

    while (true) {
        switch (stage_++) {
            case ENTRY_START:
                field(row_ ? ",{\"cmd\":" : "{\"cmd\":", custom ? entry.name : stock->cmd); return true;
            case LABEL: field(",\"label\":", custom ? entry.label : stock->label); return true;
            case GROUP: field(",\"group\":", GROUPS[value.group]); return true;
            case GROUP_PATH_START:
                if (!custom || !entry.group_count) { stage_ = VALUE; break; }
                option_ = 0;
                option_offset_ = 0;
                token(",\"groups\":["); return true;
            case GROUP_PATH_ITEM:
                if (option_ == entry.group_count) { token("]"); return true; }
                field(option_ ? "," : "", entry.groups + option_offset_);
                option_offset_ += strlen(entry.groups + option_offset_) + 1;
                option_++;
                stage_ = GROUP_PATH_ITEM;
                return true;
            case VALUE: integer(",\"value\":", valid ? (int64_t)value.raw : -1); return true;
            case EDITABLE: field(",\"editable\":", !custom || (entry.flags & 0x04) ? "true" : "false", false); return true;
            case SOURCE: if (custom) { field(",\"source\":", "custom"); return true; } break;
            case TYPE:
                field(",\"type\":", enumerated ? "enum" : custom ? "numeric" : scaled ? "scaled" : "int"); return true;
            case OPTIONS_START:
                if (enumerated) {
                    if (!custom && !stock->enum_options) { stage_ = ENTRY_END; break; }
                    option_ = 0;
                    option_offset_ = 0;
                    token(",\"options\":["); return true;
                }
                stage_ = custom || scaled ? SCALE : ENTRY_END;
                break;
            case OPTION_START:
                if ((custom && option_ == entry.option_count) || (!custom && option_offset_ == SIZE_MAX)) {
                    stage_ = ENTRY_END; token("]"); return true;
                }
                token(option_ ? (custom ? ",{" : ",") : (custom ? "{" : "")); return true;
            case OPTION_VALUE:
                if (custom) {
                    CustomSettings::option_view_t option;
                    snapshot.metadata_.option(value.descriptor, option_, option);
                    integer("\"value\":", option.value); return true;
                } else {
                    const char *start = stock->enum_options + option_offset_;
                    size_t length = strcspn(start, ",");
                    option_offset_ = start[length] ? option_offset_ + length + 1 : SIZE_MAX;
                    token(start, true, length); stage_ = OPTION_END; return true;
                }
            case OPTION_LABEL: {
                CustomSettings::option_view_t option;
                snapshot.metadata_.option(value.descriptor, option_, option);
                field(",\"label\":", option.label); return true;
            }
            case OPTION_END:
                option_++; stage_ = OPTION_START;
                if (custom) { token("}"); return true; }
                break;
            case SCALE: integer(custom ? ",\"scale\":" : ",\"scale_div\":", custom ? entry.scale : stock->scale_div); return true;
            case RAW_STEP: if (custom) { integer(",\"raw_step\":", entry.step); return true; } break;
            case DECIMALS: integer(",\"decimals\":", decimals); return true;
            case RAW_MIN: if (custom) { integer(",\"raw_min\":", entry.minimum); return true; } break;
            case RAW_MAX: if (custom) { integer(",\"raw_max\":", entry.maximum); return true; } break;
            case UNITS: if (custom) { field(",\"units\":", entry.units); return true; } break;
            case DISPLAY_VALUE:
                if (valid) {
                    decimal(",\"display\":", value.raw,
                            custom ? entry.scale : stock->scale_div, decimals); return true;
                }
                break;
            case MINIMUM:
                if (custom) { decimal(",\"min\":", entry.minimum, entry.scale, decimals); return true; } break;
            case MAXIMUM:
                if (custom) { decimal(",\"max\":", entry.maximum, entry.scale, decimals); return true; } break;
            case STEP:
                if (custom) {
                    decimal(",\"step\":", entry.step, entry.scale, decimals); return true;
                }
                if (valid) {
                    // Stock precision is 0, 1 or 2 decimal places.
                    snprintf(number_, sizeof(number_), "%s",
                             decimals == 0 ? "1" : decimals == 1 ? "0.1" : "0.01");
                    field(",\"step\":", number_); return true;
                }
                break;
            case ENTRY_END:
                row_++; stage_ = ENTRY_START; token("}"); return true;
            default: return false;
        }
    }
}

size_t Cursor::read(const Snapshot &snapshot, char *out, size_t capacity) {
    size_t written = 0;
    while (written < capacity) {
        char c;
        if (escape_) { c = escape_; escape_ = 0; }
        else if (!text_) {
            if (!next(snapshot)) break;
            continue;
        } else if (quoted_ && !quote_phase_) { c = '"'; quote_phase_ = 1; }
        else if (offset_ < length_) {
            c = text_[offset_++];
            if (quoted_) {
                char encoded[6];
                size_t count = aircannect::json_escape_char(c, encoded, true);
                if (!count) continue;
                c = encoded[0];
                if (count == 2) escape_ = encoded[1];
            }
        } else {
            text_ = nullptr;
            if (!quoted_) continue;
            c = '"';
        }
        if (out) out[written] = c;
        written++;
    }
    return written;
}

}  // namespace ClinicalSettings
