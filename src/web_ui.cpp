#include "web_ui.h"
#include "device_status.h"
#include "web_ui_generated.h"
#include "uart_arbiter.h"
#include "oxi_ble.h"
#include "oxi_arbiter.h"
#include "resmed_ota.h"
#include "debug_log.h"
#include "app_config.h"
#include "build_info.h"
#include "wifi_setup.h"
#include "network_hints.h"
#include "live_web_consumer.h"
#include "crc.h"
#include "sd_storage.h"
#include "edf_recorder.h"
#include "edf_catalog.h"
#include "export_sync.h"
#include "airbridge_ota.h"
#include "custom_settings.h"
#include "clinical_jobs.h"
#include "memory_manager.h"
#include "json_util.h"
#include "air10_clock.h"
#include "board.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <time.h>
#include <new>
#include <errno.h>
#include <utility>

static AsyncWebServer *http = nullptr;
static AsyncEventSource *events = nullptr;
static AsyncEventSource *live_events = nullptr;

static bool checkAuth(AsyncWebServerRequest *request) {
    if (request->getResponse()) return false;
    auto &cfg = Config::get();
    if (cfg.http_user.isEmpty() && cfg.http_pass.isEmpty()) return true;
    if (!request->authenticate(cfg.http_user.c_str(), cfg.http_pass.c_str())) {
        request->requestAuthentication();
        return false;
    }
    return true;
}

// iterate json string key-value pairs
// calls fn(key, val) for each pair. Only handles string values.
typedef void (*json_kv_fn)(const char *key, const char *val, void *ctx);
static bool parseJsonObject(const String &body, JsonDocument &doc) {
    return !deserializeJson(doc, body) && doc.is<JsonObject>();
}

static bool json_foreach_kv(const String &body, json_kv_fn fn, void *ctx) {
    aircannect::JsonAllocator allocator;
    JsonDocument doc(&allocator);
    if (!parseJsonObject(body, doc)) return false;
    // Validate the whole object before a callback can mutate configuration.
    for (JsonPairConst pair : doc.as<JsonObjectConst>())
        if (!pair.value().is<const char *>()) return false;
    for (JsonPairConst pair : doc.as<JsonObjectConst>())
        fn(pair.key().c_str(), pair.value().as<const char *>(), ctx);
    return true;
}


static bool writeSetting(const char *cmd, int value) {
    uint16_t timeout = ClinicalJobs::timeout_ms();
    if (!timeout) return false;
    char req[32];
    snprintf(req, sizeof(req), "P S #%s %04X", cmd, (uint16_t)value);
    char resp[64] = {};
    uint16_t resp_len = sizeof(resp);
    return Arbiter::send_cmd(req, CMD_SRC_TCP, CMD_PRIO_NORMAL,
                              resp, &resp_len, timeout);
}

static void jsonQuote(String &out, const char *val) {
    out += '"';
    aircannect::json_escape_append(out, val, val ? strlen(val) : 0, true);
    out += '"';
}

static void jsonAddString(String &out, const char *key, const char *val, bool comma = true) {
    if (comma) out += ',';
    jsonQuote(out, key);
    out += ':';
    jsonQuote(out, val);
}

static void jsonAddInt(String &out, const char *key, int val, bool comma = true) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", val);
    out += buf;
}

static void jsonAddUInt32(String &out, const char *key, uint32_t val,
                          bool comma = true) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    char buf[12];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)val);
    out += buf;
}

static void jsonAddBool(String &out, const char *key, bool val,
                        bool comma = true) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += val ? "true" : "false";
}


static constexpr size_t JSON_BODY_MAX = 4096;
struct JsonBody {
    size_t total;
    size_t received;
    // Bytes follow this POD header; the request destructor frees _tempObject.
};

static void handleJsonBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (request->getResponse()) return;
    if (!checkAuth(request)) return;
    size_t limit = request->url() == "/api/settings"
        ? ClinicalJobs::MAX_BODY_SIZE : JSON_BODY_MAX;
    if (total > limit) {
        request->send(413, "application/json", "{\"error\":\"body_too_large\"}");
        return;
    }
    if (!total || index > total || len > total - index || !len || !data) {
        request->send(400, "application/json", "{\"error\":\"invalid_body_chunk\"}");
        return;
    }

    auto *body = static_cast<JsonBody *>(request->_tempObject);
    if (!body && index == 0) {
        size_t bytes = sizeof(JsonBody) + total + 1;
        body = static_cast<JsonBody *>(aircannect::Memory::alloc_large(bytes));
        if (!body) {
            request->send(503, "application/json", "{\"error\":\"body_allocation_failed\"}");
            return;
        }
        *body = {total, 0};
        request->_tempObject = body;
    }
    if (!body || body->total != total || body->received != index ||
        memchr(data, '\0', len)) {
        request->send(400, "application/json", "{\"error\":\"invalid_body_chunk\"}");
        return;
    }

    char *bytes = reinterpret_cast<char *>(body + 1);
    memcpy(bytes + index, data, len);
    body->received += len;
    bytes[body->received] = '\0';
}

static bool getBody(AsyncWebServerRequest *request, String &out) {
    if (request->getResponse()) return false;
    auto *body = static_cast<JsonBody *>(request->_tempObject);
    if (!body || body->received != body->total) {
        request->send(400, "application/json", "{\"error\":\"incomplete_body\"}");
        return false;
    }

    bool ok = out.concat(reinterpret_cast<const char *>(body + 1), body->total);
    free(body);
    request->_tempObject = nullptr;
    if (!ok)
        request->send(503, "application/json", "{\"error\":\"body_allocation_failed\"}");
    return ok;
}

