#pragma once
#include <Arduino.h>

namespace OtaManager {
    enum class State : uint8_t {
        Disabled, Idle, Checking, Current, Available, Installing, Rebooting,
        Busy, Error,
    };

    struct Status {
        uint32_t revision;
        State state;
        const char *blocked;
        uint8_t progress;
        size_t bytes;
        size_t total_size;
        uint32_t last_check_age_ms;
        char update_version[48];
        char error[64];
    };

    void init();
    void handle();

    bool request_check(const char **error = nullptr);
    bool request_install(const char **error = nullptr);
    void config_changed();
    uint32_t revision();
    bool get_status(Status &status);
    const char *state_name(State state);

    bool begin_manual_upload();
    void end_manual_upload(bool success, const char *error = nullptr);
    bool begin_resmed_flash();
    void cancel_resmed_flash_claim();
    bool busy();
}
