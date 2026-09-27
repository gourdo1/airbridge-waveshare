#include "sleep_report.h"
#include <stdio.h>

namespace SleepReport {
namespace {
enum Stage : uint8_t {
    HEADER, ENTRY_START, LABEL, VALUE, DISPLAY_VALUE, UNITS, ENTRY_END,
    INTERVALS, DONE
};
}

bool Cursor::next(const Snapshot &report) {
    if (stage_ == HEADER) {
        switch (option_++) {
            case 0: field("{\"view\":", report.request.view == View::Day ? "day" : "period"); return true;
            case 1: integer(",\"day\":", report.day); return true;
            case 2: integer(",\"current_day\":", report.current_day); return true;
            case 3: integer(",\"period\":", report.request.view == View::Period ? period_days(report.request.selection) : 0); return true;
            case 4: integer(",\"days\":", report.days); return true;
            case 5: field(",\"present\":", report.present ? "true" : "false", false); return true;
            default: stage_ = ENTRY_START; token(",\"fields\":["); return true;
        }
    }
    size_t count;
    const Field *catalog = fields(report.request.view, count);
    if (row_ == count) {
        if (stage_ == DONE) return false;
        if (stage_ != INTERVALS) {
            stage_ = INTERVALS;
            option_ = 0;
            token("],\"intervals\":["); return true;
        }
        while (report.request.view == View::Day && option_ < 10) {
            uint16_t on = report.on[option_], off = report.off[option_];
            option_++;
            if (on == 0xFFFF && off == 0xFFFF) continue;
            snprintf(number_, sizeof(number_), "%s{\"on\":%d,\"off\":%d}",
                     has_interval_ ? "," : "", on == 0xFFFF ? -1 : on,
                     off == 0xFFFF ? -1 : off);
            has_interval_ = true;
            token(number_); return true;
        }
        stage_ = DONE; token("]}"); return true;
    }

    const Field &v = catalog[row_];
    int64_t raw = report.values[row_] == MISSING ? -1 : int64_t(report.values[row_]);
    switch (stage_) {
        case ENTRY_START:
            stage_ = LABEL;
            field(row_ ? ",{\"cmd\":" : "{\"cmd\":", v.tag); return true;
        case LABEL:
            stage_ = VALUE; field(",\"label\":", v.label); return true;
        case VALUE:
            stage_ = DISPLAY_VALUE; integer(",\"raw\":", raw); return true;
        case DISPLAY_VALUE:
            stage_ = UNITS;
            if (raw < 0) {
                field(",\"value\":", "--");
            } else if (v.format == Format::Duration || v.format == Format::DaysPeriod) {
                if (v.format == Format::Duration)
                    snprintf(number_, sizeof(number_), "%lld:%02lld",
                             (long long)(raw / 60), (long long)(raw % 60));
                else
                    snprintf(number_, sizeof(number_), "%lld/%u", (long long)raw, report.days);
                field(",\"value\":", number_);
            } else {
                decimal(",\"value\":", raw, v.scale, v.decimals);
            }
            return true;
        case UNITS:
            stage_ = ENTRY_END; field(",\"unit\":", v.unit); return true;
        case ENTRY_END:
            row_++; stage_ = ENTRY_START; token("}"); return true;
        default: return false;
    }
}

size_t Cursor::read(const Snapshot &snapshot, char *out, size_t capacity) {
    return read_tokens(out, capacity, [&] { return next(snapshot); });
}

size_t json_length(const Snapshot &snapshot) {
    Cursor counter;
    size_t count, length = 0;
    while ((count = counter.read(snapshot, nullptr, 512))) length += count;
    return length;
}

}  // namespace SleepReport