static void handleRoot(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", HTML_PAGE_GZ, HTML_PAGE_GZ_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}


struct FixedJson {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
};

static void fixedJsonPut(FixedJson &json, char c) {
    if (json.len + 1 >= json.cap) {
        json.overflow = true;
        return;
    }
    json.buf[json.len++] = c;
    json.buf[json.len] = '\0';
}

static void fixedJsonAppend(FixedJson &json, const char *s) {
    while (*s) fixedJsonPut(json, *s++);
}

static void fixedJsonPrintf(FixedJson &json, const char *fmt, ...) {
    if (json.len >= json.cap) {
        json.overflow = true;
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(json.buf + json.len, json.cap - json.len, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= json.cap - json.len) {
        json.len = json.cap ? json.cap - 1 : 0;
        if (json.cap) json.buf[json.len] = '\0';
        json.overflow = true;
        return;
    }
    json.len += (size_t)n;
}

static void fixedJsonAddString(FixedJson &json, const char *key, const char *val, bool comma = true) {
    if (comma) fixedJsonPut(json, ',');
    fixedJsonPut(json, '"');
    fixedJsonAppend(json, key);
    fixedJsonAppend(json, "\":\"");

    if (!val) val = "";
    while (*val) {
        char encoded[6];
        size_t count = aircannect::json_escape_char(*val++, encoded, true);
        for (size_t i = 0; i < count; i++) fixedJsonPut(json, encoded[i]);
    }

    fixedJsonPut(json, '"');
}

static void fixedJsonAddInt(FixedJson &json, const char *key, long val, bool comma = true) {
    if (comma) fixedJsonPut(json, ',');
    fixedJsonPut(json, '"');
    fixedJsonAppend(json, key);
    fixedJsonAppend(json, "\":");
    fixedJsonPrintf(json, "%ld", val);
}

static size_t buildStatusJson(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    const auto status = DeviceStatus::snapshot();
    const auto &r = status.reading;
#if AB_STORAGE_HAS_SDCARD
    SdStorage::Status sd;
    SdStorage::get_status(sd);
    EdfRecorder::Status edf;
    EdfRecorder::get_status(edf);
    EdfCatalog::Status catalog;
    EdfCatalog::get_status(catalog);
    ExportSync::Status export_status;
    ExportSync::get_status(export_status);
    ExportSync::SleepHqStatus sleephq_status;
    ExportSync::get_sleephq_status(sleephq_status);
#endif

    auto &cfg = Config::get();

    char esp_time[20] = "--";
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm t;
        localtime_r(&now, &t);
        snprintf(esp_time, sizeof(esp_time), "%04d-%02d-%02d %02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min);
    }

    char resmed_time[20];
    Air10Clock::status_time(resmed_time);

    FixedJson json = {out, cap, 0, false};
    fixedJsonPut(json, '{');
    fixedJsonAddString(json, "version", airbridge_version(), false);
    fixedJsonAddString(json, "built", airbridge_build_date());
    fixedJsonAddString(json, "system", system_state_name(status.sys));
    fixedJsonAddInt(json, "rop", status.rop);
    fixedJsonAddString(json, "pna", cfg.device_pna.c_str());
    fixedJsonAddString(json, "srn", cfg.device_srn.c_str());
    fixedJsonAddString(json, "esp_time", esp_time);
    fixedJsonAddString(json, "resmed_time", resmed_time);
    fixedJsonAddString(json, "oxi", oxi_state_name(status.oxi));
    fixedJsonAddString(json, "feeding", status.feeding ? "yes" : "no");
    fixedJsonAddInt(json, "spo2", r.valid ? r.spo2 : -1);
    fixedJsonAddInt(json, "pulse", r.valid ? r.pulse_bpm : -1);
    fixedJsonAddInt(json, "heap", ESP.getFreeHeap());
    fixedJsonAddInt(json, "rssi", WiFi.RSSI());
    fixedJsonAddInt(json, "mhr", status.mhr);
    fixedJsonAddInt(json, "uptime", millis() / 1000);
#if AB_STORAGE_HAS_SDCARD
    fixedJsonAddString(json, "sd", !sd.supported ? "unsupported" :
                       sd.mounted ? "mounted" : "unavailable");
    fixedJsonAddInt(json, "sd_total_mb", sd.card_bytes / (1024 * 1024));
    fixedJsonAddInt(json, "sd_used_mb", sd.used_bytes / (1024 * 1024));
    fixedJsonAddString(json, "edf", !edf.supported ? "unsupported" :
                       edf.active ? "recording" :
                       edf.post_processing ? "post-processing" :
                       edf.ready ? "ready" : "unavailable");
    fixedJsonAddString(json, "edf_prefix", edf.file_prefix);
    fixedJsonAddInt(json, "edf_dropped", edf.raw_dropped);
    fixedJsonAddInt(json, "edf_errors", edf.write_errors);
    fixedJsonAddInt(json, "edf_post_errors", edf.post_errors);
    fixedJsonAddInt(json, "edf_str_records", edf.str_records);
    fixedJsonAddInt(json, "edf_pending_str", edf.pending_str);
    fixedJsonAddString(json, "edf_identification",
                       edf.identification_ready ? "ready" : "missing");
    fixedJsonAddInt(json, "edf_catalog_entries", catalog.entries);
    fixedJsonAddInt(json, "edf_catalog_generation", catalog.generation);
    fixedJsonAddString(json, "smb_sync",
                       ExportSync::state_name(export_status.state));
    fixedJsonAddInt(json, "smb_files_uploaded",
                    export_status.files_uploaded);
    fixedJsonAddInt(json, "smb_files_skipped",
                    export_status.files_skipped);
    fixedJsonAddString(json, "smb_error", export_status.last_error);
    fixedJsonAddString(json, "sleephq_sync",
                       ExportSync::state_name(sleephq_status.state));
    fixedJsonAddInt(json, "sleephq_files_uploaded",
                    sleephq_status.files_uploaded);
    fixedJsonAddInt(json, "sleephq_files_skipped",
                    sleephq_status.files_skipped);
    fixedJsonAddInt(json, "sleephq_import_id",
                    sleephq_status.import_id);
    fixedJsonAddString(json, "sleephq_import_status",
                       sleephq_status.import_status);
    fixedJsonAddString(json, "sleephq_error",
                       sleephq_status.last_error);
#else
    fixedJsonAddString(json, "sd", "unsupported");
    fixedJsonAddString(json, "edf", "unsupported");
    fixedJsonAddString(json, "smb_sync", "unsupported");
    fixedJsonAddString(json, "sleephq_sync", "unsupported");
#endif
    fixedJsonPut(json, '}');

    return json.overflow ? 0 : json.len;
}

static const uint32_t STATUS_CACHE_TTL_MS = 500;
static const size_t STATUS_JSON_MAX = AB_STORAGE_HAS_SDCARD ? 1408 : 896;
static String status_cache;
static uint32_t status_cache_built_at = 0;

static bool refreshStatusCacheIfNeeded() {
    uint32_t now = millis();
    if (status_cache.length() > 0 && now - status_cache_built_at < STATUS_CACHE_TTL_MS)
        return true;

    char body[STATUS_JSON_MAX];
    size_t len = buildStatusJson(body, sizeof(body));
    if (len == 0) return false;

    status_cache = body;
    status_cache_built_at = millis();
    return true;
}

static void handleStatus(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    if (!refreshStatusCacheIfNeeded()) {
        request->send(503, "application/json", "{\"ok\":false,\"error\":\"status_unavailable\"}");
        return;
    }
    request->send(200, "application/json", status_cache);
}




static int saveSettings(const String &body, String &json) {
    if (!CustomSettings::ensure_loaded()) {
        json = "{\"error\":\"settings_metadata_unavailable\"}";
        return 503;
    }

    struct { int count; String errors; bool lan_changed; } ctx = {0, "", false};
    bool parsed = json_foreach_kv(body, [](const char *key, const char *val, void *p) {
        auto *c = (decltype(ctx)*)p;
        char *end = nullptr;
        errno = 0;
        int64_t raw = strtoll(val, &end, 10);
        if (end == val || *end != '\0' || errno == ERANGE ||
            raw < INT32_MIN || raw > UINT32_MAX) {
            c->errors += key;
            c->errors += ":invalid_number,";
            return;
        }
        if (CustomSettings::contains(key)) {
            if (CustomSettings::write_raw(key, (uint32_t)raw)) {
                c->count++;
            } else {
                c->errors += key;
                c->errors += ":fail,";
                Log::logf(CAT_WEB, LOG_DEBUG, "[CLINICAL] Custom write #%.3s failed\n", key);
            }
            return;
        }
        if (!ClinicalSettings::known_stock(key)) {
            c->errors += key;
            c->errors += ":unknown,";
            return;
        }
        if (raw < INT16_MIN || raw > UINT16_MAX) {
            c->errors += key;
            c->errors += ":invalid_number,";
            return;
        }
        if (writeSetting(key, (int)raw)) {
            c->count++;
            if (strcmp(key, "LAN") == 0) c->lan_changed = true;
        } else {
            c->errors += key;
            c->errors += ":fail,";
            Log::logf(CAT_WEB, LOG_DEBUG, "[CLINICAL] Stock write #%.3s failed\n", key);
        }
    }, &ctx);
    if (!parsed) {
        json = "{\"error\":\"bad_json\"}";
        return 400;
    }
    int count = ctx.count;
    String errors = ctx.errors;
    if (ctx.lan_changed) CustomSettings::invalidate("LAN write");
    Log::logf(CAT_WEB, errors.length() ? LOG_WARN : LOG_INFO,
              "[CLINICAL] Save applied=%d%s\n", count,
              errors.length() ? " with rejected or failed fields" : "");

    json = "{";
    jsonAddInt(json, "saved", count, false);
    if (errors.length() > 0) {
        errors.remove(errors.length() - 1);  // trailing comma
        json += ",\"errors\":[";
        jsonQuote(json, errors.c_str());
        json += ']';
    }
    json += '}';
    return 200;
}


class BufferedJsonResponse : public AsyncWebServerResponse {
public:
    void begin(int code, size_t length) {
        _code = code;
        _contentType = "application/json";
        _contentLength = length;
    }

    void _respond(AsyncWebServerRequest *request) override {
        addHeader("Connection", "close", false);
        _assembleHead(headers_, request->version());
        _state = RESPONSE_HEADERS;
        _ack(request, 0, 0);
    }

    size_t _ack(AsyncWebServerRequest *request, size_t len, uint32_t) override {
        _ackedLength += len;
        size_t written = 0;
        if (_state == RESPONSE_HEADERS) {
            size_t count = request->client()->add(headers_.c_str() + header_offset_,
                                                   headers_.length() - header_offset_);
            header_offset_ += count;
            written += count;
            if (header_offset_ == headers_.length()) _state = RESPONSE_CONTENT;
        }
        if (_state == RESPONSE_CONTENT) {
            while (_sentLength < _contentLength && request->client()->space()) {
                if (buffer_offset_ == buffer_length_) {
                    buffer_length_ = readBody(_sentLength, buffer_, sizeof(buffer_));
                    buffer_offset_ = 0;
                    if (!buffer_length_) {
                        _state = RESPONSE_FAILED;
                        request->client()->close();
                        return written;
                    }
                }
                // Keep generated bytes until TCP has accepted them, even after a zero add.
                size_t count = request->client()->add(buffer_ + buffer_offset_,
                                                       buffer_length_ - buffer_offset_);
                buffer_offset_ += count;
                _sentLength += count;
                written += count;
                if (buffer_offset_ != buffer_length_) break;
            }
            if (_sentLength == _contentLength) _state = RESPONSE_WAIT_ACK;
        }
        _writtenLength += written;
        if (_writtenLength > _ackedLength) request->client()->send();
        if (_state == RESPONSE_WAIT_ACK && _ackedLength >= _writtenLength)
            _state = RESPONSE_END;
        return written;
    }

protected:
    virtual size_t readBody(size_t offset, char *out, size_t capacity) = 0;

private:
    String headers_;
    size_t header_offset_ = 0;
    char buffer_[256];
    size_t buffer_offset_ = 0, buffer_length_ = 0;
};

class ClinicalResponse : public BufferedJsonResponse {
public:
    explicit ClinicalResponse(ClinicalJobs::Result &&ready) : result(std::move(ready)) {}
    ClinicalJobs::Result result;

    void begin(int code) { BufferedJsonResponse::begin(code, result.length()); }
    bool _sourceValid() const override { return result.available(); }

protected:
    size_t readBody(size_t offset, char *out, size_t capacity) override {
        return result.read(cursor_, offset, out, capacity);
    }

private:
    ClinicalSettings::Cursor cursor_;
};

static void handleClinicalJob(AsyncWebServerRequest *request, ClinicalJobs::Kind kind) {
    if (!checkAuth(request)) return;
    bool write = kind == ClinicalJobs::Kind::Write;
    if (!write && request->hasArg("job")) {
        ClinicalJobs::Result result;
        int code = ClinicalJobs::poll(strtoul(request->arg("job").c_str(), nullptr, 10), result);
        if (result.available()) {
            auto *response = new (std::nothrow) ClinicalResponse(std::move(result));
            if (!response) {
                request->send(503, "application/json", "{\"error\":\"settings_allocation_failed\"}");
                return;
            }
            response->begin(code);
            request->send(response);
        } else {
            request->send(code, "application/json", code == 202 ? "{\"pending\":true}"
                : "{\"error\":\"settings_job_unavailable\"}");
        }
        return;
    }
    uint32_t id = 0;
    String body;
    if (write && !getBody(request, body)) return;
    if (!ClinicalJobs::submit(kind, std::move(body), id)) {
        request->send(503, "application/json", "{\"error\":\"settings_busy\"}");
        return;
    }
    request->send(202, "application/json", "{\"job\":" + String(id) + "}");
}

static void handleGetSettings(AsyncWebServerRequest *request) { handleClinicalJob(request, ClinicalJobs::Kind::Read); }
static void handlePostSettings(AsyncWebServerRequest *request) { handleClinicalJob(request, ClinicalJobs::Kind::Write); }
static void handleReport(AsyncWebServerRequest *request) { handleClinicalJob(request, ClinicalJobs::Kind::Report); }


static void handleGetConfig(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    Config::Section section = Config::Section::All;
    if (request->hasParam("section") &&
        !Config::parse_section(request->getParam("section")->value().c_str(), section)) {
        request->send(400, "application/json", "{\"error\":\"unknown_section\"}");
        return;
    }

    String json = "{";
    struct { String *json; bool first; } ctx = {&json, true};
    Config::foreach_kv([](const char *key, const String &val, bool sensitive, void *p) {
        auto *c = (decltype(ctx)*)p;
        if (!c->first) *c->json += ',';
        c->first = false;
        jsonAddString(*c->json, key, sensitive ? "" : val.c_str(), false);
    }, &ctx, section);
    json += '}';
    request->send(200, "application/json", json);
}


static void handlePostConfig(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    String body;
    if (!getBody(request, body)) return;
    String previous_update_url = Config::get().update_url;
    int count = 0;
    if (!json_foreach_kv(body, [](const char *key, const char *val, void *p) {
        if (Config::set_value(key, val)) (*(int*)p)++;
    }, &count)) {
        request->send(400, "application/json", "{\"error\":\"bad_json\"}");
        return;
    }

    if (count > 0) Config::save();
    if (Config::get().update_url != previous_update_url)
        OtaManager::config_changed();

    String json = "{\"ok\":true,\"saved\":";
    json += String(count);
    json += '}';
    request->send(200, "application/json", json);
}

static void handleSmbSync(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    if (Arbiter::get_cached_rop() == 1) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"therapy_active\"}");
        return;
    }
    const bool queued = ExportSync::request_manual_smb();
    request->send(queued ? 202 : 409, "application/json",
                  queued ? "{\"ok\":true,\"state\":\"pending\"}" :
                           "{\"ok\":false,\"error\":\"unavailable\"}");
}

