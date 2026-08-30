#pragma once

#include <stdint.h>

#include "edf_catalog.h"

namespace ExportSync {

enum class State : uint8_t {
    Unsupported,
    Disabled,
    Idle,
    Pending,
    Working,
    Error,
};

struct Status {
    bool supported;
    State state;
    uint32_t files_seen;
    uint32_t files_uploaded;
    uint32_t files_skipped;
    uint64_t bytes_uploaded;
    uint32_t last_sync_epoch;
    char current_day[9];
    char last_error[64];
};

struct SleepHqStatus {
    bool supported;
    State state;
    uint32_t files_seen;
    uint32_t files_uploaded;
    uint32_t files_skipped;
    uint64_t bytes_uploaded;
    uint32_t import_id;
    uint32_t last_sync_epoch;
    char current_day[9];
    char import_status[32];
    char last_error[96];
};

void init();
void therapy_started();
bool request_post_therapy(const EdfCatalog::Entry &entry);
bool request_manual_smb();
bool request_manual_sleephq();
void get_status(Status &out);
void get_sleephq_status(SleepHqStatus &out);
const char *state_name(State state);

}  // namespace ExportSync
