#pragma once

#include <stdint.h>

namespace EdfRecorder {

struct Status {
    bool supported;
    bool ready;
    bool active;
    bool post_processing;
    bool identification_ready;
    uint32_t raw_dropped;
    uint32_t decode_errors;
    uint32_t sequence_gaps;
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

void init();
// Detailed capture follows ZLE; system standby/connection loss can also stop it.
void therapy_started();
void request_stop();
// Reserve the native clock until end_clock_write(); defer new STR collection.
bool begin_clock_write(const char **reason = nullptr);
void end_clock_write();
void get_status(Status &out);

}  // namespace EdfRecorder