static void handleSleepHqSync(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    if (Arbiter::get_cached_rop() == 1) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"therapy_active\"}");
        return;
    }
    const bool queued = ExportSync::request_manual_sleephq();
    request->send(queued ? 202 : 409, "application/json",
                  queued ? "{\"ok\":true,\"state\":\"pending\"}" :
                           "{\"ok\":false,\"error\":\"unavailable\"}");
}


static const esp_partition_t *resmed_part = nullptr;
size_t uploadSize = 0;
static bool uploadOk = false;
static uint16_t uploadCrc = 0xFFFF;

typedef enum {
    UPLOAD_NONE,
    UPLOAD_RESMED,
    UPLOAD_ESP,
} upload_kind_t;

static upload_kind_t uploadKind = UPLOAD_NONE;
static AsyncWebServerRequest *uploadOwner = nullptr;
static size_t uploadNextIndex = 0;
static bool uploadComplete = false;
static bool uploadOwnsUart = false;
static esp_ota_handle_t esp_ota_handle = 0;
static const esp_partition_t *esp_ota_part = nullptr;

static void finishUpload(AsyncWebServerRequest *request, bool success,
                         const char *error) {
    if (uploadOwner != request) return;
    if (esp_ota_handle) {
        esp_ota_abort(esp_ota_handle);
        esp_ota_handle = 0;
    }
    esp_ota_part = nullptr;
    if (!success) {
        uploadOk = false;
        uploadComplete = false;
        uploadKind = UPLOAD_NONE;
        resmed_part = nullptr;
    }
    if (uploadOwnsUart && Arbiter::get_state() == SYS_OTA_ESP)
        Arbiter::set_state(SYS_IDLE);
    uploadOwnsUart = false;
    uploadOwner = nullptr;
    OtaManager::end_manual_upload(success, error);
}

