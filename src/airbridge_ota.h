#pragma once
#include <Arduino.h>

namespace OtaManager {
    struct Status {
        bool enabled;
        bool checking;
        bool checked;
        bool update_available;
        bool installable;
        bool installing;
        bool reboot_pending;
        uint8_t progress;
        size_t bytes;
        size_t total_size;
        uint32_t last_check_age_ms;
        char release_target[40];
        char update_version[48];
        char error[64];
    };

    void init();
    void handle();

    bool request_check();
    bool request_install();
    void config_changed();
    void get_status(Status &status);

    bool begin_manual_upload();
    void end_manual_upload(bool success, const char *error = nullptr);
    bool begin_resmed_flash();
    void cancel_resmed_flash_claim();
    bool busy();
}
