#include "wifi.h"
#include "app_config.h"
#include "debug_log.h"
#include "web_ui.h"
#include "network_hints.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_smartconfig.h>
#include <esp_sntp.h>
#include <time.h>

typedef enum {
    WF_OFF,
    WF_HINT_TRY,        // fast reconnect via BSSID+channel hint
    WF_SCANNING,
    WF_CONNECTING,      // WiFi.begin() called, waiting for IP
    WF_PMF_RETRY,       // re-tried CONNECTING with pmf_cfg disabled
    WF_CONNECTED,
    WF_ROAM_SCAN,       // scanning for a better AP
    WF_AP_FALLBACK,     // AP+STA mode, periodically retrying STA
    WF_SMARTCONFIG,
} wifi_state_t;

static wifi_state_t wf_state = WF_OFF;
static uint32_t state_entered_ms = 0;
static bool ntp_done = false;

static int8_t last_seen_rssi[WIFI_MAX_NETWORKS] = {};
static uint8_t connect_idx = 0xFF;
static uint8_t try_order[WIFI_MAX_NETWORKS];
static uint8_t try_count = 0;
static uint8_t try_pos = 0;
static uint8_t connect_retries = 0;

#define ROAM_CHECK_INTERVAL_MS  60000
#define ROAM_RSSI_THRESHOLD     (-73)
#define ROAM_CONSECUTIVE_LOW    3
#define ROAM_HYSTERESIS_DB      8
static uint32_t last_roam_check = 0;
static uint8_t low_rssi_count = 0;
static bool roaming_suspended = false;

#define AP_RETRY_INTERVAL_MS    30000
static uint32_t last_ap_retry = 0;

#define BG_SCAN_INTERVAL_MS     120000
static uint32_t last_bg_scan = 0;

#define HINT_TIMEOUT_MS         5000
#define CONNECT_TIMEOUT_MS      15000
#define SMARTCONFIG_TIMEOUT_MS  60000
#define CONNECT_RETRIES         2

static volatile bool ntp_synced = false;
static bool got_ip = false;
static bool sta_disconnected = false;
static volatile bool hint_refresh_pending = false;
static volatile uint8_t last_disconnect_reason = 0;
static bool pending_pmf_disable = false;


static void ntp_sync_cb(struct timeval *tv) {
    ntp_synced = true;
    struct tm t;
    time_t now = time(nullptr);
    localtime_r(&now, &t);
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] NTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
              t.tm_hour, t.tm_min, t.tm_sec);
}

static void sync_ntp() {
    auto &cfg = Config::get();
    if (cfg.tz.length() > 0) {
        setenv("TZ", cfg.tz.c_str(), 1);
        tzset();
    }
    sntp_set_time_sync_notification_cb(ntp_sync_cb);
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    if (cfg.ntp_server.length() > 0) {
#if LWIP_DHCP_GET_NTP_SRV
        esp_sntp_servermode_dhcp(false);
#endif
        esp_sntp_setservername(0, cfg.ntp_server.c_str());
        Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] NTP: configured server %s\n", cfg.ntp_server.c_str());
    } else {
#if LWIP_DHCP_GET_NTP_SRV
        esp_sntp_servermode_dhcp(true);
#endif
        esp_sntp_setservername(0, "pool.ntp.org");
        Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] NTP: DHCP + pool.ntp.org fallback\n");
    }
    esp_sntp_init();
}


static void wifi_event_cb(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        got_ip = true;
        hint_refresh_pending = true;
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        sta_disconnected = true;
        last_disconnect_reason = info.wifi_sta_disconnected.reason;
        break;
    default:
        break;
    }
}


static void set_state(wifi_state_t s) {
    wf_state = s;
    state_entered_ms = millis();
}

static String ap_ssid_str() {
    auto &cfg = Config::get();
    return cfg.hostname + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

static uint8_t find_net_by_ssid(const char *ssid) {
    auto &cfg = Config::get();
    for (uint8_t i = 0; i < cfg.wifi_net_count; i++) {
        if (cfg.wifi_nets[i].enabled && cfg.wifi_nets[i].ssid.equalsIgnoreCase(ssid))
            return i;
    }
    return 0xFF;
}

static void apply_pmf_override(bool disable) {
    wifi_config_t wcfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &wcfg) != ESP_OK) return;
    wcfg.sta.pmf_cfg.capable = !disable;
    wcfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
}