static bool claimUpload(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return false;
    if (uploadOwner) {
        // More than one file in the owning request is not a firmware image.
        if (uploadOwner == request) uploadOk = false;
        return false;
    }
    if (!OtaManager::begin_manual_upload()) return false;
    uploadOwner = request;
    uploadNextIndex = 0;
    uploadComplete = false;
    uploadOwnsUart = false;
    request->onDisconnect([request]() {
        finishUpload(request, false, "upload_disconnected");
    });
    return true;
}

static bool acceptUploadChunk(AsyncWebServerRequest *request, size_t index,
                               size_t len, bool final) {
    if (uploadOwner != request) return false;
    if (uploadComplete || index != uploadNextIndex || len > SIZE_MAX - index) {
        uploadOk = false;
        return false;
    }
    uploadNextIndex += len;
    uploadComplete = final;
    return true;
}

static bool ownsUpload(AsyncWebServerRequest *request) {
    if (uploadOwner == request) return true;
    request->send(409, "application/json",
                  "{\"ok\":false,\"error\":\"upload_not_owned\"}");
    return false;
}

static bool hasValidResmedUpload() {
    return !uploadOwner && uploadComplete && uploadKind == UPLOAD_RESMED &&
           uploadOk && uploadSize > 0 && resmed_part;
}

static void handleUploadChunk(AsyncWebServerRequest *request, const String& filename,
                               size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        if (!claimUpload(request)) return;
        Log::logf(CAT_WEB, LOG_INFO, "[WEB] Upload start: %s\n", filename.c_str());
        uploadKind = UPLOAD_RESMED;
        uploadSize = 0;
        uploadOk = false;
        uploadCrc = 0xFFFF;

        resmed_part = ResmedOta::get_staging_partition();
        if (!resmed_part) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] No staging partition found\n");
            uploadKind = UPLOAD_NONE;
            finishUpload(request, false, "staging_partition_missing");
            return;
        }
        Log::logf(CAT_WEB, LOG_INFO, "[WEB] Staging to '%s' (0x%X, %u bytes)\n",
                     resmed_part->label, resmed_part->address, resmed_part->size);

        esp_err_t err = esp_partition_erase_range(resmed_part, 0, resmed_part->size);
        if (err != ESP_OK) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] Erase failed: %s\n", esp_err_to_name(err));
            resmed_part = nullptr;
            uploadKind = UPLOAD_NONE;
            finishUpload(request, false, "staging_erase_failed");
            return;
        }
        uploadOk = true;
    }

    if (!acceptUploadChunk(request, index, len, final)) return;
    if (uploadKind == UPLOAD_RESMED && resmed_part && uploadOk && len > 0) {
        // reject ESP32 binaries uploaded to resmed slot
        if (uploadSize == 0 && len > 0 && data[0] == 0xE9) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] Rejected: ESP32 binary uploaded to ResMed slot\n");
            uploadOk = false;
            return;
        }
        if (uploadSize + len > resmed_part->size) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] File too large for partition!\n");
            uploadOk = false;
            return;
        }
        esp_err_t err = esp_partition_write(resmed_part, uploadSize, data, len);
        if (err != ESP_OK) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] Write failed at offset %u: %s\n",
                         uploadSize, esp_err_to_name(err));
            uploadOk = false;
            return;
        }
        uploadCrc = crc16_ccitt(data, len, uploadCrc);
        uploadSize += len;
    }

    if (final) {
        if (uploadOk) {
            Log::logf(CAT_WEB, LOG_INFO, "[WEB] Upload complete: %u bytes\n", uploadSize);
        }
    }
}

