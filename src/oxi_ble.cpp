#include "oxi_ble.h"
#include "oxi_ble_policy.h"
#include "oxi_arbiter.h"
#include "wifi_setup.h"
#include "uart_arbiter.h"
#include "debug_log.h"
#include "app_config.h"
#include "crc.h"
#include "hex_util.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <new>
#include <strings.h>
#include "nvs_optional.h"

#define OXI_TASK_STACK      4096
#define OXI_TASK_PRIO       4
#define SCAN_DURATION_MS    10000

// The 20 ms / 1 s observer missed an advertising O2Ring for tens of seconds.
static constexpr uint16_t OBSERVE_INTERVAL_MS = 100;
static constexpr uint16_t OBSERVE_WINDOW_MS = 50;

static const NimBLEUUID PLX_SERVICE_UUID((uint16_t)0x1822);
static const NimBLEUUID PLX_CONTINUOUS_UUID((uint16_t)0x2A5F);
static const NimBLEUUID PLX_SPOT_UUID((uint16_t)0x2A5E);
static const NimBLEUUID HR_SERVICE_UUID((uint16_t)0x180D);
static const NimBLEUUID HR_MEASUREMENT_UUID((uint16_t)0x2A37);

// Nonin proprietary
static const NimBLEUUID NONIN_OXI_SERVICE_UUID("46A970E0-0D5F-11E2-8B5E-0002A5D5C51B");
static const NimBLEUUID NONIN_CONTINUOUS_UUID("0AAD7EA0-0D60-11E2-8E3C-0002A5D5C51B");
static const NimBLEUUID NONIN_CONTROL_POINT_UUID("1447AF80-0D60-11E2-88B6-0002A5D5C51B");

// Wellue/Viatom proprietary (O2Ring, CheckmeO2, SleepU, O2M)
static const NimBLEUUID VIATOM_SERVICE_UUID("14839AC4-7D7E-415C-9A42-167340CF2339");
static const NimBLEUUID VIATOM_READ_UUID("0734594A-A8E7-4B1A-A6B1-CD5243059A57");
static const NimBLEUUID VIATOM_WRITE_UUID("8B00ACE7-EB0B-49B0-BBE9-9AEE0A26E1A3");

// OxyII proprietary (O2Ring-S and related devices)
static const NimBLEUUID OXYII_SERVICE_UUID("E8FB0001-A14B-98F9-831B-4E2941D01248");
static const NimBLEUUID OXYII_WRITE_UUID("E8FB0002-A14B-98F9-831B-4E2941D01248");
static const NimBLEUUID OXYII_NOTIFY_UUID("E8FB0003-A14B-98F9-831B-4E2941D01248");

// ACCARE WS20A proprietary
static const NimBLEUUID WS20A_NOTIFY_SERVICE_UUID((uint16_t)0xFFE0);
static const NimBLEUUID WS20A_WRITE_SERVICE_UUID((uint16_t)0xFFE5);
static const NimBLEUUID WS20A_NOTIFY_UUID((uint16_t)0xFFE4);
static const NimBLEUUID WS20A_WRITE_UUID((uint16_t)0xFFE9);

static TaskHandle_t oxi_task_handle = nullptr;
static volatile oxi_state_t state = OXI_DISABLED;
static std::atomic<uint32_t> status_revision{0};
static inline void set_state(oxi_state_t s) {
    if (state == s) return;
    state = s;
}
static volatile bool scan_requested = false;
static volatile bool active_scan_requested = false;
typedef enum { CONN_NONE, CONN_AUTO, CONN_USER } connect_mode_t;
struct ConnectRequest {
    connect_mode_t mode = CONN_NONE;
    char addr[18] = {};
};
static ConnectRequest pending_connect;
static portMUX_TYPE connect_mux = portMUX_INITIALIZER_UNLOCKED;

static void request_connect(connect_mode_t mode, const char *addr) {
    ConnectRequest request;
    request.mode = mode;
    snprintf(request.addr, sizeof(request.addr), "%s", addr ? addr : "");
    portENTER_CRITICAL(&connect_mux);
    if (mode != CONN_AUTO || pending_connect.mode != CONN_USER)
        pending_connect = request;
    portEXIT_CRITICAL(&connect_mux);
}

static bool take_connect_request(ConnectRequest &request) {
    portENTER_CRITICAL(&connect_mux);
    request = pending_connect;
    pending_connect = {};
    portEXIT_CRITICAL(&connect_mux);
    return request.mode != CONN_NONE;
}

static volatile bool disconnect_requested = false;  // drop connection, stay enabled
static std::atomic<bool> charging_requested{false};
static volatile bool disable_requested = false;     // drop connection, disable scanning
static std::atomic<bool> enable_requested{false};
static volatile bool del_one_requested = false;
static volatile bool del_all_requested = false;
static char del_one_addr[18] = "";
static volatile bool ble_suspended = false;
static volatile bool suspend_enter_requested = false;  // handle entry side-effects in task
static std::atomic<bool> stop_scan_requested{false};

enum class MemoryPause { Ready, Requested, Releasing, Released, Rejected, Resume, Failed };
static std::atomic<MemoryPause> memory_pause{MemoryPause::Ready};
static SemaphoreHandle_t lifecycle_mutex = nullptr;

#define USER_CONNECT_RETRIES  3
#define USER_RETRY_DELAY_MS   2000
static volatile bool scan_complete = false;
static int scan_end_reason = 0;

static NimBLEClient *pClient = nullptr;

static SemaphoreHandle_t scan_mutex = nullptr;
static oxi_scan_result_t scan_results[MAX_SCAN_RESULTS];
static int scan_result_count = 0;
static bool device_needs_encryption = false;  // Nonin needs it, Viatom/O2Ring don't

// Known devices list
// For devices that don't use BLE bonding (O2Ring, CheckMe, etc.)
struct KnownDevice : oxi_known_device_t {
    uint8_t addr_type = 1;
    OxiBlePolicy::Holdoff holdoff;
    bool charging = false;
    uint32_t advertisements = 0;
    uint32_t first_advertisement_ms = 0;
};
static KnownDevice known_devices[MAX_KNOWN_DEVICES];
static int known_count = 0;
static portMUX_TYPE known_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t known_store_mutex = nullptr;
static std::atomic<bool> known_ready{false};
static OxiBlePolicy::Retry auto_retry;
static bool observing = false;
static uint32_t observing_since = 0;
static bool observed_pending = false;
struct ObservedDevice {
    char addr[18] = {};
    char name[32] = {};
    uint8_t addr_type = 0;
    int8_t rssi = 0;
    uint32_t received_ms = 0;
    uint32_t observing_ms = 0;
    uint32_t first_advertisement_ms = 0;
    uint32_t advertisements = 0;
};
static ObservedDevice observed_device;

static portMUX_TYPE sample_mux = portMUX_INITIALIZER_UNLOCKED;
static OxiBlePolicy::Samples ble_samples;

static void feed_ble_sample(int8_t spo2, int16_t pulse, bool valid) {
    portENTER_CRITICAL(&sample_mux);
    ble_samples.received(valid, millis());
    portEXIT_CRITICAL(&sample_mux);
    OxiArbiter::feed(OXI_SRC_BLE, spo2, pulse, valid);
}

static int known_index(const char *addr) {
    for (int i = 0; i < known_count; i++)
        if (strcasecmp(known_devices[i].addr, addr) == 0) return i;
    return -1;
}

static bool autoconnect_key(const char *addr, char key[15]) {
    if (!addr || strlen(addr) != 17) return false;
    key[0] = 'a';
    key[1] = 'c';
    for (int i = 0; i < 6; i++) {
        int high = aircannect::hex_nibble(addr[i * 3]);
        int low = aircannect::hex_nibble(addr[i * 3 + 1]);
        if (high < 0 || low < 0 || (i < 5 && addr[i * 3 + 2] != ':')) return false;
        key[2 + i * 2] = "0123456789abcdef"[high];
        key[3 + i * 2] = "0123456789abcdef"[low];
    }
    key[14] = 0;
    return true;
}

static void known_load() {
    Preferences p;
    if (!open_optional_preferences(p, "oxi_known")) return;
    int count = p.getUChar("count", 0);
    for (int i = 0; i < count && i < MAX_KNOWN_DEVICES; i++) {
        char key[4];
        snprintf(key, sizeof(key), "a%d", i);
        String a = p.getString(key, "");
        char auto_key[15];
        if (!autoconnect_key(a.c_str(), auto_key) || known_index(a.c_str()) >= 0) continue;
        KnownDevice device;
        strlcpy(device.addr, a.c_str(), sizeof(device.addr));
        device.autoconnect = p.getBool(auto_key, true);
        auto_key[0] = 't';
        device.addr_type = p.getUChar(auto_key, 1);
        auto_key[0] = 'n';
        if (p.isKey(auto_key))
            strlcpy(device.name, p.getString(auto_key, "").c_str(), sizeof(device.name));
        portENTER_CRITICAL(&known_mux);
        known_devices[known_count++] = device;
        portEXIT_CRITICAL(&known_mux);
    }
    p.end();
    Log::logf(CAT_OXI, LOG_DEBUG, "Loaded %d known devices\n", known_count);
}

// Caller serializes NVS operations; callbacks only use the short snapshot lock.
static bool known_save() {
    oxi_known_device_t devices[MAX_KNOWN_DEVICES];
    int count = OxiBle::get_known_devices(devices, MAX_KNOWN_DEVICES);
    Preferences p;
    if (!p.begin("oxi_known", false)) return false;
    bool ok = true;
    for (int i = 0; i < count; i++) {
        char key[4];
        snprintf(key, sizeof(key), "a%d", i);
        ok = p.putString(key, devices[i].addr) > 0 && ok;
    }
    if (ok) ok = p.putUChar("count", count) > 0;
    p.end();
    status_revision.fetch_add(1);
    if (!ok) Log::logf(CAT_OXI, LOG_ERROR, "Cannot save known sensors\n");
    return ok;
}

static bool known_contains(const char *addr) {
    portENTER_CRITICAL(&known_mux);
    bool found = known_index(addr) >= 0;
    portEXIT_CRITICAL(&known_mux);
    return found;
}

