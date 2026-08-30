#pragma once

#include <stdint.h>

namespace EdfRecorder {

struct Status {
    bool supported;
    bool ready;
    bool active;
    bool post_processing;
    bool identification_ready;
    uint32_t started_epoch;
    uint32_t raw_frames;
    uint32_t raw_dropped;
    uint32_t write_errors;
    uint32_t brp_records;
    uint32_t pld_records;
    uint32_t sad_records;
    uint32_t eve_records;
    uint32_t csl_records;
    uint32_t str_records;
    uint32_t post_errors;
    uint32_t recovered_files;
    char therapy_day[9];
    char file_prefix[16];
    char last_error[64];
};

void init();
void therapy_started();
void therapy_ended();
void get_status(Status &out);

}  // namespace EdfRecorder
