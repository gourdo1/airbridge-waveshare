#include "export_sync.h"

#include "board.h"

#if AB_STORAGE_HAS_SDCARD

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "background_operation_control.h"
#include "crc.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "sd_storage.h"
#include "sleephq_sync.h"
#include "storage_smb_client.h"
#include "tls_memory.h"
#include "uart_arbiter.h"

namespace ExportSync {
namespace {

using aircannect::BackgroundOperationControl;
using aircannect::BackgroundOperationTimeoutPolicy;
using aircannect::StorageSmbClient;
using aircannect::StorageSmbOperationResult;
using aircannect::StorageSmbRemoteStat;

constexpr size_t UPLOAD_BUFFER_PSRAM = 16 * 1024;
constexpr size_t UPLOAD_BUFFER_FALLBACK = 4 * 1024;
constexpr uint32_t OPERATION_IDLE_TIMEOUT_MS = 60000;
constexpr uint32_t SLEEPHQ_IDLE_TIMEOUT_MS = 5 * 60 * 1000UL;
constexpr uint32_t DNS_TIMEOUT_MS = 15000;
constexpr uint16_t EXPORT_TASK_STACK = 8192;

enum class RequestKind : uint8_t {
    PostTherapy,
    ManualSmb,
    ManualSleepHq,
};

struct Request {
    RequestKind kind;
    EdfCatalog::Entry entry;
    uint32_t generation;
};

struct RunContext {
    uint32_t generation;
    BackgroundOperationControl operation;
};

struct SmbConfig {
    char endpoint[161];
    char user[aircannect::AC_STORAGE_SMB_USER_MAX];
    char password[aircannect::AC_STORAGE_SMB_PASSWORD_MAX];
};

static Status status = {true, State::Disabled};
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static SleepHqStatus sleephq_status = {true, State::Disabled};
static portMUX_TYPE sleephq_status_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t request_queue = nullptr;
static TaskHandle_t export_task_handle = nullptr;
static SdStorage::Session storage;
static volatile uint32_t abort_generation = 1;
static uint32_t status_revision = 0;

static void publish_change() {
    __atomic_add_fetch(&status_revision, 1, __ATOMIC_RELEASE);
}

static bool generation_aborted(uint32_t generation) {
    return generation !=
               __atomic_load_n(&abort_generation, __ATOMIC_ACQUIRE) ||
           Arbiter::get_cached_rop() == 1;
}

static bool export_ready(const EdfCatalog::Entry &entry) {
    constexpr uint8_t required = EdfCatalog::ENTRY_LIVE_COMPLETE |
        EdfCatalog::ENTRY_IDENTIFICATION_READY | EdfCatalog::ENTRY_STR_READY;
    return (entry.flags & required) == required;
}

static void copy_text(char *dst, size_t capacity, const char *src) {
    if (!dst || !capacity) return;
    if (!src) src = "";
    strncpy(dst, src, capacity - 1);
    dst[capacity - 1] = 0;
}

static void set_state(State state, const char *error = nullptr) {
    portENTER_CRITICAL(&status_mux);
    status.state = state;
    if (error) copy_text(status.last_error, sizeof(status.last_error), error);
    else if (state != State::Error) status.last_error[0] = 0;
    portEXIT_CRITICAL(&status_mux);
    publish_change();
}

static void set_sleephq_state(State state, const char *error = nullptr) {
    portENTER_CRITICAL(&sleephq_status_mux);
    sleephq_status.state = state;
    if (error) {
        copy_text(sleephq_status.last_error,
                  sizeof(sleephq_status.last_error), error);
    } else if (state != State::Error) {
        sleephq_status.last_error[0] = 0;
    }
    portEXIT_CRITICAL(&sleephq_status_mux);
    publish_change();
}

static void publish_sleephq_progress(
    void *, const SleepHqSync::Progress &progress) {
    portENTER_CRITICAL(&sleephq_status_mux);
    sleephq_status.files_seen = progress.files_seen;
    sleephq_status.files_uploaded = progress.files_uploaded;
    sleephq_status.files_skipped = progress.files_skipped;
    sleephq_status.bytes_uploaded = progress.bytes_uploaded;
    sleephq_status.import_id = progress.import_id;
    copy_text(sleephq_status.current_day,
              sizeof(sleephq_status.current_day), progress.current_day);
    copy_text(sleephq_status.import_status,
              sizeof(sleephq_status.import_status), progress.import_status);
    portEXIT_CRITICAL(&sleephq_status_mux);
    publish_change();
}

static bool run_aborted(void *context) {
    const RunContext *run = static_cast<const RunContext *>(context);
    return !run || generation_aborted(run->generation) || !storage.valid();
}

static bool wait_resolve(StorageSmbClient &client, RunContext &run,
                         char *error, size_t error_size) {
    const uint32_t started = millis();
    while (true) {
        if (run_aborted(&run)) {
            copy_text(error, error_size, "preempted");
            return false;
        }
        const StorageSmbOperationResult result =
            client.resolve_host(error, error_size);
        if (result == StorageSmbOperationResult::Ready) return true;
        if (result == StorageSmbOperationResult::Error) return false;
        if (static_cast<uint32_t>(millis() - started) >= DNS_TIMEOUT_MS) {
            copy_text(error, error_size, "dns_timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool wait_connect(StorageSmbClient &client, RunContext &run,
                         char *error, size_t error_size) {
    while (true) {
        const StorageSmbOperationResult result =
            client.step_connect(error, error_size, &run.operation);
        if (result == StorageSmbOperationResult::Ready) return true;
        if (result == StorageSmbOperationResult::Error) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool wait_ensure_directory(StorageSmbClient &client,
                                  const char *path, RunContext &run,
                                  char *error, size_t error_size) {
    if (!path || !path[0]) return true;
    while (true) {
        const StorageSmbOperationResult result = client.step_ensure_directory(
            path, error, error_size, &run.operation);
        if (result == StorageSmbOperationResult::Ready) return true;
        if (result == StorageSmbOperationResult::Error) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool wait_stat(StorageSmbClient &client, const char *path,
                      StorageSmbRemoteStat &stat, RunContext &run,
                      char *error, size_t error_size) {
    while (true) {
        const StorageSmbOperationResult result =
            client.step_stat(path, stat, error, error_size, &run.operation);
        if (result == StorageSmbOperationResult::Ready) return true;
        if (result == StorageSmbOperationResult::Error) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool wait_open(StorageSmbClient &client, const char *path,
                      RunContext &run, char *error, size_t error_size) {
    while (true) {
        const StorageSmbOperationResult result = client.step_open_writer(
            path, error, error_size, &run.operation);
        if (result == StorageSmbOperationResult::Ready) return true;
        if (result == StorageSmbOperationResult::Error) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool wait_write(StorageSmbClient &client, const uint8_t *data,
                       size_t size, RunContext &run,
                       char *error, size_t error_size) {
    while (true) {
        size_t written = 0;
        const StorageSmbOperationResult result = client.step_write(
            data, size, written, error, error_size, &run.operation);
        if (result == StorageSmbOperationResult::Ready)
            return written == size;
        if (result == StorageSmbOperationResult::Error) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool wait_close(StorageSmbClient &client, RunContext &run,
                       char *error, size_t error_size) {
    while (true) {
        const StorageSmbOperationResult result = client.step_close_writer(
            error, error_size, &run.operation);
        if (result == StorageSmbOperationResult::Ready) return true;
        if (result == StorageSmbOperationResult::Error) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void disconnect(StorageSmbClient &client, RunContext &run) {
    for (uint16_t i = 0; i < 200; i++) {
        const StorageSmbOperationResult result =
            client.step_disconnect(&run.operation);
        if (result != StorageSmbOperationResult::Waiting) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    client.abort_connection();
}

static void note_seen() {
    portENTER_CRITICAL(&status_mux);
    status.files_seen++;
    portEXIT_CRITICAL(&status_mux);
    publish_change();
}

static void note_skipped() {
    portENTER_CRITICAL(&status_mux);
    status.files_skipped++;
    portEXIT_CRITICAL(&status_mux);
    publish_change();
}

static void note_uploaded(uint64_t bytes) {
    portENTER_CRITICAL(&status_mux);
    status.files_uploaded++;
    status.bytes_uploaded += bytes;
    portEXIT_CRITICAL(&status_mux);
    publish_change();
}

static bool sync_smb_file(StorageSmbClient &client, const char *local_path,
                          bool mutable_file, uint8_t *buffer,
                          size_t buffer_size, RunContext &run,
                          char *error, size_t error_size) {
    note_seen();
    SdStorage::Reader input;
    if (!input.open(storage, local_path)) {
        snprintf(error, error_size, "local_open:%s", local_path);
        return false;
    }
    const uint64_t local_size = input.size();
    char remote[aircannect::AC_STORAGE_SMB_REMOTE_PATH_MAX];
    if (!client.make_remote_path(local_path, remote, sizeof(remote))) {
        input.close();
        copy_text(error, error_size, "remote_path_too_long");
        return false;
    }

    StorageSmbRemoteStat remote_stat;
    if (!wait_stat(client, remote, remote_stat, run, error, error_size)) {
        input.close();
        return false;
    }
    if (!mutable_file && remote_stat.exists && !remote_stat.directory &&
        remote_stat.size == local_size) {
        input.close();
        note_skipped();
        return true;
    }

    char parent[aircannect::AC_STORAGE_SMB_REMOTE_PATH_MAX];
    copy_text(parent, sizeof(parent), remote);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (!wait_ensure_directory(client, parent, run, error, error_size)) {
            input.close();
            return false;
        }
    }
    if (!wait_open(client, remote, run, error, error_size)) {
        input.close();
        return false;
    }

    uint64_t sent = 0;
    bool success = true;
    while (sent < local_size) {
        if (run_aborted(&run)) {
            copy_text(error, error_size, "preempted");
            success = false;
            break;
        }
        const size_t remaining = static_cast<size_t>(local_size - sent);
        const size_t wanted = min(buffer_size, remaining);
        const size_t read = input.read(buffer, wanted);
        if (read != wanted ||
            !wait_write(client, buffer, read, run, error, error_size)) {
            if (read != wanted) copy_text(error, error_size, "local_read_short");
            success = false;
            break;
        }
        sent += read;
        run.operation.note_progress(millis());
        taskYIELD();
    }
    input.close();
    if (success) success = wait_close(client, run, error, error_size);
    if (!success) {
        client.abort_connection();
        return false;
    }

    if (!wait_stat(client, remote, remote_stat, run, error, error_size) ||
        !remote_stat.exists || remote_stat.directory ||
        remote_stat.size != local_size) {
        copy_text(error, error_size, "remote_verify_failed");
        return false;
    }
    note_uploaded(sent);
    Log::logf(CAT_EXPORT, LOG_INFO,
              "[SMB] uploaded %s bytes=%llu\n", local_path,
              static_cast<unsigned long long>(sent));
    return true;
}

static uint32_t endpoint_hash(const SmbConfig &config) {
    uint32_t crc = crc32_ieee_initial();
    crc = crc32_ieee_update(crc,
        reinterpret_cast<const uint8_t *>(config.endpoint),
        strlen(config.endpoint));
    const uint8_t newline = '\n';
    crc = crc32_ieee_update(crc, &newline, 1);
    crc = crc32_ieee_update(crc,
        reinterpret_cast<const uint8_t *>(config.user), strlen(config.user));
    return crc32_ieee_finish(crc);
}

static bool state_marker_path(const SmbConfig &config,
                              const EdfCatalog::Entry &entry,
                              char *directory, size_t directory_size,
                              char *path, size_t path_size) {
    const uint32_t hash = endpoint_hash(config);
    const int dir_len = snprintf(directory, directory_size,
                                 "/airbridge/smb_%08lx",
                                 static_cast<unsigned long>(hash));
    const int path_len = snprintf(path, path_size, "%s/%s.done",
                                  directory, entry.file_prefix);
    return dir_len > 0 && static_cast<size_t>(dir_len) < directory_size &&
           path_len > 0 && static_cast<size_t>(path_len) < path_size;
}

static bool write_state_marker(const SmbConfig &config,
                               const EdfCatalog::Entry &entry) {
    char directory[48];
    char path[80];
    char partial[88];
    if (!state_marker_path(config, entry, directory, sizeof(directory),
                           path, sizeof(path)) ||
        snprintf(partial, sizeof(partial), "%s.part", path) >=
            static_cast<int>(sizeof(partial))) {
        return false;
    }
    uint8_t marker[12] = {'A', 'B', 'S', 'M', 1, entry.flags, 0, 0};
    marker[8] = static_cast<uint8_t>(entry.finalized_epoch);
    marker[9] = static_cast<uint8_t>(entry.finalized_epoch >> 8);
    marker[10] = static_cast<uint8_t>(entry.finalized_epoch >> 16);
    marker[11] = static_cast<uint8_t>(entry.finalized_epoch >> 24);
    return storage.run([&](fs::FS &fs) {
        if (!fs.exists(directory) && !fs.mkdir(directory)) return false;
        fs.remove(partial);
        fs::File file = fs.open(partial, FILE_WRITE);
        const bool written = file && file.write(marker, sizeof(marker)) == sizeof(marker);
        if (file) {
            file.flush();
            file.close();
        }
        if (!written) {
            fs.remove(partial);
            return false;
        }
        if (fs.exists(path)) fs.remove(path);
        if (!fs.rename(partial, path)) {
            fs.remove(partial);
            return false;
        }
        return true;
    });
}

static bool state_marker_exists(const SmbConfig &config,
                                const EdfCatalog::Entry &entry) {
    char directory[48];
    char path[80];
    if (!state_marker_path(config, entry, directory, sizeof(directory),
                           path, sizeof(path))) return false;
    SdStorage::Reader file;
    if (!file.open(storage, path)) return false;
    uint8_t marker[12];
    const bool valid = file && file.size() == sizeof(marker) &&
        file.read(marker, sizeof(marker)) == sizeof(marker) &&
        memcmp(marker, "ABSM", 4) == 0 && marker[4] == 1 &&
        marker[5] == entry.flags &&
        (static_cast<uint32_t>(marker[8]) |
         static_cast<uint32_t>(marker[9]) << 8 |
         static_cast<uint32_t>(marker[10]) << 16 |
         static_cast<uint32_t>(marker[11]) << 24) == entry.finalized_epoch;
    if (file) file.close();
    return valid;
}

static bool sync_session_files(StorageSmbClient &client,
                               const EdfCatalog::Entry &entry,
                               uint8_t *buffer, size_t buffer_size,
                               RunContext &run,
                               char *error, size_t error_size) {
    static const char *const suffixes[] = {
        "BRP", "PLD", "SAD", "EVE", "CSL",
    };
    char path[128];
    for (const char *suffix : suffixes) {
        snprintf(path, sizeof(path), "/DATALOG/%s/%s_%s.edf",
                 entry.therapy_day, entry.file_prefix, suffix);
        if (!sync_smb_file(client, path, false, buffer, buffer_size,
                           run, error, error_size)) return false;
        snprintf(path, sizeof(path), "/DATALOG/%s/%s_%s.crc",
                 entry.therapy_day, entry.file_prefix, suffix);
        if (!sync_smb_file(client, path, false, buffer, buffer_size,
                           run, error, error_size)) return false;
    }
    return true;
}

static bool sync_root_files(StorageSmbClient &client,
                            uint8_t *buffer, size_t buffer_size,
                            RunContext &run,
                            char *error, size_t error_size) {
    static const char *const paths[] = {
        "/Identification.tgt", "/Identification.crc", "/STR.edf",
    };
    for (const char *path : paths) {
        bool exists = false;
        if (!storage.run([&](fs::FS &fs) { exists = fs.exists(path); return true; }))
            return false;
        if (!exists) continue;
        if (!sync_smb_file(client, path, true, buffer, buffer_size,
                           run, error, error_size)) return false;
    }
    return true;
}

static bool snapshot_smb_config(SmbConfig &out,
                                char *error, size_t error_size) {
    const AirBridgeConfig &config = Config::get();
    if (!config.smb_enabled) return false;
    if (config.smb_endpoint.isEmpty()) {
        copy_text(error, error_size, "endpoint_missing");
        return false;
    }
    if (config.smb_endpoint.length() >= sizeof(out.endpoint) ||
        config.smb_user.length() >= sizeof(out.user) ||
        config.smb_password.length() >= sizeof(out.password)) {
        copy_text(error, error_size, "configuration_too_long");
        return false;
    }
    copy_text(out.endpoint, sizeof(out.endpoint), config.smb_endpoint.c_str());
    copy_text(out.user, sizeof(out.user), config.smb_user.c_str());
    copy_text(out.password, sizeof(out.password), config.smb_password.c_str());
    return true;
}

static bool run_smb(const Request &request) {
    char error[64] = {};
    SmbConfig config = {};
    if (!snapshot_smb_config(config, error, sizeof(error))) {
        set_state(error[0] ? State::Error : State::Disabled,
                  error[0] ? error : nullptr);
        return !error[0];
    }
    set_state(State::Working);
    portENTER_CRITICAL(&status_mux);
    status.files_seen = 0;
    status.files_uploaded = 0;
    status.files_skipped = 0;
    status.bytes_uploaded = 0;
    copy_text(status.current_day, sizeof(status.current_day),
              request.kind == RequestKind::PostTherapy
                  ? request.entry.therapy_day : "");
    portEXIT_CRITICAL(&status_mux);
    publish_change();

    EdfCatalog::Status catalog;
    EdfCatalog::get_status(catalog);
    if (!catalog.ready) {
        set_state(State::Error, "catalog_not_ready");
        return false;
    }

    size_t buffer_size = UPLOAD_BUFFER_PSRAM;
    uint8_t *buffer = static_cast<uint8_t *>(aircannect::Memory::alloc_large(buffer_size, false));
    if (!buffer) {
        buffer_size = UPLOAD_BUFFER_FALLBACK;
        buffer = static_cast<uint8_t *>(aircannect::Memory::alloc_large(buffer_size));
    }
    if (!buffer) {
        set_state(State::Error, "upload_buffer_alloc");
        return false;
    }

    RunContext run = {};
    run.generation = request.generation;
    run.operation.started_ms = millis();
    run.operation.last_progress_ms = run.operation.started_ms;
    run.operation.timeout_ms = OPERATION_IDLE_TIMEOUT_MS;
    run.operation.timeout_policy = BackgroundOperationTimeoutPolicy::NoProgress;
    run.operation.should_abort = run_aborted;
    run.operation.ctx = &run;

    StorageSmbClient client;
    bool success = client.configure(config.endpoint, config.user,
                                    config.password, error, sizeof(error)) &&
                   wait_resolve(client, run, error, sizeof(error)) &&
                   wait_connect(client, run, error, sizeof(error));
    if (success) run.operation.note_progress(millis());

    if (success) {
        for (uint32_t i = catalog.entries; success && i > 0; i--) {
            EdfCatalog::Entry entry;
            if (!storage.run([&](fs::FS &) { return EdfCatalog::read(i - 1, entry); })) {
                copy_text(error, sizeof(error), "catalog_read");
                success = false;
                break;
            }
            if (!export_ready(entry) || state_marker_exists(config, entry)) continue;
            portENTER_CRITICAL(&status_mux);
            copy_text(status.current_day, sizeof(status.current_day),
                      entry.therapy_day);
            portEXIT_CRITICAL(&status_mux);
            publish_change();
            success = sync_session_files(client, entry, buffer, buffer_size,
                                         run, error, sizeof(error));
            if (success && !write_state_marker(config, entry)) {
                copy_text(error, sizeof(error), "state_marker_write");
                success = false;
            }
        }
        if (success)
            success = sync_root_files(client, buffer, buffer_size, run,
                                      error, sizeof(error));
    }

    if (client.connected()) disconnect(client, run);
    else client.abort_connection();
    heap_caps_free(buffer);
    if (!success) {
        if (run_aborted(&run)) set_state(State::Idle);
        else set_state(State::Error,
                       error[0] ? error : "smb_sync_failed");
        return false;
    }

    portENTER_CRITICAL(&status_mux);
    status.last_sync_epoch = static_cast<uint32_t>(time(nullptr));
    status.current_day[0] = 0;
    portEXIT_CRITICAL(&status_mux);
    publish_change();
    set_state(State::Idle);
    return true;
}

static bool snapshot_sleephq_config(SleepHqSync::Config &out,
                                    char *error, size_t error_size) {
    const AirBridgeConfig &config = Config::get();
    if (!config.sleephq_enabled) return false;
    if (config.sleephq_client_id.isEmpty() ||
        config.sleephq_client_secret.isEmpty()) {
        copy_text(error, error_size, "credentials_missing");
        return false;
    }
    if (config.sleephq_client_id.length() >= sizeof(out.client_id) ||
        config.sleephq_client_secret.length() >= sizeof(out.client_secret) ||
        config.sleephq_team_id.length() >= sizeof(out.team_id) ||
        config.sleephq_device_id.length() >= sizeof(out.device_id)) {
        copy_text(error, error_size, "configuration_too_long");
        return false;
    }
    copy_text(out.client_id, sizeof(out.client_id),
              config.sleephq_client_id.c_str());
    copy_text(out.client_secret, sizeof(out.client_secret),
              config.sleephq_client_secret.c_str());
    copy_text(out.team_id, sizeof(out.team_id),
              config.sleephq_team_id.c_str());
    copy_text(out.device_id, sizeof(out.device_id),
              config.sleephq_device_id.c_str());
    return true;
}

static bool run_sleephq(const Request &request) {
    char error[96] = {};
    SleepHqSync::Config config = {};
    if (!snapshot_sleephq_config(config, error, sizeof(error))) {
        set_sleephq_state(error[0] ? State::Error : State::Disabled,
                          error[0] ? error : nullptr);
        return !error[0];
    }
    if (!aircannect::TlsMemory::status().installed) {
        set_sleephq_state(State::Error, "tls_allocator_install");
        return false;
    }

    set_sleephq_state(State::Working);
    portENTER_CRITICAL(&sleephq_status_mux);
    sleephq_status.files_seen = 0;
    sleephq_status.files_uploaded = 0;
    sleephq_status.files_skipped = 0;
    sleephq_status.bytes_uploaded = 0;
    sleephq_status.import_id = 0;
    sleephq_status.import_status[0] = 0;
    copy_text(sleephq_status.current_day,
              sizeof(sleephq_status.current_day),
              request.kind == RequestKind::PostTherapy
                  ? request.entry.therapy_day : "");
    portEXIT_CRITICAL(&sleephq_status_mux);
    publish_change();

    EdfCatalog::Status catalog;
    EdfCatalog::get_status(catalog);
    if (!catalog.ready) {
        set_sleephq_state(State::Error, "catalog_not_ready");
        return false;
    }

    RunContext run = {};
    run.generation = request.generation;
    run.operation.started_ms = millis();
    run.operation.last_progress_ms = run.operation.started_ms;
    run.operation.timeout_ms = SLEEPHQ_IDLE_TIMEOUT_MS;
    run.operation.timeout_policy = BackgroundOperationTimeoutPolicy::NoProgress;
    run.operation.should_abort = run_aborted;
    run.operation.ctx = &run;

    SleepHqSync::Progress progress = {};
    bool success = true;
    for (uint32_t i = catalog.entries; success && i > 0; i--) {
        EdfCatalog::Entry entry;
        if (!storage.run([&](fs::FS &) { return EdfCatalog::read(i - 1, entry); })) {
            copy_text(error, sizeof(error), "catalog_read");
            success = false;
            break;
        }
        if (!export_ready(entry)) continue;
        success = SleepHqSync::sync_session(
            storage, config, entry, run.operation, progress,
            publish_sleephq_progress, nullptr, error, sizeof(error));
    }

    if (!success) {
        if (run_aborted(&run)) set_sleephq_state(State::Idle);
        else set_sleephq_state(State::Error,
                               error[0] ? error : "sleephq_sync_failed");
        return false;
    }

    portENTER_CRITICAL(&sleephq_status_mux);
    sleephq_status.last_sync_epoch = static_cast<uint32_t>(time(nullptr));
    sleephq_status.current_day[0] = 0;
    portEXIT_CRITICAL(&sleephq_status_mux);
    publish_change();
    set_sleephq_state(State::Idle);
    return true;
}

template <typename T>
static void run_logged(const Request &request, const char *name,
                        bool (*run)(const Request &), void (*snapshot)(T &)) {
    const uint32_t started = millis();
    Log::logf(CAT_EXPORT, LOG_INFO, "[%s] Sync started reason=%s\n", name,
              request.kind == RequestKind::PostTherapy ? "post_therapy" : "requested");
    bool success = run(request);
    T result;
    snapshot(result);
    if (!success && (generation_aborted(request.generation) || !storage.valid())) {
        Log::logf(CAT_EXPORT, LOG_INFO, "[%s] Sync interrupted by storage change or therapy\n", name);
    } else if (!success) {
        Log::logf(CAT_EXPORT, LOG_WARN, "[%s] Sync failed: %s\n", name, result.last_error);
    } else if (result.state == State::Disabled) {
        Log::logf(CAT_EXPORT, LOG_INFO, "[%s] Sync skipped: disabled\n", name);
    } else {
        Log::logf(CAT_EXPORT, LOG_INFO,
                  "[%s] Sync complete uploaded=%u skipped=%u bytes=%llu elapsed=%ums\n",
                  name, result.files_uploaded, result.files_skipped,
                  (unsigned long long)result.bytes_uploaded, (unsigned)(millis() - started));
    }
}

static void export_task(void *) {
    while (true) {
        Request request;
        if (xQueueReceive(request_queue, &request, portMAX_DELAY) != pdTRUE)
            continue;
        if (request.generation !=
            __atomic_load_n(&abort_generation, __ATOMIC_ACQUIRE)) {
            continue;
        }
        while ((WiFi.status() != WL_CONNECTED ||
                Arbiter::get_cached_rop() < 0) &&
               !generation_aborted(request.generation)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (generation_aborted(request.generation)) continue;

        bool admitted = false;
        while (!generation_aborted(request.generation)) {
            if (Arbiter::get_state() == SYS_IDLE &&
                Arbiter::get_cached_rop() == 0 && storage.begin()) {
                admitted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!admitted) continue;
        if (generation_aborted(request.generation)) { storage.end(); continue; }

        if (request.kind == RequestKind::ManualSmb) {
            run_logged(request, "SMB", run_smb, get_status);
        } else if (request.kind == RequestKind::ManualSleepHq) {
            run_logged(request, "SLEEPHQ", run_sleephq, get_sleephq_status);
        } else {
            const AirBridgeConfig &config = Config::get();
            if (config.smb_enabled && config.smb_auto_after_therapy)
                run_logged(request, "SMB", run_smb, get_status);
            if (!generation_aborted(request.generation) &&
                config.sleephq_enabled && config.sleephq_auto_after_therapy)
                run_logged(request, "SLEEPHQ", run_sleephq, get_sleephq_status);
        }
        storage.end();
    }
}

}  // namespace

const char *state_name(State value) {
    switch (value) {
        case State::Unsupported: return "unsupported";
        case State::Disabled: return "disabled";
        case State::Idle: return "idle";
        case State::Pending: return "pending";
        case State::Working: return "working";
        case State::Error: return "error";
    }
    return "unknown";
}

void init() {
    if (request_queue || !SdStorage::mounted()) return;
    request_queue = xQueueCreate(4, sizeof(Request));
    BaseType_t created = pdFAIL;
    if (request_queue && aircannect::Memory::psram_available()) {
        created = xTaskCreatePinnedToCoreWithCaps(
            export_task, "export", EXPORT_TASK_STACK, nullptr, 1,
            &export_task_handle, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (request_queue && created != pdPASS) {
        export_task_handle = nullptr;
        created = xTaskCreatePinnedToCore(
            export_task, "export", EXPORT_TASK_STACK, nullptr, 1,
            &export_task_handle, 0);
    }
    if (!request_queue || created != pdPASS) {
        set_state(State::Error, "task_initialization_failed");
        set_sleephq_state(State::Error, "task_initialization_failed");
        Log::logf(CAT_EXPORT, LOG_ERROR, "[EXPORT] Task initialization failed\n");
        return;
    }
    set_state(Config::get().smb_enabled ? State::Idle : State::Disabled);
    set_sleephq_state(Config::get().sleephq_enabled
                          ? State::Idle : State::Disabled);
    if (Config::get().smb_enabled &&
        Config::get().smb_auto_after_therapy) {
        (void)request_manual_smb();
    }
    if (Config::get().sleephq_enabled &&
        Config::get().sleephq_auto_after_therapy) {
        (void)request_manual_sleephq();
    }
}

void therapy_started() {
    __atomic_add_fetch(&abort_generation, 1, __ATOMIC_ACQ_REL);
    if (request_queue) xQueueReset(request_queue);
    set_state(Config::get().smb_enabled ? State::Idle : State::Disabled);
    set_sleephq_state(Config::get().sleephq_enabled
                          ? State::Idle : State::Disabled);
}

bool request_post_therapy(const EdfCatalog::Entry &entry) {
    const AirBridgeConfig &config = Config::get();
    const bool smb = config.smb_enabled && config.smb_auto_after_therapy;
    const bool sleephq = config.sleephq_enabled &&
                         config.sleephq_auto_after_therapy;
    if (!request_queue || Arbiter::get_cached_rop() == 1 ||
        (!smb && !sleephq)) {
        return false;
    }
    Request request = {RequestKind::PostTherapy, entry,
        __atomic_load_n(&abort_generation, __ATOMIC_ACQUIRE)};
    if (xQueueSend(request_queue, &request, 0) != pdTRUE) return false;
    if (smb) set_state(State::Pending);
    if (sleephq) set_sleephq_state(State::Pending);
    return true;
}

bool request_manual_smb() {
    if (!request_queue || Arbiter::get_cached_rop() == 1 ||
        !Config::get().smb_enabled) return false;
    Request request = {};
    request.kind = RequestKind::ManualSmb;
    request.generation =
        __atomic_load_n(&abort_generation, __ATOMIC_ACQUIRE);
    if (xQueueSend(request_queue, &request, 0) != pdTRUE) return false;
    set_state(State::Pending);
    return true;
}

bool request_manual_sleephq() {
    if (!request_queue || Arbiter::get_cached_rop() == 1 ||
        !Config::get().sleephq_enabled) return false;
    Request request = {};
    request.kind = RequestKind::ManualSleepHq;
    request.generation =
        __atomic_load_n(&abort_generation, __ATOMIC_ACQUIRE);
    if (xQueueSend(request_queue, &request, 0) != pdTRUE) return false;
    set_sleephq_state(State::Pending);
    return true;
}

uint32_t revision() {
    return __atomic_load_n(&status_revision, __ATOMIC_ACQUIRE);
}

void get_status(Status &out) {
    portENTER_CRITICAL(&status_mux);
    out = status;
    portEXIT_CRITICAL(&status_mux);
}

void get_sleephq_status(SleepHqStatus &out) {
    portENTER_CRITICAL(&sleephq_status_mux);
    out = sleephq_status;
    portEXIT_CRITICAL(&sleephq_status_mux);
}

}  // namespace ExportSync

#else

namespace ExportSync {

const char *state_name(State) { return "unsupported"; }
void init() {}
uint32_t revision() { return 0; }
void therapy_started() {}
bool request_post_therapy(const EdfCatalog::Entry &) { return false; }
bool request_manual_smb() { return false; }
bool request_manual_sleephq() { return false; }

void get_status(Status &out) {
    out = {};
    out.state = State::Unsupported;
}

void get_sleephq_status(SleepHqStatus &out) {
    out = {};
    out.state = State::Unsupported;
}

}  // namespace ExportSync

#endif