static bool known_set_charging(const char *addr, bool charging) {
    // Unsaved manual connections have no previous charging state.
    bool changed = charging;
    portENTER_CRITICAL(&known_mux);
    int index = known_index(addr);
    if (index >= 0) {
        changed = known_devices[index].charging != charging;
        known_devices[index].charging = charging;
    }
    portEXIT_CRITICAL(&known_mux);
    return changed;
}

static bool known_add(const char *addr, uint8_t addr_type, bool legacy = false,
                      const char *name = nullptr) {
    if (!known_store_mutex) return false;
    xSemaphoreTake(known_store_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&known_mux);
    bool exists = known_index(addr) >= 0;
    bool room = known_count < (legacy ? MAX_KNOWN_DEVICES : KNOWN_DEVICE_LIMIT);
    portEXIT_CRITICAL(&known_mux);
    char learned_name[sizeof(known_devices[0].name)] = {};
    if (name) strlcpy(learned_name, name, sizeof(learned_name));
    bool metadata_saved = true;
    bool name_saved = false;
    char type_key[15];
    Preferences type_prefs;
    if ((exists || room) && autoconnect_key(addr, type_key) && type_prefs.begin("oxi_known", false)) {
        type_key[0] = 't';
        if (type_prefs.getUChar(type_key, 0xFF) != addr_type)
            metadata_saved = type_prefs.putUChar(type_key, addr_type) > 0;
        // Empty passive advertisements must not erase a previously learned name.
        if (learned_name[0]) {
            type_key[0] = 'n';
            name_saved = type_prefs.isKey(type_key) &&
                         type_prefs.getString(type_key, "") == learned_name;
            if (!name_saved) name_saved = type_prefs.putString(type_key, learned_name) > 0;
            metadata_saved = metadata_saved && name_saved;
        }
        type_prefs.end();
    } else if (exists || room) {
        metadata_saved = false;
    }
    if (exists) {
        portENTER_CRITICAL(&known_mux);
        auto &device = known_devices[known_index(addr)];
        device.addr_type = addr_type;
        bool renamed = name_saved && strcmp(device.name, learned_name) != 0;
        if (renamed) strlcpy(device.name, learned_name, sizeof(device.name));
        portEXIT_CRITICAL(&known_mux);
        if (renamed) status_revision.fetch_add(1);
    }
    if (exists || !room) {
        xSemaphoreGive(known_store_mutex);
        return exists && metadata_saved;
    }

    bool autoconnect = true;
    char key[15];
    Preferences p;
    if (autoconnect_key(addr, key) && open_optional_preferences(p, "oxi_known")) {
        autoconnect = p.getBool(key, true);
        p.end();
    }
    portENTER_CRITICAL(&known_mux);
    auto &device = known_devices[known_count++];
    strlcpy(device.addr, addr, sizeof(device.addr));
    if (name_saved) strlcpy(device.name, learned_name, sizeof(device.name));
    device.autoconnect = autoconnect;
    device.addr_type = addr_type;
    portEXIT_CRITICAL(&known_mux);
    bool saved = known_save();
    xSemaphoreGive(known_store_mutex);
    Log::logf(CAT_OXI, LOG_INFO, "Added known device: %s\n", addr);
    return saved && metadata_saved;
}

static bool known_remove(const char *addr) {
    xSemaphoreTake(known_store_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&known_mux);
    int index = known_index(addr);
    if (index >= 0) {
        for (int i = index; i + 1 < known_count; i++) known_devices[i] = known_devices[i + 1];
        known_devices[--known_count] = {};
    }
    portEXIT_CRITICAL(&known_mux);
    if (index >= 0) {
        known_save();
        Preferences p;
        char key[15];
        if (autoconnect_key(addr, key) && p.begin("oxi_known", false)) {
            p.remove(key);
            key[0] = 't';
            p.remove(key);
            key[0] = 'n';
            if (p.isKey(key)) p.remove(key);
            p.end();
        }
        Log::logf(CAT_OXI, LOG_INFO, "Removed known device: %s\n", addr);
    }
    xSemaphoreGive(known_store_mutex);
    return index >= 0;
}

static void known_clear() {
    xSemaphoreTake(known_store_mutex, portMAX_DELAY);
    Preferences p;
    if (p.begin("oxi_known", false)) {
        p.clear();
        p.end();
    }
    portENTER_CRITICAL(&known_mux);
    known_count = 0;
    for (auto &device : known_devices) device = {};
    portEXIT_CRITICAL(&known_mux);
    xSemaphoreGive(known_store_mutex);
    status_revision.fetch_add(1);
    Log::logf(CAT_OXI, LOG_INFO, "Cleared all known devices\n");
}

static void update_observer(bool active) {
    portENTER_CRITICAL(&known_mux);
    uint32_t now = millis();
    if (active && !observing) {
        observing_since = now;
        for (int i = 0; i < known_count; i++) known_devices[i].advertisements = 0;
    }
    observing = active;
    if (!active) observed_pending = false;
    for (int i = 0; i < known_count; i++)
        known_devices[i].holdoff.update(now, observing, observing_since);
    portEXIT_CRITICAL(&known_mux);
}

static void hold_autoconnect(const char *addr, bool charging, bool until_absent) {
    uint32_t now = millis();
    portENTER_CRITICAL(&known_mux);
    int index = known_index(addr);
    if (index >= 0) known_devices[index].holdoff.start(now, charging, until_absent);
    portEXIT_CRITICAL(&known_mux);
    Log::logf(CAT_OXI, LOG_DEBUG, "Auto-connect holdoff addr=%s at=%lu duration=%lus clear=%s\n",
              addr, (unsigned long)now,
              (unsigned long)((charging ? OxiBlePolicy::CHARGING_HOLDOFF_MS :
                                       OxiBlePolicy::RECONNECT_HOLDOFF_MS) / 1000),
              until_absent ? "absent or deadline" : "deadline");
}


static void plx_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
    if (len < 5) return;
    uint16_t spo2_raw = data[1] | (data[2] << 8);
    uint16_t pr_raw = data[3] | (data[4] << 8);
    int16_t spo2 = parse_sfloat(spo2_raw);
    int16_t pr = parse_sfloat(pr_raw);
    if (spo2 > 0 && spo2 <= 100 && pr > 0 && pr < 500) {
        feed_ble_sample(spo2, pr, true);
    } else {
        feed_ble_sample(-1, -1, false);
    }
}

static void hr_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
    if (len < 2) return;
    uint8_t flags = data[0];
    uint16_t hr;
    if (flags & 0x01) {
        if (len < 3) return;
        hr = data[1] | (data[2] << 8);
    } else {
        hr = data[1];
    }
    // HR-only service - feed with current SpO2 from arbiter reading
    oxi_reading_t r;
    OxiArbiter::snapshot(r);
    if (hr > 0 && hr < 500) {
        feed_ble_sample(r.spo2, (int16_t)hr, r.valid);
    }
}

static void nonin_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
    if (len >= 5) {
        uint8_t spo2 = data[2];
        uint16_t pr = data[3] | (data[4] << 8);
        if (spo2 > 0 && spo2 <= 100 && pr > 0 && pr < 500) {
            feed_ble_sample((int8_t)spo2, (int16_t)pr, true);
        } else {
            feed_ble_sample(-1, -1, false);
        }
    }
}


// ACCARE WS20A: FE 5A LEN CMD PAYLOAD... SUM
// SUM is the low byte of the sum over LEN, CMD, and payload.
static NimBLERemoteCharacteristic *ws20a_write_chr = nullptr;
#define WS20A_RX_BUF_LEN 256
static uint8_t ws20a_rx_buf[WS20A_RX_BUF_LEN];
static size_t ws20a_rx_len = 0;
static uint32_t ws20a_frame_errors = 0;

static bool ws20a_send_command(uint8_t cmd, const uint8_t *payload, size_t payload_len) {
    uint8_t frame[32];
    if (!ws20a_write_chr || payload_len > sizeof(frame) - 5) return false;

    size_t frame_len = payload_len + 5;
    frame[0] = 0xFE;
    frame[1] = 0x5A;
    frame[2] = (uint8_t)frame_len;
    frame[3] = cmd;
    if (payload_len > 0) memcpy(frame + 4, payload, payload_len);

    uint8_t sum = 0;
    for (size_t i = 2; i < frame_len - 1; i++) sum += frame[i];
    frame[frame_len - 1] = sum;

    bool response = ws20a_write_chr->canWrite();
    if (!response && !ws20a_write_chr->canWriteNoResponse()) return false;
    if (!ws20a_write_chr->writeValue(frame, frame_len, response)) {
        Log::logf(CAT_OXI, LOG_WARN, "WS20A command 0x%02X write failed\n", cmd);
        return false;
    }

    Log::logf(CAT_OXI, LOG_DEBUG, "WS20A command 0x%02X sent\n", cmd);
    return true;
}

static void ws20a_process_frame(const uint8_t *frame, size_t frame_len) {
    uint8_t cmd = frame[3];
    const uint8_t *payload = frame + 4;
    size_t payload_len = frame_len - 5;

    if (cmd == 0x12) {
        Log::logf(CAT_OXI, LOG_DEBUG, "WS20A measurement started\n");
        return;
    }

    if (cmd != 0x10) {
        Log::logf(CAT_OXI, LOG_DEBUG, "WS20A RX cmd=0x%02X len=%u\n",
                  cmd, (unsigned)payload_len);
        return;
    }

    if (payload_len < 15) {
        Log::logf(CAT_OXI, LOG_WARN, "WS20A realtime packet too short: %u\n",
                  (unsigned)payload_len);
        return;
    }

    uint16_t pulse = ((uint16_t)payload[0] << 8) | payload[1];
    uint8_t spo2 = payload[2];
    uint16_t pi = ((uint16_t)payload[3] << 8) | payload[4];
    uint8_t battery = payload[5];
    uint8_t seq = payload[14];

    Log::logf(CAT_OXI, LOG_DEBUG,
              "WS20A: SpO2=%d HR=%d PI=%d battery=%d seq=%d\n",
              spo2, pulse, pi, battery, seq);

    if (spo2 >= 50 && spo2 <= 100 && pulse >= 25 && pulse <= 250) {
        feed_ble_sample((int8_t)spo2, (int16_t)pulse, true);
    } else {
        feed_ble_sample(-1, -1, false);
    }
}

