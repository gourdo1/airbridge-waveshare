#include "sd_storage.h"

#include <Arduino.h>
#include <string.h>

#include "board.h"
#include "debug_log.h"

#if AB_STORAGE_HAS_SDCARD
#include <SD_MMC.h>
#endif

namespace SdStorage {

static Status status = {
    AB_STORAGE_HAS_SDCARD != 0,
    false,
    0,
    0,
    "",
};
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;

void init() {
#if AB_STORAGE_HAS_SDCARD
    if (status.mounted) return;

    bool pins_ok;
    if (AB_SDMMC_WIDTH == 1) {
        pins_ok = SD_MMC.setPins(AB_SDMMC_CLK_GPIO, AB_SDMMC_CMD_GPIO,
                                 AB_SDMMC_D0_GPIO);
    } else {
        pins_ok = SD_MMC.setPins(AB_SDMMC_CLK_GPIO, AB_SDMMC_CMD_GPIO,
                                 AB_SDMMC_D0_GPIO, AB_SDMMC_D1_GPIO,
                                 AB_SDMMC_D2_GPIO, AB_SDMMC_D3_GPIO);
    }
    if (!pins_ok) {
        strncpy(status.error, "pin configuration failed", sizeof(status.error) - 1);
        Log::logf(CAT_GENERAL, LOG_ERROR, "[SD] pin configuration failed\n");
        return;
    }

    const bool one_bit = AB_SDMMC_WIDTH == 1;
    if (!SD_MMC.begin("/sdcard", one_bit, false, AB_SDMMC_FREQ_KHZ, 8)) {
        strncpy(status.error, "mount failed", sizeof(status.error) - 1);
        Log::logf(CAT_GENERAL, LOG_ERROR, "[SD] mount failed\n");
        return;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        strncpy(status.error, "no card", sizeof(status.error) - 1);
        Log::logf(CAT_GENERAL, LOG_WARN, "[SD] no card detected\n");
        return;
    }

    status.mounted = true;
    status.card_bytes = SD_MMC.cardSize();
    status.used_bytes = SD_MMC.usedBytes();
    status.error[0] = 0;
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[SD] mounted width=%u freq=%ukHz size=%lluMB\n",
              AB_SDMMC_WIDTH, AB_SDMMC_FREQ_KHZ,
              static_cast<unsigned long long>(status.card_bytes / (1024 * 1024)));
#endif
}

bool mounted() {
    portENTER_CRITICAL(&status_mux);
    const bool value = status.mounted;
    portEXIT_CRITICAL(&status_mux);
    return value;
}

fs::FS *filesystem() {
#if AB_STORAGE_HAS_SDCARD
    return mounted() ? &SD_MMC : nullptr;
#else
    return nullptr;
#endif
}

void refresh_usage() {
#if AB_STORAGE_HAS_SDCARD
    if (!mounted()) return;
    const uint64_t used_bytes = SD_MMC.usedBytes();
    portENTER_CRITICAL(&status_mux);
    status.used_bytes = used_bytes;
    portEXIT_CRITICAL(&status_mux);
#endif
}

void get_status(Status &out) {
    portENTER_CRITICAL(&status_mux);
    out = status;
    portEXIT_CRITICAL(&status_mux);
}

}  // namespace SdStorage