// Some routers (e.g. older OpenWrt builds, certain ISP-provisioned units)
// advertise PMF capability but reject the handshake with reason 208
// (WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG). Retry once with pmf_cfg cleared.
static void enter_pmf_retry() {
    auto &cfg = Config::get();
    if (connect_idx >= cfg.wifi_net_count) return;
    WiFiNetwork &net = cfg.wifi_nets[connect_idx];

    Log::logf(CAT_WIFI, LOG_INFO,
              "[WIFI] PMF retry: disabling pmf_cfg and reconnecting to '%s'\n",
              net.ssid.c_str());

    apply_pmf_override(true);
    pending_pmf_disable = true;

    // Reuse the staged credentials (set by the previous WiFi.begin) to avoid
    // re-resetting pmf_cfg. esp_wifi_disconnect aborts the in-flight assoc;
    // esp_wifi_connect re-attempts with the new pmf_cfg.
    esp_wifi_disconnect();
    delay(50);
    esp_wifi_connect();
    set_state(WF_PMF_RETRY);
}

static void begin_connect(uint8_t idx, bool use_hint) {
    auto &cfg = Config::get();
    if (idx >= cfg.wifi_net_count) return;
    WiFiNetwork &net = cfg.wifi_nets[idx];

    connect_idx = idx;
    connect_retries = 0;
    set_state(use_hint ? WF_HINT_TRY : WF_CONNECTING);

    NetworkHint *h = use_hint ? NetworkHints::find_best(net.ssid.c_str()) : nullptr;

    if (h) {
        Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Fast connect to '%s' ch=%d\n",
                  net.ssid.c_str(), h->channel);
        WiFi.begin(net.ssid.c_str(), net.pass.c_str(), h->channel, h->bssid);
    } else {
        Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Connecting to '%s'...\n", net.ssid.c_str());
        WiFi.begin(net.ssid.c_str(), net.pass.c_str());
        set_state(WF_CONNECTING);
    }

    // the cached hint says this BSSID needs PMF off, disable pmf_cfg and
    // bounce the connect so the actual association attempt has it off.
    // WiFi.begin always resets pmf_cfg.capable=true, so the override has to
    // happen after begin().
    if (h && (h->flags & HINT_FLAG_PMF_DISABLE)) {
        Log::logf(CAT_WIFI, LOG_INFO,
                  "[WIFI] PMF pre-disabled for '%s' (cached)\n", net.ssid.c_str());
        apply_pmf_override(true);
        pending_pmf_disable = true;
        esp_wifi_disconnect();
        delay(50);
        esp_wifi_connect();
    }
}

static void process_scan_results() {
    auto &cfg = Config::get();
    int16_t n = WiFi.scanComplete();
    if (n < 0) return;

    // match visible APs against configured networks, sort by RSSI
    struct { uint8_t net_idx; int32_t rssi; } candidates[WIFI_MAX_NETWORKS];
    uint8_t nc = 0;

    for (int i = 0; i < n && nc < WIFI_MAX_NETWORKS; i++) {
        uint8_t idx = find_net_by_ssid(WiFi.SSID(i).c_str());
        if (idx == 0xFF) continue;
        // check if we already have a better entry for this network
        bool dup = false;
        for (uint8_t j = 0; j < nc; j++) {
            if (candidates[j].net_idx == idx) {
                if (WiFi.RSSI(i) > candidates[j].rssi)
                    candidates[j].rssi = WiFi.RSSI(i);
                dup = true;
                break;
            }
        }
        if (!dup) {
            candidates[nc].net_idx = idx;
            candidates[nc].rssi = WiFi.RSSI(i);
            nc++;
        }
    }

    // cache last-seen RSSI
    memset(last_seen_rssi, 0, sizeof(last_seen_rssi));
    for (uint8_t i = 0; i < nc; i++)
        last_seen_rssi[candidates[i].net_idx] = (int8_t)candidates[i].rssi;

    // sort by RSSI
    for (uint8_t i = 0; i < nc; i++) {
        for (uint8_t j = i + 1; j < nc; j++) {
            if (candidates[j].rssi > candidates[i].rssi) {
                auto tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    try_count = nc;
    for (uint8_t i = 0; i < nc; i++) try_order[i] = candidates[i].net_idx;
    try_pos = 0;

    WiFi.scanDelete();

    if (nc > 0) {
        Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Scan: %d known of %d visible, best='%s' (%d dBm)\n",
                  nc, n, cfg.wifi_nets[try_order[0]].ssid.c_str(), (int)candidates[0].rssi);
    } else {
        Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Scan: 0 known of %d visible\n", n);
    }

    WebUI::push_event("wifi", "{\"scan_done\":true}");
}

static void on_connected() {
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Connected to '%s' (%s)\n",
              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

    connect_idx = find_net_by_ssid(WiFi.SSID().c_str());

    // Persist the BSSID+channel we just connected on so the next reboot can
    // fast-path. If we got here via a PMF retry, mark that flag in the hint
    // so subsequent reconnects skip the doomed first attempt.
    uint8_t *bssid = WiFi.BSSID();
    if (bssid) {
        NetworkHints::upsert(WiFi.SSID().c_str(), bssid, WiFi.channel(),
                             pending_pmf_disable);
    }
    pending_pmf_disable = false;

    set_state(WF_CONNECTED);
    low_rssi_count = 0;
    last_roam_check = millis();
    last_bg_scan = millis();
    WebUI::push_event("wifi", "{\"connected\":true}");

    if (!ntp_done) {
        sync_ntp();
        ntp_done = true;
    }
}

static void enter_ap_fallback() {
    auto &cfg = Config::get();
    WiFi.mode(WIFI_AP_STA);
    String ap = ap_ssid_str();
    WiFi.softAP(ap.c_str(), "airbridge");
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] AP+STA fallback: %s (%s)\n",
              ap.c_str(), WiFi.softAPIP().toString().c_str());
    set_state(WF_AP_FALLBACK);
    last_ap_retry = millis();
}