static void handleUploadDone(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) {
        finishUpload(request, false, "upload_unauthorized");
        return;
    }
    if (!ownsUpload(request)) return;

    String json = "{";
    bool validUpload = uploadComplete && uploadKind == UPLOAD_RESMED &&
                       uploadOk && uploadSize > 0 && resmed_part;
    jsonAddString(json, "ok", validUpload ? "true" : "false", false);
    jsonAddInt(json, "size", uploadSize);

    if (validUpload) {
        char hexcrc[8];
        snprintf(hexcrc, sizeof(hexcrc), "%04X", uploadCrc);
        jsonAddString(json, "crc", hexcrc);

        // Firmware verification
        fw_verify_result_t v = ResmedOta::verify_image(resmed_part, uploadSize);
        if (v.has_blx) {
            jsonAddString(json, "bid", v.bid);
            jsonAddString(json, "bid_ok", v.bid_ok ? "true" : "false");
            jsonAddString(json, "blx_crc", v.blx_crc_ok ? "ok" : "fail");
            const char *patch = "none";
            if (v.blx_patch == BLX_PATCH_A_DANGEROUS) patch = "method_a";
            else if (v.blx_patch == BLX_PATCH_B_SAFE) patch = "method_b";
            jsonAddString(json, "blx_patch", patch);
        }
        if (v.has_ccx) jsonAddString(json, "ccx_crc", v.ccx_crc_ok ? "ok" : "fail");
        if (v.has_cdx) jsonAddString(json, "cdx_crc", v.cdx_crc_ok ? "ok" : "fail");
    }

    finishUpload(request, validUpload,
                  validUpload ? nullptr : "resmed_upload_failed");
    json += '}';
    request->send(200, "application/json", json);
}


class LiveResponse : public BufferedJsonResponse {
public:
    ~LiveResponse() override { aircannect::Memory::free(samples_); }

    bool prepare(uint16_t since) {
        samples_ = static_cast<LiveWebConsumer::Sample *>(aircannect::Memory::alloc_large(
            LiveWebConsumer::HISTORY_CAPACITY * sizeof(*samples_)));
        if (!samples_) return false;

        count_ = LiveWebConsumer::get_samples(samples_, LiveWebConsumer::HISTORY_CAPACITY,
                                               since, &sequence_);
        oxi_reading_t reading;
        OxiArbiter::snapshot(reading);
        spo2_ = reading.valid ? reading.spo2 : -1;
        pulse_ = reading.valid ? reading.pulse_bpm : -1;
        active_ = LiveWebConsumer::is_active();

        char token[128];
        size_t length = 0;
        for (uint16_t part = 0; part < count_ + 2; part++)
            length += formatPart(part, token, sizeof(token));
        BufferedJsonResponse::begin(200, length);
        return true;
    }

    bool _sourceValid() const override { return samples_ != nullptr; }

protected:
    size_t readBody(size_t, char *out, size_t capacity) override {
        size_t written = 0;
        while (written < capacity && part_ < count_ + 2) {
            char token[128];
            size_t length = formatPart(part_, token, sizeof(token));
            size_t count = min(capacity - written, length - part_offset_);
            memcpy(out + written, token + part_offset_, count);
            written += count;
            part_offset_ += count;
            if (part_offset_ == length) { part_++; part_offset_ = 0; }
        }
        return written;
    }

private:
    size_t formatPart(uint16_t part, char *out, size_t capacity) const {
        if (part == 0) {
            return snprintf(out, capacity,
                "{\"seq\":%u,\"rate\":25,\"active\":\"%s\",\"spo2\":%d,\"pulse\":%d,\"samples\":[",
                sequence_, active_ ? "yes" : "no", spo2_, pulse_);
        }
        if (part <= count_) {
            const auto &sample = samples_[part - 1];
            return snprintf(out, capacity, "%s[%d,%d,%d]", part == 1 ? "" : ",",
                            sample.mkp, sample.rfl, sample.lyk);
        }
        memcpy(out, "]}", 2);
        return 2;
    }

    LiveWebConsumer::Sample *samples_ = nullptr;
    uint16_t count_ = 0, sequence_ = 0, part_ = 0;
    size_t part_offset_ = 0;
    int16_t spo2_ = -1, pulse_ = -1;
    bool active_ = false;
};

static void handleLive(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    uint16_t since = 0;
    if (request->hasArg("since"))
        since = (uint16_t)request->arg("since").toInt();

    auto *response = new (std::nothrow) LiveResponse;
    if (!response || !response->prepare(since)) {
        delete response;
        request->send(503, "application/json", "{\"error\":\"live_allocation_failed\"}");
        return;
    }
    request->send(response);
}


static void handleBleStatus(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    oxi_state_t st = OxiBle::get_state();
    oxi_reading_t r;
    OxiArbiter::snapshot(r);
    auto &cfg = Config::get();

    String json = "{";
    jsonAddString(json, "state", oxi_state_name(st), false);
    jsonAddString(json, "feeding", OxiArbiter::is_feeding() ? "yes" : "no");
    jsonAddInt(json, "spo2", r.valid ? r.spo2 : -1);
    jsonAddInt(json, "pulse", r.valid ? r.pulse_bpm : -1);
    jsonAddString(json, "configured_addr", cfg.oxi_device_addr.c_str());
    jsonAddString(json, "auto_start", cfg.oxi_auto_start ? "yes" : "no");


    oxi_scan_result_t devs[MAX_SCAN_RESULTS];
    int scan_count = OxiBle::get_scan_results(devs, MAX_SCAN_RESULTS);
    json += ",\"devices\":[";
    for (int i = 0; i < scan_count; i++) {
        if (i > 0) json += ',';
        json += "{";
        jsonAddString(json, "addr", devs[i].addr, false);
        jsonAddString(json, "name", devs[i].name.c_str());
        jsonAddInt(json, "rssi", devs[i].rssi);
        json += "}";
    }
    json += "],\"bonds\":[";
    char known_addrs[6][18];
    int nk = OxiBle::get_all_known(known_addrs, 6);
    for (int i = 0; i < nk; i++) {
        if (i > 0) json += ',';
        json += '"';
        json += known_addrs[i];
        json += '"';
    }
    json += "]}";
    request->send(200, "application/json", json);
}


static void handleBleAction(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    String body;
    if (!getBody(request, body)) return;
    String action, addr;
    struct { String *action; String *addr; } ctx = {&action, &addr};
    if (!json_foreach_kv(body, [](const char *key, const char *val, void *p) {
        auto *c = (decltype(ctx)*)p;
        if (strcmp(key, "action") == 0) *c->action = val;
        else if (strcmp(key, "addr") == 0) *c->addr = val;
    }, &ctx)) {
        request->send(400, "application/json", "{\"error\":\"bad_json\"}");
        return;
    }

    String result = "unknown action";
    bool ok = false;

    if (action == "scan") {
        OxiBle::start_scan();
        result = "scan started";
        ok = true;
    } else if (action == "stop_scan") {
        OxiBle::stop_scan();
        result = "scan stopped";
        ok = true;
    } else if (action == "connect") {
        if (addr.length() > 0) {
            OxiBle::connect(addr.c_str());
        } else {
            OxiBle::connect(nullptr);
        }
        result = "connecting";
        ok = true;
    } else if (action == "disconnect") {
        OxiBle::disconnect();
        result = "disconnected";
        ok = true;
    } else if (action == "enable") {
        OxiBle::enable();
        result = "oximetry enabled";
        ok = true;
    } else if (action == "disable") {
        OxiBle::disable();
        result = "oximetry disabled";
        ok = true;
    } else if (action == "start_feed") {
        OxiArbiter::start_feed();
        result = "feeding started";
        ok = true;
    } else if (action == "stop_feed") {
        OxiArbiter::stop_feed();
        result = "feeding stopped";
        ok = true;
    } else if (action == "delete_bond") {
        if (addr.length() > 0) {
            // Clear the configured device addr synchronously if it matches.
            if (strcasecmp(addr.c_str(), Config::get().oxi_device_addr.c_str()) == 0) {
                Config::set_value("oxi_device_addr", "");
                Config::save();
            }
            OxiBle::request_remove_known(addr.c_str());
            result = "queued";
            ok = true;
        } else {
            result = "no address specified";
            ok = true;
        }
    } else if (action == "delete_all_bonds") {
        if (Config::get().oxi_device_addr.length() > 0) {
            Config::set_value("oxi_device_addr", "");
            Config::save();
        }
        OxiBle::request_clear_all_known();
        result = "queued";
        ok = true;
    }

    String json = "{";
    jsonAddString(json, "ok", ok ? "true" : "false", false);
    jsonAddString(json, "result", result.c_str());
    json += '}';
    request->send(200, "application/json", json);
}


