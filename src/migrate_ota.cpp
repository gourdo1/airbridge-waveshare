#ifdef FIRMWARE_MIGRATE

#include "airbridge_ota.h"
#include "app_config.h"
#include "debug_log.h"
#include "uart_arbiter.h"

#include <ArduinoOTA.h>
#include <esp_ota_ops.h>

namespace OtaManager {

void init() {
    auto &cfg = Config::get();
    esp_ota_mark_app_valid_cancel_rollback();
    ArduinoOTA.setHostname(cfg.hostname.c_str());
    ArduinoOTA.setPort(DEFAULT_OTA_PORT);
    if (cfg.ota_password.length() > 0)
        ArduinoOTA.setPassword(cfg.ota_password.c_str());

    ArduinoOTA.onStart([]() {
        Arbiter::set_state(SYS_OTA_ESP);
        Log::logf(CAT_OTA, LOG_INFO, "[MIG] OTA start\n");
    });
    ArduinoOTA.onEnd([]() {
        Log::logf(CAT_OTA, LOG_INFO, "[MIG] OTA complete\n");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Arbiter::set_state(SYS_IDLE);
        Log::logf(CAT_OTA, LOG_ERROR, "[MIG] OTA error %u\n", error);
    });
    ArduinoOTA.begin();
}

void handle() {
    ArduinoOTA.handle();
}

}  // namespace OtaManager

#endif
