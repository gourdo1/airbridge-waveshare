#include "debug_log.h"
#include "memory_manager.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <Preferences.h>
#include "nvs_optional.h"
#include <stdarg.h>

static Preferences log_prefs;

static Print *outputs[LOG_MAX_OUTPUTS] = { &Serial };
static int output_count = 1;
static SemaphoreHandle_t log_mutex = nullptr;
static log_level_t cat_levels[CAT_COUNT];

static constexpr size_t SYSLOG_QUEUE_DEPTH = 8;
static constexpr size_t SYSLOG_SEND_BUDGET = 4;

struct SyslogRecord {
    uint8_t cat;
    uint8_t level;
    char text[128];
};

static SyslogRecord *syslog_queue = nullptr;
static size_t syslog_head = 0;
static size_t syslog_count = 0;
static sockaddr_in syslog_remote = {};
static char syslog_hostname[64] = {};

void Log::init() {
    log_mutex = xSemaphoreCreateMutex();
    bool stored = open_optional_preferences(log_prefs, "log_levels");
    for (int i = 0; i < CAT_COUNT; i++) {
        cat_levels[i] = stored ? (log_level_t)log_prefs.getUChar(
            Log::cat_name((log_cat_t)i), LOG_INFO) : LOG_INFO;
    }
    log_prefs.end();
}

bool Log::configure_syslog(bool enabled, const char *host, uint16_t port,
                           const char *hostname) {
    if (!log_mutex) return false;

    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    bool valid = !enabled || !*host ||
                 (port && inet_pton(AF_INET, host, &remote.sin_addr) == 1);
    enabled = enabled && *host && valid;

    char name[sizeof(syslog_hostname)];
    strlcpy(name, *hostname ? hostname : "airbridge", sizeof(name));
    for (char *p = name; *p; p++) {
        if ((uint8_t)*p < 33 || (uint8_t)*p > 126) *p = '_';
    }

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    if (!enabled) {
        aircannect::Memory::free(syslog_queue);
        syslog_queue = nullptr;
        syslog_head = syslog_count = 0;
    } else {
        if (!syslog_queue) {
            syslog_queue = static_cast<SyslogRecord *>(
                aircannect::Memory::alloc_large(
                    SYSLOG_QUEUE_DEPTH * sizeof(SyslogRecord)));
        }
        if (syslog_remote.sin_addr.s_addr != remote.sin_addr.s_addr ||
            syslog_remote.sin_port != remote.sin_port ||
            strcmp(syslog_hostname, name) != 0) {
            syslog_head = syslog_count = 0;
        }
        syslog_remote = remote;
        strlcpy(syslog_hostname, name, sizeof(syslog_hostname));
        valid = syslog_queue != nullptr;
    }
    xSemaphoreGive(log_mutex);
    return valid;
}

void Log::poll() {
    // Only the loop task owns this socket; producers never touch the network.
    static int fd = -1;
    if (!log_mutex || xSemaphoreTake(log_mutex, 0) != pdTRUE) return;
    bool enabled = syslog_queue != nullptr;
    xSemaphoreGive(log_mutex);
    bool network_ready = enabled &&
                         (WiFi.isConnected() || WiFi.softAPgetStationNum() > 0);
    for (size_t i = 0; i < SYSLOG_SEND_BUDGET; i++) {
        if (xSemaphoreTake(log_mutex, 0) != pdTRUE) return;
        if (!syslog_queue || !network_ready) {
            syslog_head = syslog_count = 0;
            xSemaphoreGive(log_mutex);
            if (fd >= 0) close(fd);
            fd = -1;
            return;
        }
        if (!syslog_count) {
            xSemaphoreGive(log_mutex);
            return;
        }

        SyslogRecord record = syslog_queue[syslog_head];
        syslog_head = (syslog_head + 1) % SYSLOG_QUEUE_DEPTH;
        syslog_count--;
        sockaddr_in remote = syslog_remote;
        char hostname[sizeof(syslog_hostname)];
        memcpy(hostname, syslog_hostname, sizeof(hostname));
        xSemaphoreGive(log_mutex);

        static const uint8_t severity[] = {3, 4, 6, 7};
        unsigned pri = 16 * 8 + (record.level <= LOG_DEBUG
                                 ? severity[record.level] : 6);
        char payload[256];
        int len = snprintf(payload, sizeof(payload),
                           "<%u>1 - %s airbridge - %s - %s", pri, hostname,
                           cat_name((log_cat_t)record.cat), record.text);

        if (fd < 0) fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) return;
        if (sendto(fd, payload, len, MSG_DONTWAIT,
                   (sockaddr *)&remote, sizeof(remote)) < 0) {
            close(fd);
            fd = -1;
            return;
        }
    }
}

