#include "build_info.h"
#include "build_info_generated.h"

const char *airbridge_version() {
#ifdef FIRMWARE_MIGRATE
    return "MIGRATE-" AIRBRIDGE_VERSION;
#else
    return AIRBRIDGE_VERSION;
#endif
}

const char *airbridge_build_date() {
    return AIRBRIDGE_BUILD_DATE;
}
