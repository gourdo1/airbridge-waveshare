#include "airbridge_ota.h"

#include "app_config.h"
#include "debug_log.h"
#include "export_sync.h"
#include "ota_release_manifest.h"
#include "ota_url_client.h"
#include "resmed_ota.h"
#include "oxi_arbiter.h"
#include "uart_arbiter.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern const char *airbridge_version();

namespace OtaManager {
namespace {

static constexpr size_t MANIFEST_MAX_BYTES = 4 * 1024;
static constexpr uint32_t INITIAL_CHECK_DELAY_MS = 60 * 1000;
static constexpr uint32_t CHECK_INTERVAL_MS = 24UL * 60 * 60 * 1000;
static constexpr uint32_t RETRY_INTERVAL_MS = 60UL * 60 * 1000;
static constexpr uint32_t REBOOT_DELAY_MS = 1500;
static constexpr uint32_t TASK_STACK_BYTES = 8192;

enum Operation : uint8_t {
    OP_NONE,
    OP_CHECK,
    OP_INSTALL,
    OP_MANUAL,
    OP_ARDUINO,
    OP_RESMED,
};

struct RuntimeStatus {
    bool initialized = false;
    bool enabled = false;
    bool checked = false;
    bool update_available = false;
    bool installable = false;
    bool reboot_pending = false;
    size_t bytes = 0;
    size_t total_size = 0;
    uint32_t last_check_ms = 0;
    uint32_t next_check_ms = 0;
    uint32_t reboot_at_ms = 0;
    uint32_t resmed_claimed_at_ms = 0;
    bool resmed_started = false;
    Operation operation = OP_NONE;
    char update_version[OtaRelease::VERSION_MAX] = {};
    char error[64] = {};
};

static RuntimeStatus runtime;
static SemaphoreHandle_t mutex = nullptr;
static char work_url[OtaRelease::URL_MAX] = {};
static OtaRelease::Artifact available_artifact;
static esp_ota_handle_t install_handle = 0;
static const esp_partition_t *install_partition = nullptr;

bool lock(TickType_t timeout = portMAX_DELAY) {
    return !mutex || xSemaphoreTakeRecursive(mutex, timeout) == pdTRUE;
}

void unlock() {
    if (mutex) xSemaphoreGiveRecursive(mutex);
}

void set_error_locked(const char *error) {
    snprintf(runtime.error, sizeof(runtime.error), "%s", error ? error : "error");
}

bool deadline_due(uint32_t now, uint32_t deadline) {
    return deadline && (int32_t)(now - deadline) >= 0;
}

void *allocate_manifest_buffer() {
    void *buffer = heap_caps_malloc(
        MANIFEST_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) buffer = heap_caps_malloc(MANIFEST_MAX_BYTES, MALLOC_CAP_8BIT);
    return buffer;
}

bool operation_allowed() {
    if (!lock(pdMS_TO_TICKS(50))) return false;
    bool allowed = runtime.operation == OP_CHECK;
    unlock();
    return allowed;
}

void finish_check(const OtaRelease::Manifest *manifest,
                  const OtaRelease::Artifact *artifact,
                  bool update_available, bool installable,
                  const char *error) {
    if (!lock()) return;
    if (runtime.operation != OP_CHECK) {
        unlock();
        return;
    }
    runtime.checked = error == nullptr;
    runtime.update_available = update_available;
    runtime.installable = installable;
    runtime.last_check_ms = millis();
    runtime.next_check_ms = runtime.last_check_ms +
        (error ? RETRY_INTERVAL_MS : CHECK_INTERVAL_MS);
    runtime.operation = OP_NONE;
    available_artifact = {};
    runtime.update_version[0] = '\0';
    runtime.error[0] = '\0';
    if (manifest) {
        snprintf(runtime.update_version, sizeof(runtime.update_version), "%s",
                 manifest->version);
    }
    if (artifact && update_available) available_artifact = *artifact;
    if (error) set_error_locked(error);
    unlock();
}

void check_task(void *) {
    char url[sizeof(work_url)] = {};
    if (lock()) {
        snprintf(url, sizeof(url), "%s", work_url);
        unlock();
    }

    uint8_t *buffer = (uint8_t *)allocate_manifest_buffer();
    if (!buffer) {
        finish_check(nullptr, nullptr, false, false, "manifest_alloc_failed");
        vTaskDelete(nullptr);
        return;
    }

    size_t length = 0;
    OtaUrl::Error transport_error;
    if (!OtaUrl::fetch(url, buffer, MANIFEST_MAX_BYTES, length,
                       transport_error,
                       [](void *) { return operation_allowed(); }, nullptr)) {
        heap_caps_free(buffer);
        finish_check(nullptr, nullptr, false, false,
                     transport_error.code[0] ? transport_error.code
                                             : "manifest_fetch_failed");
        vTaskDelete(nullptr);
        return;
    }

    OtaRelease::Manifest manifest;
    char manifest_error[OtaRelease::ERROR_MAX] = {};
    bool parsed = OtaRelease::parse_manifest(
        (char *)buffer, length, AB_OTA_RELEASE_TARGET,
        manifest, manifest_error);
    heap_caps_free(buffer);
    if (!parsed) {
        finish_check(nullptr, nullptr, false, false, manifest_error);
        vTaskDelete(nullptr);
        return;
    }

    bool newer = false;
    if (!OtaRelease::is_newer(airbridge_version(), manifest.version, newer)) {
        finish_check(&manifest, nullptr, false, false,
                     "current_version_invalid");
        vTaskDelete(nullptr);
        return;
    }

    OtaRelease::Artifact resolved = manifest.artifact;
    if (!OtaRelease::resolve_artifact_url(
            url, manifest.artifact.url, resolved.url, sizeof(resolved.url))) {
        finish_check(&manifest, nullptr, false, false,
                     "artifact_url_invalid");
        vTaskDelete(nullptr);
        return;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    bool installable = newer && partition && resolved.size <= partition->size;
    finish_check(&manifest, &resolved, newer, installable,
                 newer && !installable ? "artifact_too_large" : nullptr);
    Log::logf(CAT_OTA, LOG_INFO,
              "[OTA] Release check: current=%s latest=%s available=%d target=%s\n",
              airbridge_version(), manifest.version, newer,
              AB_OTA_RELEASE_TARGET);
    vTaskDelete(nullptr);
}

void abort_install(const char *error) {
    if (install_handle) {
        esp_ota_abort(install_handle);
        install_handle = 0;
    }
    install_partition = nullptr;

    if (lock()) {
        runtime.operation = OP_NONE;
        runtime.reboot_pending = false;
        runtime.reboot_at_ms = 0;
        set_error_locked(error);
        unlock();
    }
    Arbiter::set_state(SYS_IDLE);
    Log::logf(CAT_OTA, LOG_ERROR, "[OTA] Release install failed: %s\n",
              error ? error : "install_failed");
}

bool install_continue(void *) {
    if (!lock(pdMS_TO_TICKS(50))) return false;
    bool allowed = runtime.operation == OP_INSTALL &&
                   Arbiter::get_state() == SYS_OTA_ESP;
    unlock();
    return allowed;
}

bool install_write(void *, size_t offset, const uint8_t *data, size_t len) {
    if (!data || !len || !install_handle) return false;
    if (offset == 0 && data[0] != 0xe9) return false;

    if (!lock(pdMS_TO_TICKS(100))) return false;
    bool expected = runtime.operation == OP_INSTALL && offset == runtime.bytes &&
                    runtime.bytes <= runtime.total_size &&
                    len <= runtime.total_size - runtime.bytes;
    unlock();
    if (!expected) return false;

    if (esp_ota_write(install_handle, data, len) != ESP_OK) return false;

    if (lock()) {
        runtime.bytes += len;
        unlock();
    }
    return true;
}

void install_task(void *) {
    OtaRelease::Artifact artifact;
    if (lock()) {
        artifact = available_artifact;
        unlock();
    }

    // Let the main loop suspend live streams after entering the OTA state.
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!install_continue(nullptr)) {
        abort_install("install_cancelled");
        vTaskDelete(nullptr);
        return;
    }

    install_partition = esp_ota_get_next_update_partition(nullptr);
    if (!install_partition || artifact.size > install_partition->size) {
        abort_install("artifact_too_large");
        vTaskDelete(nullptr);
        return;
    }

    if (esp_ota_begin(install_partition, artifact.size, &install_handle) != ESP_OK) {
        abort_install("esp_ota_begin_failed");
        vTaskDelete(nullptr);
        return;
    }

    OtaUrl::Error transport_error;
    if (!OtaUrl::stream(artifact.url, artifact.size, install_write,
                        install_continue, nullptr, transport_error)) {
        abort_install(transport_error.code[0] ? transport_error.code
                                              : "firmware_fetch_failed");
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t result = esp_ota_end(install_handle);
    install_handle = 0;
    if (result != ESP_OK ||
        esp_ota_set_boot_partition(install_partition) != ESP_OK) {
        abort_install(result != ESP_OK ? "esp_ota_end_failed"
                                      : "set_boot_partition_failed");
        vTaskDelete(nullptr);
        return;
    }

    char partition_name[17] = {};
    snprintf(partition_name, sizeof(partition_name), "%s",
             install_partition->label);
    install_partition = nullptr;
    if (lock()) {
        runtime.operation = OP_NONE;
        runtime.reboot_pending = true;
        runtime.reboot_at_ms = millis() + REBOOT_DELAY_MS;
        runtime.error[0] = '\0';
        unlock();
    }
    Log::logf(CAT_OTA, LOG_INFO,
              "[OTA] Release %s installed to '%s', rebooting\n",
              runtime.update_version, partition_name);
    vTaskDelete(nullptr);
}

bool start_worker(TaskFunction_t function, const char *name) {
    BaseType_t result = xTaskCreatePinnedToCore(
        function, name, TASK_STACK_BYTES, nullptr, 1, nullptr, 0);
    return result == pdPASS;
}

bool background_work_idle() {
    if (OxiArbiter::is_feeding()) return false;
    ExportSync::Status smb;
    ExportSync::SleepHqStatus sleephq;
    ExportSync::get_status(smb);
    ExportSync::get_sleephq_status(sleephq);
    return smb.state != ExportSync::State::Pending &&
           smb.state != ExportSync::State::Working &&
           sleephq.state != ExportSync::State::Pending &&
           sleephq.state != ExportSync::State::Working;
}

}  // namespace

void init() {
    auto &cfg = Config::get();
    esp_ota_mark_app_valid_cancel_rollback();
    if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
    if (!lock()) return;
    runtime = {};
    runtime.initialized = cfg.wifi_mode != WIFI_MODE_OFF;
    runtime.enabled = runtime.initialized && cfg.update_url.length() > 0;
    runtime.next_check_ms = millis() + INITIAL_CHECK_DELAY_MS;
    unlock();

    if (!runtime.initialized) return;
    ArduinoOTA.setHostname(cfg.hostname.c_str());
    ArduinoOTA.setPort(DEFAULT_OTA_PORT);
    if (cfg.ota_password.length() > 0)
        ArduinoOTA.setPassword(cfg.ota_password.c_str());

    ArduinoOTA.onStart([]() {
        bool claimed = false;
        if (lock()) {
            if (runtime.operation == OP_NONE && !runtime.reboot_pending) {
                runtime.operation = OP_ARDUINO;
                runtime.bytes = 0;
                runtime.total_size = 0;
                runtime.error[0] = '\0';
                claimed = true;
            }
            unlock();
        }
        if (!claimed) {
            Log::logf(CAT_OTA, LOG_ERROR, "[OTA] ArduinoOTA collision\n");
            return;
        }
        Log::logf(CAT_OTA, LOG_INFO, "[OTA] ArduinoOTA start\n");
        Arbiter::set_state(SYS_OTA_ESP);
    });

    ArduinoOTA.onEnd([]() {
        Log::logf(CAT_OTA, LOG_INFO, "[OTA] ArduinoOTA complete\n");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        if (!lock(pdMS_TO_TICKS(10))) return;
        runtime.bytes = progress;
        runtime.total_size = total;
        unlock();
    });

    ArduinoOTA.onError([](ota_error_t error) {
        if (lock()) {
            runtime.operation = OP_NONE;
            snprintf(runtime.error, sizeof(runtime.error),
                     "arduino_ota_%u", (unsigned)error);
            unlock();
        }
        Arbiter::set_state(SYS_IDLE);
        Log::logf(CAT_OTA, LOG_ERROR, "[OTA] ArduinoOTA error %u\n", error);
    });

    ArduinoOTA.begin();
    Log::logf(CAT_OTA, LOG_INFO,
              "[OTA] Ready, release target=%s\n", AB_OTA_RELEASE_TARGET);
}

void handle() {
    bool initialized = false;
    bool poll_arduino = false;
    bool arduino_active = false;
    bool reboot = false;
    bool auto_check = false;
    if (lock(pdMS_TO_TICKS(10))) {
        initialized = runtime.initialized;
        poll_arduino = runtime.operation == OP_NONE ||
                       runtime.operation == OP_ARDUINO;
        arduino_active = runtime.operation == OP_ARDUINO;
        reboot = runtime.reboot_pending &&
                 deadline_due(millis(), runtime.reboot_at_ms);
        auto_check = runtime.enabled && runtime.operation == OP_NONE &&
                     !runtime.reboot_pending &&
                     deadline_due(millis(), runtime.next_check_ms);
        if (runtime.operation == OP_RESMED) {
            if (ResmedOta::is_active()) runtime.resmed_started = true;
            bool release_claim =
                (runtime.resmed_started && !ResmedOta::is_active()) ||
                (!runtime.resmed_started &&
                 millis() - runtime.resmed_claimed_at_ms >= 5000);
            if (release_claim) {
                runtime.operation = OP_NONE;
                if (!runtime.resmed_started)
                    set_error_locked("resmed_ota_start_failed");
                runtime.resmed_started = false;
            }
        }
        unlock();
    }
    if (!initialized) return;
    if (reboot) {
        delay(50);
        ESP.restart();
        return;
    }
    bool arduino_allowed = arduino_active ||
        (Arbiter::get_state() == SYS_IDLE && !ResmedOta::is_active() &&
         background_work_idle());
    if (poll_arduino && arduino_allowed && lock(pdMS_TO_TICKS(10))) {
        if (runtime.operation == OP_NONE || runtime.operation == OP_ARDUINO)
            ArduinoOTA.handle();
        unlock();
    }
    if (auto_check && WiFi.status() == WL_CONNECTED &&
        Arbiter::get_state() == SYS_IDLE && !ResmedOta::is_active() &&
        background_work_idle()) {
        if (!request_check() && lock(pdMS_TO_TICKS(10))) {
            if (deadline_due(millis(), runtime.next_check_ms))
                runtime.next_check_ms = millis() + RETRY_INTERVAL_MS;
            unlock();
        }
    }
}

bool request_check() {
    auto &cfg = Config::get();
    if (!mutex || !lock()) return false;
    runtime.enabled = runtime.initialized && cfg.update_url.length() > 0;
    if (!runtime.enabled) set_error_locked("update_check_disabled");
    else if (cfg.update_url.length() >= sizeof(work_url) ||
             !OtaUrl::supported(cfg.update_url.c_str()))
        set_error_locked("update_url_invalid");
    else if (runtime.operation != OP_NONE || runtime.reboot_pending)
        set_error_locked("ota_busy");
    else if (WiFi.status() != WL_CONNECTED)
        set_error_locked("network_unavailable");
    else if (Arbiter::get_state() != SYS_IDLE)
        set_error_locked("device_not_idle");
    else if (ResmedOta::is_active())
        set_error_locked("resmed_ota_active");
    else if (!background_work_idle())
        set_error_locked("background_work_active");
    else {
        snprintf(work_url, sizeof(work_url), "%s", cfg.update_url.c_str());
        runtime.operation = OP_CHECK;
        runtime.error[0] = '\0';
        unlock();
        if (start_worker(check_task, "ota_check")) return true;
        if (lock()) {
            runtime.operation = OP_NONE;
            set_error_locked("update_task_alloc_failed");
            unlock();
        }
        return false;
    }
    unlock();
    return false;
}

bool request_install() {
    if (!mutex || !lock()) return false;
    if (runtime.operation != OP_NONE || runtime.reboot_pending)
        set_error_locked("ota_busy");
    else if (!runtime.update_available || !runtime.installable ||
             !available_artifact.size)
        set_error_locked("update_not_available");
    else if (WiFi.status() != WL_CONNECTED)
        set_error_locked("network_unavailable");
    else if (Arbiter::get_state() != SYS_IDLE)
        set_error_locked("device_not_idle");
    else if (ResmedOta::is_active())
        set_error_locked("resmed_ota_active");
    else if (!background_work_idle())
        set_error_locked("background_work_active");
    else {
        runtime.operation = OP_INSTALL;
        runtime.bytes = 0;
        runtime.total_size = available_artifact.size;
        runtime.error[0] = '\0';
        Arbiter::set_state(SYS_OTA_ESP);
        unlock();
        if (start_worker(install_task, "ota_install")) return true;
        if (lock()) {
            runtime.operation = OP_NONE;
            set_error_locked("install_task_alloc_failed");
            unlock();
        }
        Arbiter::set_state(SYS_IDLE);
        return false;
    }
    unlock();
    return false;
}

void config_changed() {
    auto &cfg = Config::get();
    if (!lock()) return;
    if (runtime.operation == OP_CHECK) runtime.operation = OP_NONE;
    runtime.enabled = runtime.initialized && cfg.update_url.length() > 0;
    runtime.checked = false;
    runtime.update_available = false;
    runtime.installable = false;
    runtime.update_version[0] = '\0';
    runtime.error[0] = '\0';
    available_artifact = {};
    runtime.next_check_ms = millis() + INITIAL_CHECK_DELAY_MS;
    unlock();
}

void get_status(Status &status) {
    memset(&status, 0, sizeof(status));
    if (!lock(pdMS_TO_TICKS(50))) return;
    status.enabled = runtime.enabled;
    status.checking = runtime.operation == OP_CHECK;
    status.checked = runtime.checked;
    status.update_available = runtime.update_available;
    status.installable = runtime.installable;
    status.installing = runtime.operation == OP_INSTALL ||
                        runtime.operation == OP_MANUAL ||
                        runtime.operation == OP_ARDUINO;
    status.reboot_pending = runtime.reboot_pending;
    status.bytes = runtime.bytes;
    status.total_size = runtime.total_size;
    status.progress = runtime.total_size
        ? (uint8_t)min((size_t)100, runtime.bytes * 100 / runtime.total_size)
        : 0;
    status.last_check_age_ms = runtime.last_check_ms
        ? millis() - runtime.last_check_ms : 0;
    snprintf(status.release_target, sizeof(status.release_target), "%s",
             AB_OTA_RELEASE_TARGET);
    snprintf(status.update_version, sizeof(status.update_version), "%s",
             runtime.update_version);
    snprintf(status.error, sizeof(status.error), "%s", runtime.error);
    unlock();
}

bool begin_manual_upload() {
    if (!lock()) return false;
    bool allowed = runtime.initialized && runtime.operation == OP_NONE &&
                   !runtime.reboot_pending &&
                   Arbiter::get_state() == SYS_IDLE &&
                   !ResmedOta::is_active() && background_work_idle();
    if (allowed) {
        runtime.operation = OP_MANUAL;
        runtime.bytes = 0;
        runtime.total_size = 0;
        runtime.error[0] = '\0';
    } else {
        set_error_locked("ota_busy");
    }
    unlock();
    return allowed;
}

void end_manual_upload(bool success, const char *error) {
    if (!lock()) return;
    if (runtime.operation == OP_MANUAL) runtime.operation = OP_NONE;
    if (!success) set_error_locked(error ? error : "upload_failed");
    unlock();
}

bool begin_resmed_flash() {
    if (!lock()) return false;
    bool allowed = runtime.initialized && runtime.operation == OP_NONE &&
                   !runtime.reboot_pending &&
                   Arbiter::get_state() == SYS_IDLE &&
                   !ResmedOta::is_active() && background_work_idle();
    if (allowed) {
        runtime.operation = OP_RESMED;
        runtime.resmed_claimed_at_ms = millis();
        runtime.resmed_started = false;
        runtime.error[0] = '\0';
    } else {
        set_error_locked("ota_busy");
    }
    unlock();
    return allowed;
}

void cancel_resmed_flash_claim() {
    if (!lock()) return;
    if (runtime.operation == OP_RESMED && !runtime.resmed_started)
        runtime.operation = OP_NONE;
    unlock();
}

bool busy() {
    if (!lock(pdMS_TO_TICKS(20))) return true;
    bool result = runtime.operation != OP_NONE || runtime.reboot_pending;
    unlock();
    return result;
}

}  // namespace OtaManager
