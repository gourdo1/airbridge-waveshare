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

enum class CheckState : uint8_t { NotRun, Pending, Working, Passed, Failed };

struct CheckStatus {
    CheckState state;
    uint32_t config_revision;
    char error[96];
};

struct Backlog {
    bool known;
    uint32_t files;
    uint32_t sessions;
    uint32_t waiting_sessions;
    uint32_t config_revision;
    uint32_t catalog_generation;
    char error[48];
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
    CheckStatus check;
    Backlog backlog;
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
    CheckStatus check;
    Backlog backlog;
};

void init();
void therapy_started();
bool request_post_therapy(const EdfCatalog::Entry &entry);
bool request_manual_smb(bool check = false);
bool request_manual_sleephq(bool check = false);
void request_backlog_refresh(bool files_changed = false);
uint32_t revision();
void get_status(Status &out);
void get_sleephq_status(SleepHqStatus &out);
const char *state_name(State state);
const char *check_state_name(CheckState state);

}  // namespace ExportSync