static void ws20a_process_rx() {
    while (ws20a_rx_len >= 2) {
        if (ws20a_rx_buf[0] != 0xFE || ws20a_rx_buf[1] != 0x5A) {
            memmove(ws20a_rx_buf, ws20a_rx_buf + 1, --ws20a_rx_len);
            continue;
        }

        if (ws20a_rx_len < 3) return;
        size_t frame_len = ws20a_rx_buf[2];
        if (frame_len < 5) {
            ws20a_frame_errors++;
            Log::logf(CAT_OXI, LOG_WARN, "WS20A invalid frame length: %u\n",
                      (unsigned)frame_len);
            memmove(ws20a_rx_buf, ws20a_rx_buf + 1, --ws20a_rx_len);
            continue;
        }
        if (ws20a_rx_len < frame_len) return;

        uint8_t sum = 0;
        for (size_t i = 2; i < frame_len - 1; i++) sum += ws20a_rx_buf[i];
        if (sum != ws20a_rx_buf[frame_len - 1]) {
            ws20a_frame_errors++;
            Log::logf(CAT_OXI, LOG_WARN,
                      "WS20A checksum mismatch: expected=%02X actual=%02X errors=%lu\n",
                      ws20a_rx_buf[frame_len - 1], sum, (unsigned long)ws20a_frame_errors);
            memmove(ws20a_rx_buf, ws20a_rx_buf + 1, --ws20a_rx_len);
            continue;
        }

        ws20a_process_frame(ws20a_rx_buf, frame_len);
        ws20a_rx_len -= frame_len;
        if (ws20a_rx_len > 0) {
            memmove(ws20a_rx_buf, ws20a_rx_buf + frame_len, ws20a_rx_len);
        }
    }

    if (ws20a_rx_len == 1 && ws20a_rx_buf[0] != 0xFE) ws20a_rx_len = 0;
}

static void ws20a_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
    if (len > 0 && Log::get_cat_level(CAT_OXI) >= LOG_DEBUG) {
        char hex[64] = {};
        int n = len > 20 ? 20 : (int)len;
        for (int i = 0; i < n; i++) snprintf(hex + i*3, 4, "%02X ", data[i]);
        Log::logf(CAT_OXI, LOG_DEBUG, "WS20A RX len=%d: %s\n", (int)len, hex);
    }

    while (len > 0) {
        size_t available = sizeof(ws20a_rx_buf) - ws20a_rx_len;
        if (available == 0) {
            ws20a_frame_errors++;
            ws20a_rx_len = 0;
            available = sizeof(ws20a_rx_buf);
            Log::logf(CAT_OXI, LOG_WARN, "WS20A RX buffer overflow\n");
        }

        size_t chunk_len = len < available ? len : available;
        memcpy(ws20a_rx_buf + ws20a_rx_len, data, chunk_len);
        ws20a_rx_len += chunk_len;
        data += chunk_len;
        len -= chunk_len;
        ws20a_process_rx();
    }
}


// Viatom/Wellue: response packet header is 7 bytes (0x55, cmd, ~cmd, blk_lo, blk_hi, len_lo, len_hi)
// CMD_READ_SENSORS response: payload byte 0 = SpO2, byte 1 = HR
static NimBLERemoteCharacteristic *viatom_write_chr = nullptr;
static std::atomic<bool> viatom_initial_probe{false};
static std::atomic<bool> viatom_time_pending{false};
#define VIATOM_WRITE_CHUNK_LEN 20
#define VIATOM_WRITE_CHUNK_DELAY_MS 50

static void viatom_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
    if (len > 0 && Log::get_cat_level(CAT_OXI) >= LOG_DEBUG) {
        char hex[64] = {};
        int n = len > 20 ? 20 : len;
        for (int i = 0; i < n; i++) snprintf(hex + i*3, 4, "%02X ", data[i]);
        Log::logf(CAT_OXI, LOG_DEBUG, "Viatom RX len=%d: %s\n", len, hex);
    }

    // Response: 55 CMD ~CMD BLK_LO BLK_HI LEN_LO LEN_HI [payload...]
    // Byte 7 = SpO2, Byte 8 = HR, 0xFF = no finger, 0x00 = no reading
    // Read replies contain at least 12 payload bytes. A SetTIME ACK does not.
    if (len >= 19 && data[0] == 0x55 && data[1] == 0 && data[2] == 0xFF &&
        (data[5] | ((uint16_t)data[6] << 8)) >= 12) {
        if (data[15] == 1 || data[15] == 2) {
            charging_requested.store(true);
            return;
        }
        if (!viatom_initial_probe.exchange(true)) viatom_time_pending.store(true);
        uint8_t spo2 = data[7];
        uint16_t hr = data[8] | ((uint16_t)data[9] << 8);
        bool no_finger = (spo2 == 0 || spo2 == 0xFF || hr == 0 || hr == 0xFF);
        if (!no_finger && spo2 <= 100 && hr < 250) {
            feed_ble_sample((int8_t)spo2, (int16_t)hr, true);
        } else {
            feed_ble_sample(-1, -1, false);
        }
    }
}


// OxyII: A5 CMD ~CMD 00 SEQ LEN_LO LEN_HI PAYLOAD... CRC8
#define OXYII_CMD_LIVE_SAMPLES       0x04
#define OXYII_CMD_SETUP              0x10
#define OXYII_CMD_SET_TIME           0xC0
#define OXYII_CMD_AUTH               0xFF
#define OXYII_NO_PENDING_CMD         0xFE
#define OXYII_SENSOR_POLL_MS         1000
#define OXYII_RESPONSE_TIMEOUT_MS    1500
#define OXYII_RX_BUF_LEN             640
#define OXYII_TX_BUF_LEN             32

static const uint8_t oxyii_lepucloud_md5[16] = {
    0xC2, 0xA7, 0xCF, 0x50, 0xDA, 0xFE, 0xD8, 0x85,
    0xA8, 0xF8, 0xF7, 0xEA, 0xC4, 0x43, 0x35, 0xF3,
};

static NimBLERemoteCharacteristic *oxyii_write_chr = nullptr;
static uint8_t oxyii_rx_buf[OXYII_RX_BUF_LEN];
static size_t oxyii_rx_len = 0;
static size_t oxyii_rx_want = 0;
static volatile uint8_t oxyii_pending_cmd = OXYII_NO_PENDING_CMD;
static volatile uint32_t oxyii_pending_ms = 0;
static uint32_t oxyii_last_poll_ms = 0;
static uint8_t oxyii_sequence = 0;
static volatile bool oxyii_need_auth = false;
static volatile bool oxyii_need_setup = false;
static volatile bool oxyii_need_time_sync = false;
static bool oxyii_waiting_clock = false;
static std::atomic<bool> oxyii_problem{false};
static std::atomic<bool> oxyii_stream_started{false};

static const char *oxyii_command_name(uint8_t cmd) {
    switch (cmd) {
        case OXYII_CMD_LIVE_SAMPLES: return "live_samples";
        case OXYII_CMD_SETUP: return "setup";
        case OXYII_CMD_SET_TIME: return "set_time";
        case OXYII_CMD_AUTH: return "auth";
        case OXYII_NO_PENDING_CMD: return "none";
        default: return "unknown";
    }
}

static void oxyii_log_problem(uint8_t cmd, const char *reason) {
    bool already_reported = oxyii_problem.exchange(true);
    Log::logf(CAT_OXI, already_reported ? LOG_DEBUG : LOG_WARN,
              "OxyII %s: %s\n", oxyii_command_name(cmd), reason);
}

static void oxyii_reset_rx() {
    oxyii_rx_len = 0;
    oxyii_rx_want = 0;
}

static void oxyii_clear_pending() {
    oxyii_pending_cmd = OXYII_NO_PENDING_CMD;
    oxyii_pending_ms = 0;
}

static void oxyii_reset() {
    oxyii_write_chr = nullptr;
    oxyii_reset_rx();
    oxyii_clear_pending();
    oxyii_last_poll_ms = 0;
    oxyii_sequence = 0;
    oxyii_need_auth = false;
    oxyii_need_setup = false;
    oxyii_need_time_sync = false;
    oxyii_waiting_clock = false;
    oxyii_problem = false;
    oxyii_stream_started = false;
}

static void oxyii_process_frame(const uint8_t *frame, size_t frame_len) {
    if (frame_len < 8 || frame[0] != 0xA5) return;

    uint8_t cmd = frame[1];
    if (frame[2] != (uint8_t)~cmd) {
        Log::logf(CAT_OXI, LOG_DEBUG, "OxyII RX invalid command complement\n");
        return;
    }

    size_t payload_len = frame[5] | ((size_t)frame[6] << 8);
    if (payload_len + 8 != frame_len ||
        crc8_ccitt(frame, frame_len - 1) != frame[frame_len - 1]) {
        Log::logf(CAT_OXI, LOG_DEBUG, "OxyII RX decode failed len=%u\n",
                  (unsigned)frame_len);
        return;
    }

    uint8_t pending_cmd = oxyii_pending_cmd;
    oxyii_clear_pending();
    if (pending_cmd == OXYII_CMD_SETUP && cmd == OXYII_CMD_SETUP) {
        oxyii_need_time_sync = true;
        return;
    }
    if (pending_cmd == OXYII_CMD_SET_TIME && cmd == OXYII_CMD_SET_TIME) return;
    if (pending_cmd != OXYII_CMD_LIVE_SAMPLES || cmd != OXYII_CMD_LIVE_SAMPLES) {
        Log::logf(CAT_OXI, LOG_DEBUG, "OxyII RX ignored cmd=%s pending=%s\n",
                  oxyii_command_name(cmd), oxyii_command_name(pending_cmd));
        return;
    }

    if (payload_len < 9) {
        oxyii_log_problem(OXYII_CMD_LIVE_SAMPLES, "short response");
        return;
    }

    bool recovering = oxyii_problem.exchange(false);
    bool started = oxyii_stream_started.exchange(true);
    if (!started || recovering)
        Log::logf(CAT_OXI, LOG_INFO, "OxyII stream %s\n",
                  started ? "recovered" : "started");

    const uint8_t *payload = frame + 7;
    uint8_t spo2 = payload[6];
    uint8_t pulse = payload[8];
    bool valid = spo2 > 0 && spo2 <= 100 && pulse > 0 && pulse != 0xFF && pulse < 250;
    Log::logf(CAT_OXI, LOG_DEBUG, "OxyII: SpO2=%d HR=%d valid=%d\n",
              spo2, pulse, valid);
    if (valid) {
        feed_ble_sample((int8_t)spo2, (int16_t)pulse, true);
    } else {
        feed_ble_sample(-1, -1, false);
    }
}