static bool try_smartconfig() {
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] SmartConfig waiting...\n");
    WiFi.mode(WIFI_STA);
    WiFi.beginSmartConfig();
    set_state(WF_SMARTCONFIG);
    return true;  // non-blocking, check() handles the rest
}


bool WiFiSetup::init() {
    auto &cfg = Config::get();

    WiFi.onEvent(wifi_event_cb);

    if (cfg.wifi_mode == 2) {
        WiFi.mode(WIFI_OFF);
        set_state(WF_OFF);
        return false;
    }

    WiFi.setHostname(cfg.hostname.c_str());

    if (cfg.wifi_mode == 0 && cfg.wifi_net_count > 0) {
        WiFi.mode(WIFI_STA);
        begin_connect(0, true);
        return true;
    }

    if (cfg.wifi_mode == 0 && cfg.wifi_net_count == 0) {
        try_smartconfig();
        return true;
    }

    WiFi.mode(WIFI_AP);
    String ap = ap_ssid_str();
    WiFi.softAP(ap.c_str(), "airbridge");
    delay(100);
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] AP mode: %s %s\n",
              ap.c_str(), WiFi.softAPIP().toString().c_str());
    set_state(WF_OFF);
    return true;
}

void WiFiSetup::check() {
    auto &cfg = Config::get();
    uint32_t elapsed = millis() - state_entered_ms;

    if (got_ip) {
        got_ip = false;
        sta_disconnected = false;
        if (wf_state == WF_HINT_TRY || wf_state == WF_CONNECTING ||
            wf_state == WF_PMF_RETRY) {
            on_connected();
        } else if (wf_state == WF_AP_FALLBACK) {
            on_connected();
            // switch from AP+STA to STA only
            WiFi.mode(WIFI_STA);
        }
        return;
    }

    if (sta_disconnected && wf_state == WF_CONNECTED) {
        sta_disconnected = false;
        Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Disconnected, scanning...\n");
        WiFi.scanNetworks(true);  // async
        set_state(WF_SCANNING);
        return;
    }
    sta_disconnected = false;

    switch (wf_state) {
    case WF_OFF:
        break;

    case WF_HINT_TRY:
        if (elapsed > HINT_TIMEOUT_MS) {
            Log::logf(CAT_WIFI, LOG_DEBUG, "[WIFI] Hint timeout, full scan\n");
            WiFi.disconnect();
            WiFi.scanNetworks(true);
            set_state(WF_SCANNING);
        }
        break;

    case WF_SCANNING: {
        int16_t result = WiFi.scanComplete();
        if (result >= 0) {
            process_scan_results();
            if (try_count > 0) {
                begin_connect(try_order[0], false);
            } else if (cfg.wifi_net_count > 0) {
                // No known APs visible - AP fallback
                enter_ap_fallback();
            } else {
                try_smartconfig();
            }
        } else if (result == WIFI_SCAN_FAILED) {
            Log::logf(CAT_WIFI, LOG_WARN, "[WIFI] Scan failed\n");
            if (cfg.wifi_net_count > 0) enter_ap_fallback();
            else try_smartconfig();
        }
        break;
    }

    case WF_CONNECTING:
        if (elapsed > CONNECT_TIMEOUT_MS) {
            if (last_disconnect_reason == 208) {
                last_disconnect_reason = 0;
                enter_pmf_retry();
                break;
            }
            connect_retries++;
            if (connect_retries < CONNECT_RETRIES) {
                Log::logf(CAT_WIFI, LOG_DEBUG, "[WIFI] Connect timeout, retry %d\n", connect_retries);
                begin_connect(connect_idx, false);
            } else {
                try_pos++;
                if (try_pos < try_count) {
                    Log::logf(CAT_WIFI, LOG_DEBUG, "[WIFI] Trying next network\n");
                    begin_connect(try_order[try_pos], false);
                } else {
                    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] All networks exhausted\n");
                    enter_ap_fallback();
                }
            }
        }
        break;

    case WF_PMF_RETRY:
        if (elapsed > CONNECT_TIMEOUT_MS) {
            Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] PMF retry timed out, advancing\n");
            try_pos++;
            if (try_pos < try_count) {
                begin_connect(try_order[try_pos], false);
            } else {
                enter_ap_fallback();
            }
        }
        break;

    case WF_CONNECTED:
        // periodic RSSI check for roaming
        if (cfg.wifi_roam && !roaming_suspended &&
            millis() - last_roam_check >= ROAM_CHECK_INTERVAL_MS) {
            last_roam_check = millis();
            int8_t rssi = WiFi.RSSI();
            if (rssi < ROAM_RSSI_THRESHOLD) {
                low_rssi_count++;
                Log::logf(CAT_WIFI, LOG_DEBUG, "[WIFI] Low RSSI %d dBm (%d/%d)\n",
                          rssi, low_rssi_count, ROAM_CONSECUTIVE_LOW);
                if (low_rssi_count >= ROAM_CONSECUTIVE_LOW) {
                    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Roaming: scanning for better AP\n");
                    WiFi.scanNetworks(true);
                    set_state(WF_ROAM_SCAN);
                }
            } else {
                low_rssi_count = 0;
            }
        }