extern void dispatch_command(const char *line, String &response);

static void handleCmd(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    String body;
    if (!getBody(request, body)) return;
    aircannect::JsonAllocator allocator;
    JsonDocument doc(&allocator);
    if (!parseJsonObject(body, doc) || !doc["cmd"].is<const char *>()) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }
    String cmd = doc["cmd"].as<const char *>();

    String json = "{";

    if (cmd.startsWith("$")) {
        // Internal command
        String response;
        dispatch_command(cmd.c_str() + 1, response);
        response.trim();
        jsonAddString(json, "ok", "true", false);
        jsonAddString(json, "response", response.c_str());
    } else {
        // Q-frame
        char resp[128] = {};
        uint16_t resp_len = sizeof(resp);
        bool ok = Arbiter::send_cmd(cmd.c_str(), CMD_SRC_TCP, CMD_PRIO_NORMAL,
                                     resp, &resp_len);
        jsonAddString(json, "ok", ok ? "true" : "false", false);
        if (ok && resp_len > 0) {
            jsonAddString(json, "response", resp);
        }
        if (ok && cmd.startsWith("P S #ROP ")) {
            int new_rop = (int)strtoul(cmd.c_str() + 9, nullptr, 16);
            Arbiter::set_cached_rop(new_rop);
            WebUI::push_status_event();
        }
    }

    json += '}';
    request->send(200, "application/json", json);
}


static void handleFlashStatus(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;

    String json = "{";
    jsonAddString(json, "active", ResmedOta::is_active() ? "true" : "false", false);
    jsonAddString(json, "phase", ResmedOta::get_phase());
    jsonAddInt(json, "sent", ResmedOta::get_sent());
    jsonAddInt(json, "total", ResmedOta::get_total());
    const char *err = ResmedOta::last_error();
    if (err && err[0]) {
        jsonAddString(json, "error", err);
    }

    if (hasValidResmedUpload()) {
        const char *detected = ResmedOta::detect_block(uploadSize);
        jsonAddString(json, "detected_block", detected ? detected : "unknown");
        jsonAddInt(json, "fw_size", uploadSize);
    }
    json += '}';
    request->send(200, "application/json", json);
}


static void handleFlashStart(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    String body;
    if (!getBody(request, body)) return;

    aircannect::JsonAllocator allocator;
    JsonDocument doc(&allocator);
    if (!parseJsonObject(body, doc)) {
        request->send(400, "application/json", "{\"error\":\"bad_json\"}");
        return;
    }

    if (ResmedOta::is_active()) {
        request->send(409, "application/json", "{\"ok\":false,\"error\":\"flash already active\"}");
        return;
    }

    if (!OtaManager::begin_resmed_flash()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"another OTA is active\"}");
        return;
    }

    if (!hasValidResmedUpload()) {
        OtaManager::cancel_resmed_flash_claim();
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"no valid ResMed firmware uploaded\"}");
        return;
    }

    String block = doc["block"] | "";
    bool flash_blx = doc["flash_blx"] | false;
    bool force_blx = doc["force_blx"] | false;

    ResmedOta::start_flash(
        block.length() > 0 ? block.c_str() : nullptr,
        uploadSize,
        flash_blx,
        force_blx
    );

    String json = "{";
    jsonAddString(json, "ok", "true", false);
    jsonAddString(json, "block", block.length() > 0 ? block.c_str() :
                  (ResmedOta::detect_block(uploadSize) ? ResmedOta::detect_block(uploadSize) : "auto"));
    json += '}';
    request->send(200, "application/json", json);
}


static void handleFlashCancel(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    ResmedOta::cancel();
    request->send(200, "application/json", "{\"ok\":true}");
}



// ESP32 OTA
static void abortEspOtaUpload(AsyncWebServerRequest *request) {
    if (uploadOwner != request) return;
    if (esp_ota_handle) {
        esp_ota_abort(esp_ota_handle);
        esp_ota_handle = 0;
    }
    esp_ota_part = nullptr;
    uploadKind = UPLOAD_NONE;
    // Keep the request reservation until completion or disconnect.
}

static void handleEspOtaChunk(AsyncWebServerRequest *request, const String& filename,
                               size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        if (!claimUpload(request)) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] ESP OTA rejected: OTA busy\n");
            return;
        }
        Log::logf(CAT_WEB, LOG_INFO, "[WEB] ESP OTA start: %s\n", filename.c_str());
        uploadKind = UPLOAD_ESP;
        resmed_part = nullptr;
        uploadSize = 0;
        uploadOk = false;

        esp_ota_part = esp_ota_get_next_update_partition(NULL);
        if (!esp_ota_part) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] No OTA partition found\n");
            uploadKind = UPLOAD_NONE;
            finishUpload(request, false, "ota_partition_missing");
            return;
        }
        Log::logf(CAT_WEB, LOG_INFO, "[WEB] OTA target: '%s' (0x%X, %u bytes)\n",
                  esp_ota_part->label, esp_ota_part->address, esp_ota_part->size);

        esp_err_t err = esp_ota_begin(esp_ota_part, OTA_SIZE_UNKNOWN, &esp_ota_handle);
        if (err != ESP_OK) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] esp_ota_begin failed: %s\n", esp_err_to_name(err));
            esp_ota_part = nullptr;
            uploadKind = UPLOAD_NONE;
            esp_ota_handle = 0;
            finishUpload(request, false, "esp_ota_begin_failed");
            return;
        }
        uploadOk = true;
    }

    if (!acceptUploadChunk(request, index, len, final)) return;
    if (uploadKind == UPLOAD_ESP && esp_ota_part && uploadOk && len > 0) {
        // validate esp binary magic on first data
        if (uploadSize == 0 && len > 0 && data[0] != 0xE9) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] Not an ESP32 binary (magic=0x%02X)\n", data[0]);
            uploadOk = false;
            abortEspOtaUpload(request);
            return;
        }
        if (uploadSize == 0) {
            uploadOwnsUart = true;
            Arbiter::set_state(SYS_OTA_ESP);
        }
        esp_err_t err = esp_ota_write(esp_ota_handle, data, len);
        if (err != ESP_OK) {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] esp_ota_write failed at %u: %s\n",
                      uploadSize, esp_err_to_name(err));
            uploadOk = false;
            abortEspOtaUpload(request);
            return;
        }
        uploadSize += len;
    }

    if (final && uploadOk) {
        Log::logf(CAT_WEB, LOG_INFO, "[WEB] ESP OTA upload complete: %u bytes\n", uploadSize);
    }
}