static void oxyii_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
    if (!data || len == 0) return;

    if (data[0] == 0xA5) oxyii_reset_rx();
    if (oxyii_rx_len == 0 && data[0] != 0xA5) return;
    if (oxyii_rx_len + len > sizeof(oxyii_rx_buf)) {
        Log::logf(CAT_OXI, LOG_DEBUG, "OxyII RX buffer overflow\n");
        oxyii_reset_rx();
        return;
    }

    memcpy(oxyii_rx_buf + oxyii_rx_len, data, len);
    oxyii_rx_len += len;

    if (oxyii_rx_want == 0 && oxyii_rx_len >= 7) {
        size_t payload_len = oxyii_rx_buf[5] | ((size_t)oxyii_rx_buf[6] << 8);
        oxyii_rx_want = payload_len + 8;
        if (oxyii_rx_want > sizeof(oxyii_rx_buf)) {
            Log::logf(CAT_OXI, LOG_DEBUG, "OxyII RX too large: %u\n",
                      (unsigned)oxyii_rx_want);
            oxyii_reset_rx();
            return;
        }
    }

    if (oxyii_rx_want == 0 || oxyii_rx_len < oxyii_rx_want) return;
    oxyii_process_frame(oxyii_rx_buf, oxyii_rx_want);
    oxyii_reset_rx();
}

static bool oxyii_write_frame(uint8_t cmd, const uint8_t *payload, size_t payload_len) {
    if (!oxyii_write_chr || payload_len > 0xFFFF) return false;

    size_t frame_len = payload_len + 8;
    if (frame_len > OXYII_TX_BUF_LEN) return false;

    uint8_t frame[OXYII_TX_BUF_LEN] = {};
    frame[0] = 0xA5;
    frame[1] = cmd;
    frame[2] = (uint8_t)~cmd;
    frame[4] = oxyii_sequence++;
    frame[5] = payload_len & 0xFF;
    frame[6] = (payload_len >> 8) & 0xFF;
    if (payload && payload_len > 0) memcpy(frame + 7, payload, payload_len);
    frame[frame_len - 1] = crc8_ccitt(frame, frame_len - 1);
    return oxyii_write_chr->writeValue(frame, frame_len, false);
}

static bool oxyii_send_command(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                               uint32_t now_ms, bool expect_reply = true) {
    if (!oxyii_write_frame(cmd, payload, payload_len)) return false;

    if (expect_reply) {
        oxyii_pending_cmd = cmd;
        oxyii_pending_ms = now_ms;
    }
    Log::logf(CAT_OXI, LOG_DEBUG, "OxyII TX cmd=%s payload_len=%u\n",
              oxyii_command_name(cmd), (unsigned)payload_len);
    return true;
}

static bool oxyii_send_auth(uint32_t now_ms) {
    time_t seconds = time(nullptr);
    if (seconds < 1704067200) return false;

    uint8_t session_key[16] = {};
    for (size_t i = 0; i < 8; i++) session_key[i] = oxyii_lepucloud_md5[i * 2];
    session_key[8] = '0';
    session_key[9] = '0';
    session_key[10] = '0';
    session_key[11] = '0';

    uint32_t timestamp = (uint32_t)seconds;
    // The device expects bit shifts 0, 1, 2, and 3 here.
    for (uint8_t i = 0; i < 4; i++) {
        session_key[12 + i] = (timestamp >> i) & 0xFF;
    }

    uint8_t payload[16];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = session_key[i] ^ oxyii_lepucloud_md5[i];
    }
    return oxyii_send_command(OXYII_CMD_AUTH, payload, sizeof(payload), now_ms, false);
}

static bool oxyii_sync_datetime(uint32_t now_ms) {
    if (!WiFiSetup::time_synced()) return false;

    time_t now = time(nullptr);
    if (now < 1704067200) return false;

    struct tm t;
    localtime_r(&now, &t);

    uint8_t payload[8] = {};
    uint16_t year = t.tm_year + 1900;
    payload[0] = year & 0xFF;
    payload[1] = (year >> 8) & 0xFF;
    payload[2] = t.tm_mon + 1;
    payload[3] = t.tm_mday;
    payload[4] = t.tm_hour;
    payload[5] = t.tm_min;
    payload[6] = t.tm_sec;
    return oxyii_send_command(OXYII_CMD_SET_TIME, payload, sizeof(payload), now_ms);
}

static void oxyii_poll(uint32_t now_ms) {
    if (!oxyii_write_chr || !pClient || !pClient->isConnected()) return;

    if (oxyii_pending_cmd != OXYII_NO_PENDING_CMD &&
        now_ms - oxyii_pending_ms >= OXYII_RESPONSE_TIMEOUT_MS) {
        oxyii_log_problem(oxyii_pending_cmd, "response timeout");
        oxyii_reset_rx();
        oxyii_clear_pending();
    }
    if (oxyii_pending_cmd != OXYII_NO_PENDING_CMD) return;

    if (oxyii_need_auth) {
        if (time(nullptr) < 1704067200) {
            if (!oxyii_waiting_clock)
                Log::logf(CAT_OXI, LOG_INFO, "OxyII auth waiting for clock\n");
            oxyii_waiting_clock = true;
            return;
        }
        if (oxyii_send_auth(now_ms)) {
            oxyii_need_auth = false;
            oxyii_need_setup = true;
            Log::logf(CAT_OXI, LOG_DEBUG, "OxyII auth sent, starting setup\n");
        } else {
            oxyii_log_problem(OXYII_CMD_AUTH, "write failed");
        }
        return;
    }

    if (oxyii_need_setup) {
        uint8_t payload = 0;
        if (oxyii_send_command(OXYII_CMD_SETUP, &payload, sizeof(payload), now_ms)) {
            oxyii_need_setup = false;
        } else {
            oxyii_log_problem(OXYII_CMD_SETUP, "write failed");
        }
        return;
    }

    if (oxyii_need_time_sync) {
        if (!WiFiSetup::time_synced()) {
            Log::logf(CAT_OXI, LOG_DEBUG, "Skipping OxyII datetime - NTP not synced\n");
        } else if (!oxyii_sync_datetime(now_ms)) {
            oxyii_log_problem(OXYII_CMD_SET_TIME, "write failed");
        }
        oxyii_need_time_sync = false;
        return;
    }

    if (now_ms - oxyii_last_poll_ms < OXYII_SENSOR_POLL_MS) return;
    if (!oxyii_send_command(OXYII_CMD_LIVE_SAMPLES, nullptr, 0, now_ms)) {
        oxyii_log_problem(OXYII_CMD_LIVE_SAMPLES, "write failed");
    }
    oxyii_last_poll_ms = now_ms;
}

static bool has_oxyii_manufacturer(const NimBLEAdvertisedDevice *dev) {
    for (uint8_t i = 0; i < dev->getManufacturerDataCount(); i++) {
        std::string data = dev->getManufacturerData(i);
        if (data.size() < 2) continue;

        uint16_t company = (uint8_t)data[0] | ((uint16_t)(uint8_t)data[1] << 8);
        if (company == 0x036F || company == 0xF34E) return true;
    }
    return false;
}

static bool has_oximeter_name(const char *name) {
    static const char *const prefixes[] = {
        "Nonin", "O2 ", "O2Ring", "O2M", "S8-AW", "T8520_", "CheckMe",
        "CheckO2", "SleepU", "SleepO2", "WearO2", "KidsO2", "BabyO2",
        "OxyLink", "WS20", "ACCARE",
    };
    for (const char *prefix : prefixes)
        if (strncasecmp(name, prefix, strlen(prefix)) == 0) return true;
    return false;
}

class OxiScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        std::string name = dev->getName();
        const auto &address = dev->getAddress();
        const uint8_t *bytes = address.getVal();
        char addr[18];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 bytes[5], bytes[4], bytes[3], bytes[2], bytes[1], bytes[0]);

        portENTER_CRITICAL(&known_mux);
        bool background = observing;
        int index = known_index(addr);
        if (background && index >= 0) {
            uint32_t now = millis();
            auto &device = known_devices[index];
            if (!device.advertisements++) device.first_advertisement_ms = now;
            device.holdoff.update(now, true, observing_since);
            device.holdoff.last_seen = now;
            if (device.autoconnect && !device.holdoff.active &&
                auto_retry.ready(now) && !observed_pending) {
                strlcpy(observed_device.addr, addr, sizeof(observed_device.addr));
                strlcpy(observed_device.name, name.c_str(), sizeof(observed_device.name));
                observed_device.addr_type = address.getType();
                observed_device.rssi = dev->getRSSI();
                observed_device.received_ms = now;
                observed_device.observing_ms = observing_since;
                observed_device.first_advertisement_ms = device.first_advertisement_ms;
                observed_device.advertisements = device.advertisements;
                observed_pending = true;
            }
        }
        portEXIT_CRITICAL(&known_mux);
        if (background) return;
        // Passive advertisements need not contain a name or service UUID.
        bool known = known_contains(addr);
        bool is_oxi = has_oxyii_manufacturer(dev) ||
                       known ||
                       dev->isAdvertisingService(PLX_SERVICE_UUID) ||
                       dev->isAdvertisingService(NONIN_OXI_SERVICE_UUID) ||
                       dev->isAdvertisingService(HR_SERVICE_UUID) ||
                       dev->isAdvertisingService(VIATOM_SERVICE_UUID) ||
                       dev->isAdvertisingService(OXYII_SERVICE_UUID) ||
                       has_oximeter_name(name.c_str());

        if (is_oxi && scan_mutex && xSemaphoreTake(scan_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool duplicate = false;
            for (int i = 0; i < scan_result_count; i++)
                if (strcasecmp(scan_results[i].addr, addr) == 0) duplicate = true;
            if (!duplicate && scan_result_count < MAX_SCAN_RESULTS) {
                snprintf(scan_results[scan_result_count].addr,
                         sizeof(scan_results[scan_result_count].addr), "%s", addr);
                scan_results[scan_result_count].name = name.c_str();
                scan_results[scan_result_count].rssi = dev->getRSSI();
                scan_results[scan_result_count].addr_type = address.getType();
                scan_result_count++;
            }
            xSemaphoreGive(scan_mutex);
            Log::logf(CAT_OXI, LOG_DEBUG, "Found: %s (%s) RSSI=%d\n",
                          name.c_str(), addr, dev->getRSSI());
        }
    }

    void onScanEnd(const NimBLEScanResults &results, int reason) override {
        scan_end_reason = reason;
        scan_complete = true;
    }
};