/*
        // periodic background scan to update RSSI for all networks
        if (!roaming_suspended && millis() - last_bg_scan >= BG_SCAN_INTERVAL_MS) {
            last_bg_scan = millis();
            WiFi.scanNetworks(true);
        }
*/
        // Check if a background scan completed
        if (WiFi.scanComplete() >= 0) {
            process_scan_results();
        }
        // Refresh the hint on any (re)association we caught via STA_GOT_IP,
        // including supplicant-driven reconnects we didn't initiate.
        if (hint_refresh_pending) {
            hint_refresh_pending = false;
            uint8_t *bssid = WiFi.BSSID();
            if (bssid) {
                NetworkHints::upsert(WiFi.SSID().c_str(), bssid,
                                     WiFi.channel(), false);
            }
        }
        break;

    case WF_ROAM_SCAN: {
        int16_t result = WiFi.scanComplete();
        if (result >= 0) {
            process_scan_results();
            // is best candidate is significantly better?
            bool should_switch = false;
            uint8_t best_idx = 0xFF;
            if (try_count > 0 && try_order[0] != connect_idx) {
                int8_t current_rssi = WiFi.RSSI();
                int8_t candidate_rssi = last_seen_rssi[try_order[0]];
                if (candidate_rssi > current_rssi + ROAM_HYSTERESIS_DB) {
                    should_switch = true;
                    best_idx = try_order[0];
                    Log::logf(CAT_WIFI, LOG_INFO,
                              "[WIFI] Candidate '%s' %d dBm beats current %d dBm by >=%d\n",
                              cfg.wifi_nets[best_idx].ssid.c_str(),
                              candidate_rssi, current_rssi, ROAM_HYSTERESIS_DB);
                } else {
                    Log::logf(CAT_WIFI, LOG_DEBUG,
                              "[WIFI] Candidate '%s' %d dBm vs current %d dBm (<%d hysteresis), staying\n",
                              cfg.wifi_nets[try_order[0]].ssid.c_str(),
                              candidate_rssi, current_rssi, ROAM_HYSTERESIS_DB);
                }
            }
            if (should_switch && best_idx < cfg.wifi_net_count) {
                Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Roaming to '%s'\n",
                          cfg.wifi_nets[best_idx].ssid.c_str());
                WiFi.disconnect();
                delay(100);
                begin_connect(best_idx, true);
            } else {
                set_state(WF_CONNECTED);
                low_rssi_count = 0;
            }
        } else if (result == WIFI_SCAN_FAILED) {
            set_state(WF_CONNECTED);
            low_rssi_count = 0;
        }
        break;
    }

    case WF_AP_FALLBACK:
        if (millis() - last_ap_retry >= AP_RETRY_INTERVAL_MS) {
            last_ap_retry = millis();
            Log::logf(CAT_WIFI, LOG_DEBUG, "[WIFI] AP fallback: retrying scan\n");
            WiFi.scanNetworks(true);
            set_state(WF_SCANNING);
        }
        break;

    case WF_SMARTCONFIG:
        if (WiFi.smartConfigDone()) {
            WiFi.stopSmartConfig();
            Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] SmartConfig: got '%s'\n", WiFi.SSID().c_str());
            // add to list, replace oldest if full
            if (!Config::add_network(WiFi.SSID().c_str(), WiFi.psk().c_str())) {
                // full, shift all down
                Config::remove_network(0);
                Config::add_network(WiFi.SSID().c_str(), WiFi.psk().c_str());
                Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] SmartConfig: replaced oldest network\n");
            }
            cfg.wifi_mode = 0;
            Config::save();

            uint8_t idx = find_net_by_ssid(WiFi.SSID().c_str());
            if (idx != 0xFF) begin_connect(idx, false);
            else set_state(WF_CONNECTING);  // already began in SmartConfig
        } else if (elapsed > SMARTCONFIG_TIMEOUT_MS) {
            WiFi.stopSmartConfig();
            Log::logf(CAT_WIFI, LOG_WARN, "[WIFI] SmartConfig timeout\n");
            enter_ap_fallback();
        }
        break;
    }
}