static void handleEspOtaDone(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) {
        finishUpload(request, false, "upload_unauthorized");
        return;
    }
    if (!ownsUpload(request)) return;

    bool ok = false;
    const char *error = "invalid firmware image";

    if (uploadComplete && uploadKind == UPLOAD_ESP && uploadOk &&
        uploadSize > 0 && esp_ota_part) {
        esp_err_t err = esp_ota_end(esp_ota_handle);
        esp_ota_handle = 0;
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(esp_ota_part);
            if (err == ESP_OK) {
                ok = true;
                Log::logf(CAT_WEB, LOG_INFO, "[WEB] ESP OTA OK, boot set to '%s'\n",
                          esp_ota_part->label);
            } else {
                error = esp_err_to_name(err);
                Log::logf(CAT_WEB, LOG_ERROR, "[WEB] set_boot_partition failed: %s\n",
                          esp_err_to_name(err));
            }
        } else {
            Log::logf(CAT_WEB, LOG_ERROR, "[WEB] esp_ota_end failed: %s\n", esp_err_to_name(err));
            error = esp_err_to_name(err);
        }
    }

    String json = "{";
    jsonAddString(json, "ok", ok ? "true" : "false", false);
    if (!ok) jsonAddString(json, "error", error);
    jsonAddInt(json, "size", uploadSize);
    if (esp_ota_part)
        jsonAddString(json, "partition", esp_ota_part->label);

    finishUpload(request, ok, ok ? nullptr : "esp_upload_failed");
    uploadKind = UPLOAD_NONE;

    json += '}';
    request->send(200, "application/json", json);
}

static void sendOtaStatus(AsyncWebServerRequest *request, int status_code) {
    OtaManager::Status status;
    OtaManager::get_status(status);
    String json = "{";
    json.reserve(512);
    jsonAddString(json, "version", airbridge_version(), false);
    jsonAddString(json, "release_target", status.release_target);
    jsonAddBool(json, "enabled", status.enabled);
    jsonAddBool(json, "checking", status.checking);
    jsonAddBool(json, "checked", status.checked);
    jsonAddBool(json, "update_available", status.update_available);
    jsonAddBool(json, "installable", status.installable);
    jsonAddBool(json, "installing", status.installing);
    jsonAddBool(json, "reboot_pending", status.reboot_pending);
    jsonAddInt(json, "progress", status.progress);
    jsonAddInt(json, "bytes", status.bytes);
    jsonAddInt(json, "total_size", status.total_size);
    jsonAddInt(json, "last_check_age_ms", status.last_check_age_ms);
    jsonAddString(json, "update_version", status.update_version);
    jsonAddString(json, "error", status.error);
    json += '}';
    request->send(status_code, "application/json", json);
}

static void handleOtaStatus(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    sendOtaStatus(request, 200);
}

static void handleOtaCheck(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    sendOtaStatus(request, OtaManager::request_check() ? 202 : 409);
}

static void handleOtaInstall(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    sendOtaStatus(request, OtaManager::request_install() ? 202 : 409);
}

// WiFi management

static void handleWifiGet(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    auto &cfg = Config::get();

    String json = "{";
    jsonAddString(json, "state", WiFiSetup::state_name(), false);
    jsonAddString(json, "ssid", WiFiSetup::connected_ssid());
    jsonAddInt(json, "rssi", WiFiSetup::current_rssi());
    jsonAddInt(json, "net_idx", WiFiSetup::connected_net_idx());
    jsonAddString(json, "roam", cfg.wifi_roam ? "1" : "0");

    json += ",\"networks\":[";
    for (int i = 0; i < cfg.wifi_net_count; i++) {
        if (i > 0) json += ',';
        json += "{";
        jsonAddString(json, "ssid", cfg.wifi_nets[i].ssid.c_str(), false);
        jsonAddString(json, "enabled", cfg.wifi_nets[i].enabled ? "1" : "0");
        // Hint info comes from NetworkHints now (one slot may have multiple
        // BSSID hints with multi-AP roaming; pick the most recent here).
        const NetworkHint *h = NetworkHints::find_best(cfg.wifi_nets[i].ssid.c_str());
        jsonAddString(json, "hint", h ? "1" : "0");
        jsonAddInt(json, "channel", h ? h->channel : 0);
        jsonAddInt(json, "rssi", WiFiSetup::net_rssi(i));
        json += "}";
    }
    json += "]}";
    request->send(200, "application/json", json);
}

static void handleWifiPost(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    String body;
    if (!getBody(request, body)) return;
    String action, ssid, pass;
    int idx = -1;

    struct wifi_kv_ctx { String *action; String *ssid; String *pass; int *idx; };
    wifi_kv_ctx wctx = {&action, &ssid, &pass, &idx};
    if (!json_foreach_kv(body, [](const char *key, const char *val, void *p) {
        wifi_kv_ctx *c = (wifi_kv_ctx *)p;
        if (strcmp(key, "action") == 0) *c->action = val;
        else if (strcmp(key, "ssid") == 0) *c->ssid = val;
        else if (strcmp(key, "pass") == 0) *c->pass = val;
        else if (strcmp(key, "idx") == 0) *c->idx = atol(val);
    }, &wctx)) {
        request->send(400, "application/json", "{\"error\":\"bad_json\"}");
        return;
    }

    String result = "unknown action";
    bool ok = false;

    if (action == "add") {
        if (ssid.length() > 0) {
            ok = Config::add_network(ssid.c_str(), pass.c_str());
            result = ok ? "network added" : "list full (max 4)";
        } else {
            result = "ssid required";
        }
    } else if (action == "remove") {
        if (idx >= 0) {
            ok = Config::remove_network((uint8_t)idx);
            result = ok ? "network removed" : "invalid index";
        } else {
            result = "idx required";
        }
    } else if (action == "update") {
        auto &cfg = Config::get();
        if (idx >= 0 && idx < cfg.wifi_net_count) {
            if (ssid.length() > 0) cfg.wifi_nets[idx].ssid = ssid;
            if (pass.length() > 0) cfg.wifi_nets[idx].pass = pass;
            Config::save_wifi_nets();
            ok = true;
            result = "network updated";
        } else {
            result = "invalid index";
        }
    }

    String json = "{";
    jsonAddString(json, "ok", ok ? "true" : "false", false);
    jsonAddString(json, "result", result.c_str());
    json += '}';
    request->send(200, "application/json", json);
}

static void handleReboot(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    request->send(200, "application/json", "{\"ok\":true}");
    delay(100);
    ESP.restart();
}