static OxiScanCB scanCB;

class OxiClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *client) override {
        Log::logf(CAT_OXI, LOG_DEBUG, "BLE link established\n");
    }

    void onDisconnect(NimBLEClient *client, int reason) override {
        bool established = state == OXI_STREAMING &&
                           (!viatom_write_chr || viatom_initial_probe.load());
        Log::logf(CAT_OXI, established ? LOG_INFO : LOG_DEBUG,
                  "BLE disconnected addr=%s reason=0x%X\n",
                  client->getPeerAddress().toString().c_str(), reason);
        viatom_write_chr = nullptr;
        oxyii_reset();
        ws20a_write_chr = nullptr;
        ws20a_rx_len = 0;
        OxiArbiter::stop_feed(OXI_SRC_BLE);
        if (state == OXI_STREAMING || state == OXI_BONDING) {
            set_state(OXI_DISCONNECTED);
        }
    }

    bool onConnParamsUpdateRequest(NimBLEClient *client, const ble_gap_upd_params *params) override {
        return true;
    }

    void onPassKeyEntry(NimBLEConnInfo &connInfo) override {
        // "Just Works"
        NimBLEDevice::injectPassKey(connInfo, 0);
    }

    void onConfirmPasskey(NimBLEConnInfo &connInfo, uint32_t pin) override {
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
        status_revision.fetch_add(1);
        if (connInfo.isEncrypted()) {
            Log::logf(CAT_OXI, LOG_DEBUG, "Encrypted + bonded\n");
        } else {
            Log::logf(CAT_OXI, LOG_DEBUG, "Auth complete (no encryption)\n");
        }
    }
};

static OxiClientCB clientCB;


static bool subscribe_services(NimBLEClient *cl) {
    bool got_spo2 = false;
    bool got_hr = false;

    NimBLERemoteService *plxSvc = cl->getService(PLX_SERVICE_UUID);
    if (plxSvc) {
        NimBLERemoteCharacteristic *plxCont = plxSvc->getCharacteristic(PLX_CONTINUOUS_UUID);
        if (plxCont && plxCont->canNotify() && plxCont->subscribe(true, plx_notify_cb)) {
            Log::logf(CAT_OXI, LOG_DEBUG, "Subscribed PLX Continuous\n");
            got_spo2 = got_hr = true;
        }
        if (!got_spo2) {
            NimBLERemoteCharacteristic *plxSpot = plxSvc->getCharacteristic(PLX_SPOT_UUID);
            if (plxSpot && plxSpot->canIndicate() && plxSpot->subscribe(false, plx_notify_cb)) {
                Log::logf(CAT_OXI, LOG_DEBUG, "Subscribed PLX Spot\n");
                got_spo2 = got_hr = true;
            }
        }
    }

    if (!got_spo2) {
        NimBLERemoteService *noninSvc = cl->getService(NONIN_OXI_SERVICE_UUID);
        if (noninSvc) {
            NimBLERemoteCharacteristic *noninCont = noninSvc->getCharacteristic(NONIN_CONTINUOUS_UUID);
            if (noninCont && noninCont->canNotify() && noninCont->subscribe(true, nonin_notify_cb)) {
                Log::logf(CAT_OXI, LOG_DEBUG, "Subscribed Nonin Continuous\n");
                got_spo2 = got_hr = true;
            }
        }
    }

    // Some O2Ring-S expose Viatom too, but supply live samples over OxyII.
    if (!got_spo2) {
        NimBLERemoteService *oxyiiSvc = cl->getService(OXYII_SERVICE_UUID);
        if (oxyiiSvc) {
            NimBLERemoteCharacteristic *oxyiiNotify = oxyiiSvc->getCharacteristic(OXYII_NOTIFY_UUID);
            NimBLERemoteCharacteristic *oxyiiWrite = oxyiiSvc->getCharacteristic(OXYII_WRITE_UUID);
            if (oxyiiNotify && oxyiiNotify->canNotify() && oxyiiWrite) {
                oxyii_reset();
                oxyii_write_chr = oxyiiWrite;
                oxyii_need_auth = true;
                if (oxyiiNotify->subscribe(true, oxyii_notify_cb)) {
                    Log::logf(CAT_OXI, LOG_DEBUG, "Subscribed OxyII notify\n");
                    got_spo2 = got_hr = true;
                } else {
                    Log::logf(CAT_OXI, LOG_DEBUG, "OxyII notification subscribe failed\n");
                    oxyii_reset();
                }
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "OxyII characteristics unavailable: notify=%d write=%d\n",
                          oxyiiNotify && oxyiiNotify->canNotify(), oxyiiWrite != nullptr);
            }
        }
    }

    if (!got_spo2) {
        NimBLERemoteService *viatomSvc = cl->getService(VIATOM_SERVICE_UUID);
        if (viatomSvc) {
            NimBLERemoteCharacteristic *viatomRead = viatomSvc->getCharacteristic(VIATOM_READ_UUID);
            if (viatomRead && viatomRead->canNotify() && viatomRead->subscribe(true, viatom_notify_cb)) {
                Log::logf(CAT_OXI, LOG_DEBUG, "Subscribed Viatom read\n");
                viatom_initial_probe.store(false);
                viatom_time_pending.store(false);
                viatom_write_chr = viatomSvc->getCharacteristic(VIATOM_WRITE_UUID);
                if (viatom_write_chr) Log::logf(CAT_OXI, LOG_DEBUG, "Viatom write chr found\n");
                got_spo2 = got_hr = true;
            }
        }
    }

    if (!got_spo2) {
        NimBLERemoteService *wsNotifySvc = cl->getService(WS20A_NOTIFY_SERVICE_UUID);
        NimBLERemoteService *wsWriteSvc = cl->getService(WS20A_WRITE_SERVICE_UUID);
        NimBLERemoteCharacteristic *wsNotify = nullptr;
        if (wsNotifySvc) wsNotify = wsNotifySvc->getCharacteristic(WS20A_NOTIFY_UUID);
        if (!wsNotify && wsWriteSvc) wsNotify = wsWriteSvc->getCharacteristic(WS20A_NOTIFY_UUID);

        NimBLERemoteCharacteristic *wsWrite = nullptr;
        if (wsWriteSvc) wsWrite = wsWriteSvc->getCharacteristic(WS20A_WRITE_UUID);

        bool can_subscribe = wsNotify && (wsNotify->canNotify() || wsNotify->canIndicate());
        bool can_write = wsWrite && (wsWrite->canWrite() || wsWrite->canWriteNoResponse());
        if (can_subscribe && can_write) {
            bool notifications = wsNotify->canNotify();
            if (wsNotify->subscribe(notifications, ws20a_notify_cb)) {
                ws20a_write_chr = wsWrite;
                ws20a_rx_len = 0;
                ws20a_frame_errors = 0;
                uint8_t start_payload = 0x00;
                if (ws20a_send_command(0x12, &start_payload, 1)) {
                    Log::logf(CAT_OXI, LOG_DEBUG, "Subscribed WS20A realtime data\n");
                    got_spo2 = got_hr = true;
                } else {
                    ws20a_write_chr = nullptr;
                }
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG, "WS20A notification subscribe failed\n");
            }
        } else if (wsNotifySvc || wsWriteSvc) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "WS20A characteristics unavailable: notify=%d write=%d\n",
                      can_subscribe, can_write);
        }
    }

    if (!got_hr) {
        NimBLERemoteService *hrSvc = cl->getService(HR_SERVICE_UUID);
        if (hrSvc) {
            NimBLERemoteCharacteristic *hrMeas = hrSvc->getCharacteristic(HR_MEASUREMENT_UUID);
            if (hrMeas && hrMeas->canNotify() && hrMeas->subscribe(true, hr_notify_cb)) {
                Log::logf(CAT_OXI, LOG_DEBUG, "Subscribed Heart Rate\n");
                got_hr = true;
            }
        }
    }

    return got_spo2 || got_hr;
}

// Set date/time on Nonin devices so stored records have correct timestamps.
static void set_nonin_datetime(NimBLEClient *cl) {
    if (!WiFiSetup::time_synced()) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Skipping Nonin datetime - NTP not synced\n");
        return;
    }

    NimBLERemoteService *svc = cl->getService(NONIN_OXI_SERVICE_UUID);
    if (!svc) return;

    NimBLERemoteCharacteristic *cp = svc->getCharacteristic(NONIN_CONTROL_POINT_UUID);
    if (!cp || !cp->canWrite()) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Nonin control point not writable\n");
        return;
    }

    struct tm timeinfo;
    time_t now = time(nullptr);
    localtime_r(&now, &timeinfo);

    char ts[13];
    strftime(ts, sizeof(ts), "%y%m%d%H%M%S", &timeinfo);

    uint8_t cmd[] = {0x60, 0x4E, 0x4D, 0x49, 0x12, 0x44, 0x54, 0x4D, 0x3D,
                     0,0,0,0,0,0,0,0,0,0,0,0, 0x0D, 0x0A};
    memcpy(cmd + 9, ts, 12);

    if (cp->writeValue(cmd, sizeof(cmd), true)) {
        Log::logf(CAT_OXI, LOG_INFO, "Nonin datetime set: %s\n", ts);
    } else {
        Log::logf(CAT_OXI, LOG_WARN, "Nonin datetime write failed\n");
    }
}

