#pragma once

#include "json_cursor.h"
#include "edf_report.h"
#include <stddef.h>
#include <stdint.h>

namespace SleepReport {

enum class Source : uint8_t { Device, Edf };
struct Publication {
    Source source = Source::Device;
    EdfReport::Status local;
};
using LocalRequest = EdfReport::Request;
using Result = EdfReport::Result;

// Capability selection never falls back from an SD build to UART on failure.
void init();
void tick();
Source default_source();
const char *source_name(Source source);
void get_publication(Publication &out);
bool submit(const LocalRequest &request, uint32_t &id,
            const char **error = nullptr);
int poll(uint32_t id, Result &out);
void cancel(uint32_t id);

enum class View : uint8_t { Day, Period };
struct Request {
    View view = View::Day;
    uint16_t selection = 0;  // Native epoch day (0 = SSD), or SEP index.
    bool operator==(const Request &other) const {
        return view == other.view && selection == other.selection;
    }
};

enum class Format : uint8_t { Number, Duration, DaysPeriod };
struct Field {
    const char *tag;
    const char *label;
    int16_t scale;
    uint8_t decimals;
    const char *unit;
    Format format = Format::Number;
};

constexpr uint32_t MISSING = UINT32_MAX;
constexpr size_t MAX_FIELDS = 16;
struct Snapshot {
    Request request;
    uint16_t day = 0, current_day = 0, days = 0;
    bool present = false;
    uint32_t values[MAX_FIELDS];
    uint16_t on[10], off[10];  // Native minutes from noon; 0xFFFF = missing.
    const char *error = nullptr;
};

// Resumable JSON over the immutable report pinned by its job.
class Cursor : private JsonCursor {
public:
    size_t read(const Snapshot &snapshot, char *out, size_t capacity);

private:
    bool next(const Snapshot &snapshot);
    uint8_t row_ = 0, stage_ = 0, option_ = 0;
    bool has_interval_ = false;
};

size_t json_length(const Snapshot &snapshot);

using Budget = uint16_t (*)(uint32_t reserve_ms);
bool valid(const Request &request);
const Field *fields(View view, size_t &count);
uint16_t period_days(uint16_t selection);
// Caller serializes report jobs. Standby only; therapy stops collection,
// except cleanup of a temporarily changed SEP. No persistent report cache.
int collect(Snapshot &snapshot, Request request, Budget budget);

}  // namespace SleepReport
