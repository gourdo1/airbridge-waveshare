#include "factory_reset.h"

#include "airbridge_ota.h"
#include "airsense_state.h"
#include "board.h"
#include "debug_log.h"
#include "resmed_ota.h"
#if AB_STORAGE_HAS_SDCARD
#include "sd_storage.h"
#endif

#include <nvs.h>
#include <nvs_flash.h>

namespace FactoryReset {
namespace {

constexpr const char *MARKER_NAMESPACE = "factory_reset";
constexpr const char *MARKER_KEY = "pending";
constexpr uint8_t MARKER_VALUE = 1;

const char *blocked() {
    if (!AirSenseState::local_background_allowed()) return "device_not_idle";
    if (ResmedOta::is_active()) return "resmed_ota_active";
    return nullptr;
}

bool prepare() {
    const char *error = blocked();
    if (error) {
        Log::logf(CAT_CONFIG, LOG_WARN, "Factory reset cancelled: %s\n", error);
        return false;
    }

    nvs_handle_t handle;
    esp_err_t cleanup = ESP_OK;
    esp_err_t result = nvs_open(MARKER_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, MARKER_KEY, MARKER_VALUE);
        if (result == ESP_OK) result = nvs_commit(handle);
        if (result != ESP_OK) {
            // NVS writes may persist before commit; disarm a failed request.
            cleanup = nvs_erase_key(handle, MARKER_KEY);
            if (cleanup == ESP_OK) cleanup = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        Log::logf(CAT_CONFIG, LOG_ERROR, "Factory reset marker save failed: %s\n",
                  esp_err_to_name(result));
        if (cleanup != ESP_OK && cleanup != ESP_ERR_NVS_NOT_FOUND) {
            Log::logf(CAT_CONFIG, LOG_ERROR,
                      "Factory reset fatal: marker cleanup failed: %s; reset may remain armed. "
                      "Restarting before normal work resumes\n", esp_err_to_name(cleanup));
            Log::poll();
            ESP.restart();
            for (;;) delay(1000);
        }
        return false;
    }
    Log::logf(CAT_CONFIG, LOG_WARN, "Factory reset armed for next boot\n");
    return true;
}

}  // namespace

bool request(const char **error) {
    const char *rejected = blocked();
    if (!rejected && !OtaManager::request_reboot(prepare))
        rejected = "prepared_reboot_unavailable";
    if (error) *error = rejected;
    return !rejected;
}

bool run_pending_on_boot() {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(MARKER_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;

    uint8_t marker = 0;
    if (result == ESP_OK) {
        result = nvs_get_u8(handle, MARKER_KEY, &marker);
        nvs_close(handle);
    }
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) {
        Log::logf(CAT_CONFIG, LOG_ERROR,
                  "Factory reset fatal: marker read failed: %s; pending intent unknown, boot halted\n",
                  esp_err_to_name(result));
        return false;
    }

    result = nvs_open(MARKER_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, MARKER_KEY);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        Log::logf(CAT_CONFIG, LOG_ERROR, "Factory reset marker consume failed: %s\n",
                  esp_err_to_name(result));
        if (marker == MARKER_VALUE) {
            Log::logf(CAT_CONFIG, LOG_ERROR,
                      "Factory reset fatal: armed intent could not be consumed; "
                      "boot halted before config/storage owners\n");
            return false;
        }
        return true;
    }
    if (marker != MARKER_VALUE) {
        Log::logf(CAT_CONFIG, LOG_WARN, "Unknown factory reset marker ignored\n");
        return true;
    }

#if AB_STORAGE_HAS_SDCARD
    if (!SdStorage::factory_format()) {
        Log::logf(CAT_CONFIG, LOG_ERROR,
                  "Factory reset failed: SD format failed; other NVS settings preserved. "
                  "SD data may be lost; explicit retry required\n");
        return true;
    }
#endif

    result = nvs_flash_erase();
    if (result != ESP_OK) {
        Log::logf(CAT_CONFIG, LOG_ERROR,
                  "Factory reset failed: NVS erase: %s; data may be lost. "
                  "Restarting; explicit retry required\n",
                  esp_err_to_name(result));
    } else {
        Log::logf(CAT_CONFIG, LOG_WARN, "Factory reset complete; restarting\n");
    }
    // Erase deinitializes NVS even on failure. Never start config owners here.
    Log::poll();
    ESP.restart();
    // Do not allow old configuration owners to run even if restart returns.
    for (;;) delay(1000);
}

}  // namespace FactoryReset