void WebUI::push_event(const char *event, const char *json) {
    AsyncEventSource *target = strcmp(event, "live") == 0 ? live_events : events;
    if (target && target->count()) target->send(json, event, millis());
}

void WebUI::push_event(const char *event, const String &json) {
    push_event(event, json.c_str());
}

extern bool push_time_to_resmed();
extern bool pull_time_from_resmed(bool force);

static void handleTimeAction(AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    String body;
    if (!getBody(request, body)) return;
    String action;
    if (!json_foreach_kv(body, [](const char *key, const char *val, void *p) {
        if (strcmp(key, "action") == 0) *(String*)p = val;
    }, &action)) {
        request->send(400, "application/json", "{\"error\":\"bad_json\"}");
        return;
    }

    String result = "unknown action";
    bool ok = false;

    if (action == "ntp_sync") {
        WiFiSetup::force_ntp_sync();
        result = "NTP resync triggered";
        ok = true;
    } else if (action == "sync_to_resmed") {
        ok = push_time_to_resmed();
        result = ok ? "ResMed clock updated" : "Failed (NTP not synced or device error)";
    } else if (action == "sync_from_resmed") {
        ok = pull_time_from_resmed(true);
        result = ok ? "ESP32 clock set from ResMed" : "Failed to read ResMed clock";
    }

    String json = "{";
    jsonAddString(json, "ok", ok ? "true" : "false", false);
    jsonAddString(json, "result", result.c_str());
    json += '}';
    request->send(200, "application/json", json);
}

void WebUI::init(uint16_t port) {
    if (port == 0) return;
    ClinicalJobs::init(saveSettings);

    http = new AsyncWebServer(port);
    events = new AsyncEventSource("/events");
    live_events = new AsyncEventSource("/events/live");
    auto authenticate_events = [](AsyncWebServerRequest *request, ArMiddlewareNext next) {
        if (checkAuth(request)) next();
    };
    events->addMiddleware(authenticate_events);
    live_events->addMiddleware(authenticate_events);
    live_events->onConnect([](AsyncEventSourceClient *) {
        LiveWebConsumer::acquire();
    });
    live_events->onDisconnect([](AsyncEventSourceClient *) {
        LiveWebConsumer::release();
    });
    http->addHandler(events);
    http->addHandler(live_events);

    http->on("/", HTTP_GET, handleRoot);
    http->on("/api/status", HTTP_GET, handleStatus);
    http->on("/api/settings", HTTP_GET, handleGetSettings);
    http->on("/api/settings", HTTP_POST, handlePostSettings, NULL, handleJsonBody);
    http->on("/api/config", HTTP_GET, handleGetConfig);
    http->on("/api/config", HTTP_POST, handlePostConfig, NULL, handleJsonBody);
    http->on("/api/export/smb", HTTP_POST, handleSmbSync);
    http->on("/api/export/sleephq", HTTP_POST, handleSleepHqSync);
    http->on("/api/live", HTTP_GET, handleLive);
    http->on("/api/upload", HTTP_POST, handleUploadDone, handleUploadChunk);
    http->on("/api/ble", HTTP_GET, handleBleStatus);
    http->on("/api/ble", HTTP_POST, handleBleAction, NULL, handleJsonBody);
    http->on("/api/cmd", HTTP_POST, handleCmd, NULL, handleJsonBody);
    http->on("/api/flash", HTTP_GET, handleFlashStatus);
    http->on("/api/flash", HTTP_POST, handleFlashStart, NULL, handleJsonBody);
    http->on("/api/flash/cancel", HTTP_POST, handleFlashCancel);
    http->on("/api/time", HTTP_POST, handleTimeAction, NULL, handleJsonBody);
    http->on("/api/report", HTTP_GET, handleReport);
    http->on("/api/wifi", HTTP_GET, handleWifiGet);
    http->on("/api/wifi", HTTP_POST, handleWifiPost, NULL, handleJsonBody);
    http->on("/api/esp32/upload", HTTP_POST, handleEspOtaDone, handleEspOtaChunk);
    http->on("/api/ota", HTTP_GET, handleOtaStatus);
    http->on("/api/ota/check", HTTP_POST, handleOtaCheck);
    http->on("/api/ota/install", HTTP_POST, handleOtaInstall);
    http->on("/api/reboot", HTTP_POST, handleReboot);

    DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");

    http->begin();
    Log::logf(CAT_WEB, LOG_INFO, "[WEB] HTTP server on port %d\n", port);
}

static String build_status_payload(const DeviceStatus::Snapshot &status) {
    const auto &r = status.reading;

    String sj;
    sj.reserve(256);
    sj = "{";
    jsonAddString(sj, "system", system_state_name(status.sys), false);
    jsonAddInt(sj, "rop", status.rop);
    jsonAddInt(sj, "mhr", status.mhr);
    jsonAddString(sj, "oxi", oxi_state_name(status.oxi));
    char oxi_addr[32];
    OxiArbiter::get_source_id(oxi_addr, sizeof(oxi_addr));
    jsonAddString(sj, "oxi_addr", oxi_addr);
    jsonAddString(sj, "feeding", status.feeding ? "yes" : "no");
    jsonAddInt(sj, "spo2", r.valid ? r.spo2 : -1);
    jsonAddInt(sj, "pulse", r.valid ? r.pulse_bpm : -1);
    jsonAddInt(sj, "heap", ESP.getFreeHeap());
    jsonAddInt(sj, "rssi", WiFi.RSSI());
    jsonAddInt(sj, "uptime", millis() / 1000);
    sj += '}';
    return sj;
}

static DeviceStatus::Snapshot last_published = {
    SYS_IDLE, OXI_DISABLED, {}, INT_MIN, INT_MIN, false
};

static bool snapshot_differs(const DeviceStatus::Snapshot &a,
                             const DeviceStatus::Snapshot &b) {
    return a.rop     != b.rop     ||
           a.mhr     != b.mhr     ||
           a.sys     != b.sys     ||
           a.oxi     != b.oxi     ||
           a.feeding != b.feeding ||
           (a.reading.valid ? a.reading.spo2 : -1) !=
               (b.reading.valid ? b.reading.spo2 : -1) ||
           (a.reading.valid ? a.reading.pulse_bpm : -1) !=
               (b.reading.valid ? b.reading.pulse_bpm : -1);
}

static uint32_t last_status_push = 0;

void WebUI::push_status_event() {
    if (!events || events->count() == 0) return;
    const auto status = DeviceStatus::snapshot();
    last_published = status;
    last_status_push = millis();
    String sj = build_status_payload(status);
    events->send(sj.c_str(), "status", millis());
}

void WebUI::handle() {
    if (!events || events->count() == 0) return;
    static uint32_t last_check = 0;
    uint32_t now = millis();
    if (uint32_t(now - last_check) < 100) return;
    last_check = now;

    if (snapshot_differs(DeviceStatus::snapshot(), last_published) ||
        millis() - last_status_push >= 10000) {
        WebUI::push_status_event();
    }
}
