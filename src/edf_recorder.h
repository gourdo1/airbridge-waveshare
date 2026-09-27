#pragma once

#include <stdint.h>
#include "air10_edf.h"

namespace EdfRecorder {

struct Status {
    bool supported;
    bool ready;
    bool active;
    bool post_processing;
    bool identification_ready;
    uint32_t raw_dropped;
    uint32_t write_errors;
    uint32_t brp_records;
    uint32_t pld_records;
    uint32_t sad_records;
    uint32_t eve_records;
    uint32_t csl_records;
    uint32_t str_records;
    uint32_t post_errors;
    uint32_t pending_str;
    char therapy_day[9];
    char file_prefix[16];
    char last_error[64];
};

struct FileProgress {
    const Air10Edf::Schema *schema;
    uint32_t records;
    uint64_t readable_bytes;
};

// A frozen description, not permission to open live files. Only complete,
// flushed records are included; the on-card header still has count zero.
struct Progress {
    uint32_t revision;
    uint32_t segment;
    bool active;
    char directory[40];
    char prefix[16];
    char recording_id[81];
    char start_date[9];
    char start_time[9];
    FileProgress files[5];
};

void init();
// Detailed capture follows ZLE; system standby/connection loss can also stop it.
void therapy_started();
void therapy_ended();
void device_restarted();
bool clock_write_allowed(const char **reason = nullptr);
void get_status(Status &out);
void get_progress(Progress &out);

}  // namespace EdfRecorder
