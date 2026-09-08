#pragma once

#include <Preferences.h>
#include <nvs.h>

// Missing optional namespaces are normal before the first saved setting.
inline bool open_optional_preferences(Preferences &prefs, const char *name) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(name, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err == ESP_OK) nvs_close(handle);
    return prefs.begin(name, true);
}