static void set_viatom_datetime() {
    if (!WiFiSetup::time_synced() || !viatom_write_chr) return;

    struct tm t;
    time_t now = time(nullptr);
    localtime_r(&now, &t);

    // json {"SetTIME":"YYYY-MM-DD,HH:MM:SS"}
    char json[48];
    strftime(json, sizeof(json), "{\"SetTIME\":\"%Y-%m-%d,%H:%M:%S\"}", &t);
    int json_len = strlen(json);

    // AA CMD ~CMD BLK_LO BLK_HI LEN_LO LEN_HI [json] CRC8
    int pkt_len = 7 + json_len + 1;
    uint8_t pkt[64];
    pkt[0] = 0xAA;
    pkt[1] = 0x16;  // CMD_CONFIG
    pkt[2] = 0x16 ^ 0xFF;
    pkt[3] = 0x00; pkt[4] = 0x00;  // block
    pkt[5] = json_len & 0xFF;
    pkt[6] = (json_len >> 8) & 0xFF;
    memcpy(pkt + 7, json, json_len);

    uint8_t crc = crc8_ccitt(pkt, 7 + json_len);
    pkt[7 + json_len] = crc;

    bool ok = true;
    for (int off = 0; off < pkt_len; off += VIATOM_WRITE_CHUNK_LEN) {
        int chunk_len = pkt_len - off;
        if (chunk_len > VIATOM_WRITE_CHUNK_LEN) chunk_len = VIATOM_WRITE_CHUNK_LEN;
        if (!viatom_write_chr->writeValue(pkt + off, chunk_len, false)) {
            ok = false;
            break;
        }
        if (off + chunk_len < pkt_len) {
            vTaskDelay(pdMS_TO_TICKS(VIATOM_WRITE_CHUNK_DELAY_MS));
        }
    }

    if (ok) {
        Log::logf(CAT_OXI, LOG_INFO, "Viatom datetime set: %s\n", json);
    } else {
        Log::logf(CAT_OXI, LOG_WARN, "Viatom datetime write failed\n");
    }
}


static bool do_remove_known(const char *addr);
static void do_clear_all_known();