bool WiFiSetup::is_connected() {
    return wf_state == WF_CONNECTED && WiFi.status() == WL_CONNECTED;
}

bool WiFiSetup::time_synced() {
    return ntp_synced;
}

void WiFiSetup::set_fallback_time(int year, int month, int day, int hour, int min, int sec, bool force) {
    if (ntp_synced && !force) return;

    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;

    time_t epoch = mktime(&t);
    if (epoch < 0) return;

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    auto &cfg = Config::get();
    if (cfg.tz.length() > 0) {
        setenv("TZ", cfg.tz.c_str(), 1);
        tzset();
    }

    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Fallback time from ResMed: %04d-%02d-%02d %02d:%02d\n",
              year, month, day, hour, min);
}

void WiFiSetup::force_ntp_sync() {
    Log::logf(CAT_WIFI, LOG_INFO, "[WIFI] Forcing NTP resync\n");
    sync_ntp();
}

void WiFiSetup::suspend_roaming() { roaming_suspended = true; }
void WiFiSetup::resume_roaming()  { roaming_suspended = false; }

const char *WiFiSetup::state_name() {
    switch (wf_state) {
        case WF_OFF:          return "off";
        case WF_HINT_TRY:     return "hint";
        case WF_SCANNING:     return "scanning";
        case WF_CONNECTING:   return "connecting";
        case WF_PMF_RETRY:    return "pmf_retry";
        case WF_CONNECTED:    return "connected";
        case WF_ROAM_SCAN:    return "roaming";
        case WF_AP_FALLBACK:  return "ap_fallback";
        case WF_SMARTCONFIG:  return "smartconfig";
        default:              return "?";
    }
}

int8_t WiFiSetup::current_rssi() {
    if (wf_state == WF_CONNECTED || wf_state == WF_ROAM_SCAN)
        return WiFi.RSSI();
    return 0;
}

const char *WiFiSetup::connected_ssid() {
    static char buf[33] = {};
    if (wf_state == WF_CONNECTED || wf_state == WF_ROAM_SCAN) {
        strncpy(buf, WiFi.SSID().c_str(), 32);
        return buf;
    }
    return "";
}

uint8_t WiFiSetup::connected_net_idx() {
    return (wf_state == WF_CONNECTED || wf_state == WF_ROAM_SCAN) ? connect_idx : 0xFF;
}

int8_t WiFiSetup::net_rssi(uint8_t idx) {
    if (idx >= WIFI_MAX_NETWORKS) return 0;
    // For the connected network, return live RSSI
    if (idx == connect_idx && (wf_state == WF_CONNECTED || wf_state == WF_ROAM_SCAN))
        return WiFi.RSSI();
    return last_seen_rssi[idx];
}