static void save_levels() {
    log_prefs.begin("log_levels", false);
    for (int i = 0; i < CAT_COUNT; i++)
        log_prefs.putUChar(Log::cat_name((log_cat_t)i), (uint8_t)cat_levels[i]);
    log_prefs.end();
}

void Log::set_level(log_level_t lvl) {
    for (int i = 0; i < CAT_COUNT; i++)
        cat_levels[i] = lvl;
    save_levels();
}

log_level_t Log::get_level() {
    return cat_levels[CAT_GENERAL];
}

void Log::set_cat_level(log_cat_t cat, log_level_t lvl) {
    if (cat < CAT_COUNT) {
        cat_levels[cat] = lvl;
        log_prefs.begin("log_levels", false);
        log_prefs.putUChar(Log::cat_name(cat), (uint8_t)lvl);
        log_prefs.end();
    }
}

log_level_t Log::get_cat_level(log_cat_t cat) {
    return (cat < CAT_COUNT) ? cat_levels[cat] : LOG_INFO;
}

const char *Log::level_name(log_level_t lvl) {
    switch (lvl) {
        case LOG_ERROR: return "ERROR";
        case LOG_WARN:  return "WARN";
        case LOG_INFO:  return "INFO";
        case LOG_DEBUG: return "DEBUG";
        default:        return "?";
    }
}

const char *Log::cat_name(log_cat_t cat) {
    switch (cat) {
        case CAT_GENERAL: return "GENERAL";
        case CAT_OXI:     return "OXI";
        case CAT_TCP:     return "TCP";
        case CAT_WIFI:    return "WIFI";
        case CAT_OTA:     return "OTA";
        case CAT_WEB:     return "WEB";
        case CAT_ARB:     return "ARB";
        case CAT_HEALTH:  return "HEALTH";
        case CAT_EXPORT:  return "EXPORT";
        default:          return "?";
    }
}

void Log::add_output(Print *out) {
    if (!log_mutex) return;
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    if (output_count < LOG_MAX_OUTPUTS) {
        outputs[output_count++] = out;
    }
    xSemaphoreGive(log_mutex);
}

void Log::remove_output(Print *out) {
    if (!log_mutex) return;
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    for (int i = 1; i < output_count; i++) {
        if (outputs[i] == out) {
            for (int j = i; j < output_count - 1; j++)
                outputs[j] = outputs[j + 1];
            output_count--;
            break;
        }
    }
    xSemaphoreGive(log_mutex);
}

static void log_dispatch(log_cat_t cat, log_level_t lvl,
                         const char *fmt, va_list args) {
    char buf[128];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < output_count; i++) {
            if (outputs[i]) outputs[i]->write((const uint8_t*)buf, len);
        }
        if (syslog_queue && syslog_count < SYSLOG_QUEUE_DEPTH) {
            while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n'))
                buf[--len] = 0;
            if (len > 0) {
                SyslogRecord &record =
                    syslog_queue[(syslog_head + syslog_count) % SYSLOG_QUEUE_DEPTH];
                record.cat = cat;
                record.level = lvl;
                memcpy(record.text, buf, len + 1);
                syslog_count++;
            }
        }
        xSemaphoreGive(log_mutex);
    } else {
        Serial.write((const uint8_t*)buf, len);
    }
}

void Log::printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_dispatch(CAT_GENERAL, LOG_INFO, fmt, args);
    va_end(args);
}

void Log::logf(log_cat_t cat, log_level_t lvl, const char *fmt, ...) {
    if (cat < CAT_COUNT && lvl > cat_levels[cat]) return;
    va_list args;
    va_start(args, fmt);
    log_dispatch(cat, lvl, fmt, args);
    va_end(args);
}