static bool init_stack() {
    if (!NimBLEDevice::init(Config::get().hostname.c_str())) return false;
    NimBLEDevice::setSecurityAuth(true, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    return true;
}

// Only the BLE task changes the stack lifecycle. A timed-out caller may request
// Resume while deinit is still running; never overwrite that cancellation.
static bool handle_memory_pause() {
    static bool released = false;
    MemoryPause phase = memory_pause.load();
    if (phase == MemoryPause::Requested &&
        memory_pause.compare_exchange_strong(phase, MemoryPause::Releasing)) {
        bool idle = !pClient->isConnected() && !OxiArbiter::is_feeding();
        if (idle) {
            if (Log::get_cat_level(CAT_OXI) >= LOG_DEBUG)
                Log::logf(CAT_OXI, LOG_DEBUG,
                      "Before OTA pause: internal=%u largest=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            NimBLEDevice::getScan()->stop();
            update_observer(false);
            xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
            released = NimBLEDevice::deinit(false);
            xSemaphoreGive(lifecycle_mutex);
            scan_complete = false;
            if (state != OXI_DISABLED) set_state(OXI_DISCONNECTED);
            if (!released || Log::get_cat_level(CAT_OXI) >= LOG_DEBUG)
                Log::logf(CAT_OXI, released ? LOG_DEBUG : LOG_ERROR,
                      "OTA memory release: %s internal=%u largest=%u\n",
                      released ? "done" : "failed",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        phase = MemoryPause::Releasing;
        memory_pause.compare_exchange_strong(
            phase, released ? MemoryPause::Released : MemoryPause::Rejected);
    }

    if (memory_pause.load() == MemoryPause::Resume) {
        bool restored = true;
        if (released) {
            xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
            restored = init_stack();
            xSemaphoreGive(lifecycle_mutex);
            if (restored) released = false;
            if (!restored || Log::get_cat_level(CAT_OXI) >= LOG_DEBUG)
                Log::logf(CAT_OXI, restored ? LOG_DEBUG : LOG_ERROR,
                      "OTA memory restore: %s internal=%u largest=%u\n",
                      restored ? "done" : "failed",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        memory_pause.store(restored ? MemoryPause::Ready : MemoryPause::Failed);
    }
    return memory_pause.load() != MemoryPause::Ready;
}

void OxiBle::task(void *param) {
    scan_mutex = xSemaphoreCreateMutex();
    known_load();
    xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
    init_stack();

    // Import legacy bonded-only sensors without removing any NimBLE bond.
    for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) {
        auto address = NimBLEDevice::getBondedAddress(i);
        known_add(address.toString().c_str(), address.getType(), true);
    }
    known_ready = true;

    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCB);
    pClient->setConnectionParams(12, 12, 0, 400);
    xSemaphoreGive(lifecycle_mutex);

    auto &cfg = Config::get();
    if (cfg.oxi_enabled) {
        set_state(OXI_DISCONNECTED);
    }

    bool enabled = cfg.oxi_enabled;
    bool config_enabled = enabled;
    uint32_t scan_retry_at = 0;
    bool manual_scan = false;
    char failed_addr[18] = {};
    const char *last_failure = nullptr;
    int last_connect_error = 0;
    char connected_name[32] = {};

    while (true) {
        if (handle_memory_pause()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (config_enabled != cfg.oxi_enabled) {
            config_enabled = cfg.oxi_enabled;
            if (config_enabled) enable_requested.store(true);
            else disable_requested = true;
        }
        if (enable_requested.exchange(false)) {
            enabled = true;
            if (state == OXI_DISABLED) set_state(OXI_DISCONNECTED);
        }

        if (stop_scan_requested.exchange(false)) {
            manual_scan = true;
            scan_requested = active_scan_requested = false;
            update_observer(false);
            NimBLEDevice::getScan()->stop();
        }

        if (disable_requested) {
            disable_requested = false;
            enabled = false;
            disconnect_requested = false;
            charging_requested.store(false);
            request_connect(CONN_NONE, nullptr);
            Log::logf(CAT_OXI, LOG_DEBUG, "Disable requested\n");
            update_observer(false);
            NimBLEDevice::getScan()->stop();
            scan_requested = active_scan_requested = false;
            if (pClient->isConnected()) pClient->disconnect();
            OxiArbiter::stop_feed(OXI_SRC_BLE);
            set_state(OXI_DISABLED);
        }

        bool charging = charging_requested.exchange(false);
        OxiBlePolicy::Disconnect data_problem = OxiBlePolicy::Disconnect::None;
        if (state == OXI_STREAMING) {
            portENTER_CRITICAL(&sample_mux);
            data_problem = ble_samples.check(millis());
            portEXIT_CRITICAL(&sample_mux);
        }
        if (disconnect_requested || charging || data_problem != OxiBlePolicy::Disconnect::None) {
            disconnect_requested = false;
            request_connect(CONN_NONE, nullptr);
            const auto addr = pClient->getPeerAddress().toString();
            if (charging) {
                bool changed = known_set_charging(addr.c_str(), true);
                Log::logf(CAT_OXI, changed ? LOG_INFO : LOG_DEBUG,
                          "Sensor charging addr=%s; retry in %lus\n", addr.c_str(),
                          (unsigned long)(OxiBlePolicy::CHARGING_HOLDOFF_MS / 1000));
                // The expected disconnect of a charging probe is not a link failure.
                set_state(OXI_DISCONNECTED);
            } else {
                const char *reason = data_problem == OxiBlePolicy::Disconnect::Silent ? "no samples for 10s" :
                    data_problem == OxiBlePolicy::Disconnect::Invalid ? "invalid samples for 30s" : "requested";
                Log::logf(CAT_OXI, data_problem == OxiBlePolicy::Disconnect::Silent ? LOG_WARN : LOG_INFO,
                          "Sensor disconnect: %s\n", reason);
            }
            if (charging || pClient->isConnected()) {
                // Sparse passive observation cannot prove an idle sensor powered off.
                bool until_absent = !charging && data_problem == OxiBlePolicy::Disconnect::None;
                hold_autoconnect(addr.c_str(), charging, until_absent);
            }
            update_observer(false);
            NimBLEDevice::getScan()->stop();
            if (pClient->isConnected()) pClient->disconnect();
            OxiArbiter::stop_feed(OXI_SRC_BLE);
            set_state(OXI_DISCONNECTED);
        }

        if (suspend_enter_requested) {
            suspend_enter_requested = false;
            Log::logf(CAT_OXI, LOG_DEBUG, "Suspend: stopping scan and dropping connection\n");
            NimBLEDevice::getScan()->stop();
            update_observer(false);
            if (pClient->isConnected()) pClient->disconnect();
            OxiArbiter::stop_feed(OXI_SRC_BLE);
            scan_requested = false;
            request_connect(CONN_NONE, nullptr);
            if (state != OXI_DISABLED) set_state(OXI_DISCONNECTED);
        }

        if (del_one_requested) {
            del_one_requested = false;
            char addr[18];
            strncpy(addr, del_one_addr, sizeof(addr));
            Log::logf(CAT_OXI, LOG_DEBUG, "Remove-known requested: %s\n", addr);
            NimBLEDevice::getScan()->stop();
            update_observer(false);
            if (pClient->isConnected()) pClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
            bool ok = do_remove_known(addr);
            status_revision.fetch_add(1);
            Log::logf(CAT_OXI, LOG_DEBUG, "Remove-known %s: %s\n",
                      addr, ok ? "done" : "not found");
        }

        if (del_all_requested) {
            del_all_requested = false;
            Log::logf(CAT_OXI, LOG_DEBUG, "Clear-all-known requested\n");
            NimBLEDevice::getScan()->stop();
            update_observer(false);
            if (pClient->isConnected()) pClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
            do_clear_all_known();
        }

        if (scan_complete) {
            scan_complete = false;
            if (state == OXI_SCANNING) {
                status_revision.fetch_add(1);
                Log::logf(CAT_OXI, manual_scan ? LOG_INFO : LOG_DEBUG,
                          "%s scan ended candidates=%d reason=%d\n",
                          manual_scan ? "Manual" : "Background",
                          scan_result_count, scan_end_reason);
                set_state(enabled ? OXI_DISCONNECTED : OXI_DISABLED);
            } else if (state == OXI_OBSERVING) {
                update_observer(false);
                set_state(enabled ? OXI_DISCONNECTED : OXI_DISABLED);
            }
        }

        if (scan_requested && !ble_suspended) {
            scan_requested = false;
            NimBLEScan *pScan = NimBLEDevice::getScan();
            update_observer(false);
            if (pScan->isScanning()) {
                if (active_scan_requested) {
                    pScan->stop();
                    scan_requested = true;
                } else {
                    Log::logf(CAT_OXI, LOG_DEBUG, "scan_requested ignored, scan already in progress\n");
                }
            } else {
                if (pClient->isConnected()) pClient->disconnect();
                if (scan_mutex) xSemaphoreTake(scan_mutex, portMAX_DELAY);
                for (auto &result : scan_results) {
                    result.~oxi_scan_result_t();
                    new (&result) oxi_scan_result_t{};
                }
                scan_result_count = 0;
                if (scan_mutex) xSemaphoreGive(scan_mutex);
                scan_complete = false;
                pScan->clearResults();   // defeat NimBLE dedup across scans
                set_state(OXI_SCANNING);
                Log::logf(CAT_OXI, LOG_DEBUG, "Starting scan (%dms)\n", SCAN_DURATION_MS);
                pScan->setScanCallbacks(&scanCB);
                pScan->setDuplicateFilter(true);
                pScan->setMaxResults(0);  // callbacks own the bounded oximeter list
                bool active = active_scan_requested;
                active_scan_requested = false;
                manual_scan = active;
                // Manual discovery is separate from passive observation.
                pScan->setActiveScan(active);
                pScan->setInterval(active ? 100 : 1000);
                pScan->setWindow(active ? 99 : 20);
                if (!pScan->start(SCAN_DURATION_MS)) {
                    set_state(enabled ? OXI_DISCONNECTED : OXI_DISABLED);
                }
            }
        }

        bool auto_allowed = enabled && !ble_suspended &&
                            OxiArbiter::active_source() != OXI_SRC_UDP;
        ObservedDevice automatic;
        bool have_automatic = false;
        portENTER_CRITICAL(&known_mux);
        if (observed_pending && auto_allowed && observing) {
            int index = known_index(observed_device.addr);
            if (index >= 0 && known_devices[index].autoconnect &&
                !known_devices[index].holdoff.active && auto_retry.ready(millis())) {
                automatic = observed_device;
                have_automatic = true;
            }
        }
        observed_pending = false;
        portEXIT_CRITICAL(&known_mux);
        if (have_automatic) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Auto-connect advertisement addr=%s at=%lu observing=%lu first=%lu count=%lu rssi=%d handoff=%lums\n",
                      automatic.addr, (unsigned long)automatic.received_ms,
                      (unsigned long)automatic.observing_ms,
                      (unsigned long)automatic.first_advertisement_ms,
                      (unsigned long)automatic.advertisements, automatic.rssi,
                      (unsigned long)(millis() - automatic.received_ms));
            request_connect(CONN_AUTO, automatic.addr);
        }

        ConnectRequest connection;
        if (!ble_suspended && take_connect_request(connection)) {
            uint32_t sequence_started_ms = millis();
            connect_mode_t mode = connection.mode;
            update_observer(false);
            NimBLEDevice::getScan()->stop();
            // Wait for scan to actually stop before connecting
            for (int i = 0; i < 20 && NimBLEDevice::getScan()->isScanning(); i++)
                vTaskDelay(pdMS_TO_TICKS(50));
            scan_complete = false;  // discard any pending scan-complete trigger

            String addr = connection.addr;
            if (addr.length() == 0 && scan_result_count > 0)
                addr = scan_results[0].addr;

            int max_attempts = (mode == CONN_USER) ? USER_CONNECT_RETRIES : 1;

            if (addr.length() > 0) {
                set_state(OXI_CONNECTING);

                uint8_t atype = 1;
                String dev_name = "";
                // Empty advertisements fall back to the name learned earlier.
                char saved_name[sizeof(known_devices[0].name)] = {};
                portENTER_CRITICAL(&known_mux);
                int known = known_index(addr.c_str());
                if (known >= 0) {
                    atype = known_devices[known].addr_type;
                    strlcpy(saved_name, known_devices[known].name, sizeof(saved_name));
                }
                portEXIT_CRITICAL(&known_mux);
                for (int i = 0; i < scan_result_count; i++) {
                    if (strcasecmp(scan_results[i].addr, addr.c_str()) == 0) {
                        atype = scan_results[i].addr_type;
                        dev_name = scan_results[i].name;
                        break;
                    }
                }
                if (mode == CONN_AUTO && have_automatic) {
                    atype = automatic.addr_type;
                    dev_name = automatic.name;
                }

                NimBLEAddress bleAddr(std::string(addr.c_str()), atype);
                // Bonded Nonin may omit its name from passive advertisements.
                device_needs_encryption = strncasecmp(dev_name.c_str(), "Nonin", 5) == 0 ||
                                          NimBLEDevice::isBonded(bleAddr);
                bool connected = false;
                const char *failure = "connect";
                int connect_error = 0;

                for (int attempt = 1; attempt <= max_attempts; attempt++) {
                    if (disconnect_requested || disable_requested) break;

                    if (attempt > 1) {
                        Log::logf(CAT_OXI, LOG_DEBUG, "Retry %d/%d after %dms\n",
                                  attempt, max_attempts, USER_RETRY_DELAY_MS);
                        vTaskDelay(pdMS_TO_TICKS(USER_RETRY_DELAY_MS));
                    }

                    // cancel any pending connection and clean up stale state
                    if (pClient->isConnected()) {
                        Log::logf(CAT_OXI, LOG_DEBUG, "Disconnecting stale connection\n");
                        pClient->disconnect();
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    pClient->cancelConnect();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (disconnect_requested || disable_requested || ble_suspended) break;

                    failure = "connect";
                    connect_error = 0;
                    // connect
                    set_state(OXI_CONNECTING);
                    Log::logf(CAT_OXI, LOG_DEBUG, "Connecting to %s (type=%d, %s, attempt %d/%d)...\n",
                              addr.c_str(), atype, mode == CONN_USER ? "user" : "auto",
                              attempt, max_attempts);

                    uint32_t connect_started_ms = millis();
                    bool ok = pClient->connect(bleAddr);
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "BLE connect addr=%s at=%lu elapsed=%lums ok=%d\n",
                              addr.c_str(), (unsigned long)connect_started_ms,
                              (unsigned long)(millis() - connect_started_ms), ok);

                    if (!ok) {
                        int err = pClient->getLastError();
                        connect_error = err;
                        Log::logf(CAT_OXI, LOG_DEBUG, "connect() failed (err=%d)\n", err);

                        // EALREADY: previous connect still in flight
                        if (err == 2) {
                            Log::logf(CAT_OXI, LOG_DEBUG, "Waiting for pending connect...\n");
                            for (int i = 0; i < 50 && !pClient->isConnected(); i++)
                                vTaskDelay(pdMS_TO_TICKS(200));
                            ok = pClient->isConnected();
                            Log::logf(CAT_OXI, LOG_DEBUG, "Pending connect %s\n", ok ? "succeeded" : "failed");
                            if (ok) vTaskDelay(pdMS_TO_TICKS(500));
                        }

                        // EDONE: stale bond - delete and retry within this attempt
                        if (!ok && err == 13) {
                            Log::logf(CAT_OXI, LOG_INFO, "Removing stale bond and retrying\n");
                            NimBLEDevice::deleteBond(bleAddr);
                            vTaskDelay(pdMS_TO_TICKS(500));
                            ok = pClient->connect(bleAddr);
                            if (!ok) {
                                connect_error = pClient->getLastError();
                                Log::logf(CAT_OXI, LOG_DEBUG, "Post-bond-delete retry failed (err=%d)\n",
                                          connect_error);
                            }
                        }
                    }

                    if (!ok) continue;

                    // encrypt for devices that require it 
                    if (device_needs_encryption) {
                        failure = "encryption";
                        set_state(OXI_BONDING);
                        Log::logf(CAT_OXI, LOG_DEBUG, "Initiating encryption\n");

                        bool secured = pClient->secureConnection(false);
                        connect_error = secured ? 0 : pClient->getLastError();
                        Log::logf(CAT_OXI, LOG_DEBUG, "Encryption: secure=%d connected=%d err=%d\n",
                                  secured, pClient->isConnected(), pClient->getLastError());

                        if (!pClient->isConnected()) {
                            Log::logf(CAT_OXI, LOG_DEBUG, "Lost connection during encryption\n");
                            continue;
                        }

                        if (!secured) {
                            Log::logf(CAT_OXI, LOG_DEBUG, "Encryption failed, disconnecting to retry\n");
                            pClient->disconnect();
                            vTaskDelay(pdMS_TO_TICKS(500));
                            continue;
                        }
                    } else {
                        Log::logf(CAT_OXI, LOG_DEBUG, "Skipping encryption (not required)\n");
                    }

                    // subscribe
                    failure = "subscription";
                    connect_error = 0;
                    charging_requested.store(false);
                    portENTER_CRITICAL(&sample_mux);
                    ble_samples.start(millis());
                    portEXIT_CRITICAL(&sample_mux);
                    if (!subscribe_services(pClient)) {
                        Log::logf(CAT_OXI, LOG_DEBUG, "No suitable services, disconnecting\n");
                        pClient->disconnect();
                        continue;
                    }

                    // Final check - onDisconnect may have fired during subscribe
                    if (!pClient->isConnected()) {
                        Log::logf(CAT_OXI, LOG_DEBUG, "Connection lost after subscribe\n");
                        continue;
                    }

                    set_nonin_datetime(pClient);
                    // Probe Viatom first: charging devices must not be kept awake
                    // by configuration writes before their charging state is known.
                    OxiArbiter::set_source(pClient->getPeerAddress().toString().c_str(),
                                           dev_name.length() ? dev_name.c_str() : saved_name);
                    portENTER_CRITICAL(&sample_mux);
                    ble_samples.subscribed(millis());
                    portEXIT_CRITICAL(&sample_mux);
                    set_state(OXI_STREAMING);
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "BLE setup addr=%s at=%lu elapsed=%lums\n",
                              addr.c_str(), (unsigned long)millis(),
                              (unsigned long)(millis() - sequence_started_ms));
                    strlcpy(connected_name, dev_name.c_str(), sizeof(connected_name));
                    Log::logf(CAT_OXI, viatom_write_chr ? LOG_DEBUG : LOG_INFO,
                              "%s addr=%s name=\"%s\"%s\n",
                              viatom_write_chr ? "Sensor probe subscribed" : "Sensor subscribed",
                              addr.c_str(), dev_name.c_str(),
                              oxyii_write_chr ? "; initializing OxyII" : "");

                    if ((mode == CONN_USER || known_contains(addr.c_str())) &&
                        !known_add(addr.c_str(), atype, false, dev_name.c_str())) {
                        Log::logf(CAT_OXI, LOG_WARN, "Sensor connected but not saved: known list full or NVS write failed\n");
                    }
                    failed_addr[0] = 0;
                    last_failure = nullptr;
                    connected = true;
                    break;
                }

                if (!connected) {
                    bool cancelled = disconnect_requested || disable_requested || ble_suspended;
                    bool repeated = strcmp(failed_addr, addr.c_str()) == 0 &&
                                    last_failure && strcmp(last_failure, failure) == 0 &&
                                    last_connect_error == connect_error;
                    Log::logf(CAT_OXI, cancelled ? LOG_DEBUG :
                              mode == CONN_USER ? LOG_ERROR : repeated ? LOG_DEBUG : LOG_WARN,
                              "Sensor connect %s addr=%s stage=%s error=%d\n",
                              cancelled ? "cancelled" : "failed", addr.c_str(), failure, connect_error);
                    if (!cancelled) {
                        strlcpy(failed_addr, addr.c_str(), sizeof(failed_addr));
                        last_failure = failure;
                        last_connect_error = connect_error;
                    }
                    if (pClient->isConnected()) pClient->disconnect();
                    set_state(OXI_DISCONNECTED);
                }
                if (mode == CONN_AUTO && !disconnect_requested && !disable_requested && !ble_suspended) {
                    portENTER_CRITICAL(&known_mux);
                    auto_retry.result(connected, millis());
                    uint32_t wait_ms = connected ? 0 : auto_retry.deadline - millis();
                    portEXIT_CRITICAL(&known_mux);
                    if (!connected) Log::logf(CAT_OXI, LOG_DEBUG, "Auto-connect retry in %lums\n", (unsigned long)wait_ms);
                }
            }
        }

        NimBLEScan *scanner = NimBLEDevice::getScan();
        bool has_auto = false;
        portENTER_CRITICAL(&known_mux);
        for (int i = 0; i < known_count; i++) has_auto |= known_devices[i].autoconnect;
        portEXIT_CRITICAL(&known_mux);
        auto_allowed = enabled && !ble_suspended && has_auto &&
                       OxiArbiter::active_source() != OXI_SRC_UDP;
        if (state == OXI_OBSERVING && !auto_allowed) {
            update_observer(false);
            scanner->stop();
            set_state(enabled ? OXI_DISCONNECTED : OXI_DISABLED);
        }
        if (state == OXI_DISCONNECTED && auto_allowed && !scan_requested &&
            (!scan_retry_at || static_cast<int32_t>(millis() - scan_retry_at) >= 0) &&
            !scanner->isScanning()) {
            scan_complete = false;
            scanner->clearResults();
            scanner->setScanCallbacks(&scanCB, true);
            scanner->setDuplicateFilter(false);
            scanner->setMaxResults(0);
            scanner->setActiveScan(false);
            scanner->setInterval(OBSERVE_INTERVAL_MS);
            scanner->setWindow(OBSERVE_WINDOW_MS);
            update_observer(true);
            if (scanner->start(0)) {
                scan_retry_at = 0;
                set_state(OXI_OBSERVING);
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "Known sensor observation started at=%lu interval=%ums window=%ums\n",
                          (unsigned long)observing_since, OBSERVE_INTERVAL_MS,
                          OBSERVE_WINDOW_MS);
            } else {
                update_observer(false);
                scan_retry_at = millis() + 1000;
            }
        }
        update_observer(state == OXI_OBSERVING && scanner->isScanning());

        // Viatom: poll sensor readings every 2s while streaming
        if (state == OXI_STREAMING && viatom_write_chr && pClient->isConnected()) {
            static uint32_t last_viatom_poll = 0;
            if (viatom_time_pending.exchange(false) && !charging_requested.load()) {
                const auto addr = pClient->getPeerAddress().toString();
                bool charging_ended = known_set_charging(addr.c_str(), false);
                Log::logf(CAT_OXI, LOG_INFO, "Sensor subscribed addr=%s name=\"%s\"%s\n",
                          addr.c_str(), connected_name, charging_ended ? "; charging ended" : "");
                set_viatom_datetime();
                last_viatom_poll = millis();
            } else if (millis() - last_viatom_poll >= 2000) {
                last_viatom_poll = millis();
                // CMD_READ_SENSORS packet: AA 17 E8 00 00 00 00 CRC
                uint8_t cmd[] = {0xAA, 0x17, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x00};
                cmd[7] = crc8_ccitt(cmd, 7);
                viatom_write_chr->writeValue(cmd, sizeof(cmd), false);
            }
        }

        if (state == OXI_STREAMING && oxyii_write_chr && pClient->isConnected()) {
            oxyii_poll(millis());
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


void OxiBle::init() {
    lifecycle_mutex = xSemaphoreCreateMutex();
    known_store_mutex = xSemaphoreCreateMutex();
    if (!lifecycle_mutex || !known_store_mutex) return;
    xTaskCreatePinnedToCore(OxiBle::task, "ble_oxi", OXI_TASK_STACK,
                            nullptr, OXI_TASK_PRIO, &oxi_task_handle, 0);
}

void OxiBle::start_scan()  { active_scan_requested = true; scan_requested = true; }
void OxiBle::stop_scan()   { stop_scan_requested.store(true); }

void OxiBle::connect(const char *addr) {
    request_connect(CONN_USER, addr);
}

void OxiBle::disconnect()  { disconnect_requested = true; }
void OxiBle::disable()     { disable_requested = true; }
void OxiBle::enable() {
    enable_requested.store(true);
}
void OxiBle::suspend() {
    if (!ble_suspended) {
        ble_suspended = true;
        suspend_enter_requested = true;
    }
}
void OxiBle::resume() {
    if (ble_suspended) {
        ble_suspended = false;
        Log::logf(CAT_OXI, LOG_DEBUG, "Resumed\n");
        // Task's auto-reconnect will pick up from OXI_DISCONNECTED.
    }
}

bool OxiBle::release_memory(uint32_t timeout_ms) {
    if (!oxi_task_handle) return false;
    MemoryPause expected = MemoryPause::Ready;
    if (!memory_pause.compare_exchange_strong(expected, MemoryPause::Requested)) return false;

    uint32_t started = millis();
    do {
        MemoryPause phase = memory_pause.load();
        if (phase == MemoryPause::Released) return true;
        if (phase == MemoryPause::Rejected) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (millis() - started < timeout_ms);

    memory_pause.store(MemoryPause::Resume);
    return false;
}

bool OxiBle::restore_memory(uint32_t timeout_ms) {
    memory_pause.store(MemoryPause::Resume);
    uint32_t started = millis();
    do {
        MemoryPause phase = memory_pause.load();
        if (phase == MemoryPause::Ready) return true;
        if (phase == MemoryPause::Failed) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (millis() - started < timeout_ms);
    return false;
}

oxi_state_t OxiBle::get_state()            { return state; }
uint32_t OxiBle::revision()               { return status_revision.load(); }

int OxiBle::get_scan_results(oxi_scan_result_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    if (scan_mutex) xSemaphoreTake(scan_mutex, portMAX_DELAY);
    n = (scan_result_count < max) ? scan_result_count : max;
    for (int i = 0; i < n; i++) out[i] = scan_results[i];  // String deep-copy
    if (scan_mutex) xSemaphoreGive(scan_mutex);
    return n;
}

int OxiBle::get_known_devices(oxi_known_device_t *out, int max) {
    if (!out || max <= 0) return 0;
    portENTER_CRITICAL(&known_mux);
    int n = known_count < max ? known_count : max;
    for (int i = 0; i < n; i++) out[i] = known_devices[i];
    portEXIT_CRITICAL(&known_mux);
    return n;
}

bool OxiBle::set_autoconnect(const char *addr, bool enabled) {
    char key[15];
    if (!autoconnect_key(addr, key) || !known_store_mutex || !known_ready) return false;
    xSemaphoreTake(known_store_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&known_mux);
    int index = known_index(addr);
    bool unchanged = index >= 0 && known_devices[index].autoconnect == enabled;
    portEXIT_CRITICAL(&known_mux);
    bool ok = unchanged;
    if (index >= 0 && !unchanged) {
        Preferences p;
        if (p.begin("oxi_known", false)) {
            ok = p.putBool(key, enabled) > 0;
            p.end();
        }
        if (ok) {
            portENTER_CRITICAL(&known_mux);
            known_devices[index].autoconnect = enabled;
            if (!enabled) known_devices[index].holdoff = {};
            portEXIT_CRITICAL(&known_mux);
            status_revision.fetch_add(1);
        }
    }
    xSemaphoreGive(known_store_mutex);
    return ok;
}

// Internal: must run on the BLE task (touches NimBLE + NVS).
static bool do_remove_known(const char *addr) {
    bool removed = false;
    int nb = NimBLEDevice::getNumBonds();
    for (int i = 0; i < nb; i++) {
        NimBLEAddress ba = NimBLEDevice::getBondedAddress(i);
        if (strcasecmp(ba.toString().c_str(), addr) == 0) {
            int rc = ble_gap_unpair(ba.getBase());
            if (rc == 0) removed = true;
            Log::logf(CAT_OXI, LOG_DEBUG, "Unpair %s rc=%d\n", addr, rc);
            break;
        }
    }
    if (known_remove(addr)) removed = true;
    return removed;
}

static void do_clear_all_known() {
    NimBLEDevice::deleteAllBonds();
    known_clear();
}

void OxiBle::request_remove_known(const char *addr) {
    if (!addr) return;
    strncpy(del_one_addr, addr, sizeof(del_one_addr) - 1);
    del_one_addr[sizeof(del_one_addr) - 1] = '\0';
    del_one_requested = true;
}

void OxiBle::request_clear_all_known() {
    del_all_requested = true;
}
