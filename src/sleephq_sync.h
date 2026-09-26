#pragma once

#include <stddef.h>
#include <stdint.h>

#include "background_operation_control.h"
#include "edf_catalog.h"
#include "sleephq_protocol.h"
#include "sd_storage.h"

namespace SleepHqSync {

struct Config {
    char client_id[aircannect::AC_SLEEPHQ_SECRET_MAX];
    char client_secret[aircannect::AC_SLEEPHQ_SECRET_MAX];
    char team_id[aircannect::AC_SLEEPHQ_ID_MAX];
    char device_id[aircannect::AC_SLEEPHQ_ID_MAX];
};

struct Progress {
    uint32_t files_seen;
    uint32_t files_uploaded;
    uint32_t files_skipped;
    uint64_t bytes_uploaded;
    uint32_t import_id;
    char current_day[9];
    char import_status[aircannect::AC_SLEEPHQ_STATUS_MAX];
};

using ProgressCallback = void (*)(void *context, const Progress &progress);

bool configured(const Config &config);
bool complete(SdStorage::Session &storage, const Config &config,
              const EdfCatalog::Entry &entry);
bool sync_session(SdStorage::Session &storage, const Config &config,
                  const EdfCatalog::Entry &entry,
                  aircannect::BackgroundOperationControl &operation,
                  Progress &progress, ProgressCallback callback,
                  void *callback_context, char *error, size_t error_size);

}  // namespace SleepHqSync
