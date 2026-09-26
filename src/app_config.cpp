#include "app_config.h"
#include "uart_arbiter.h"
#include "qframe.h"
#include "network_hints.h"
#include "debug_log.h"
#include <Preferences.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#define CFG_DEFAULT_TCP_PORT    23
#define CFG_DEFAULT_BAUD        57600

static Preferences prefs;
static AirBridgeConfig cfg;

struct KVEntry {
    const char *key;
    const char *nvs_key;
    enum { STR, U8, U16, U32, BOOL } type;
    void *ptr;
    union Default {
        const char *text;
        uint32_t number;
        constexpr Default(const char *value) : text(value) {}
        constexpr Default(uint32_t value) : number(value) {}
    } initial;
    bool sensitive;
};

#define KV_STR(k, n, f, d) {k, n, KVEntry::STR, &cfg.f, {d}, false}
#define KV_SECRET(k, n, f, d) {k, n, KVEntry::STR, &cfg.f, {d}, true}
#define KV_U8(k, n, f, d) {k, n, KVEntry::U8, &cfg.f, {uint32_t(d)}, false}
#define KV_U16(k, n, f, d) {k, n, KVEntry::U16, &cfg.f, {uint32_t(d)}, false}
#define KV_U32(k, n, f, d) {k, n, KVEntry::U32, &cfg.f, {uint32_t(d)}, false}
#define KV_BOOL(k, n, f, d) {k, n, KVEntry::BOOL, &cfg.f, {uint32_t(d)}, false}

static const KVEntry kv_table[] = {
    KV_STR("hostname", "hostname", hostname, DEFAULT_HOSTNAME),
    KV_U8("wifi_mode", "wifi_mode", wifi_mode, WIFI_MODE_AP_ONLY),
    KV_BOOL("wifi_roam", "wifi_roam", wifi_roam, true),
    KV_STR("wifi_country", "wifi_country", wifi_country, "01"),
    KV_U16("tcp_port", "tcp_port", tcp_port, CFG_DEFAULT_TCP_PORT),
    KV_BOOL("oxi_enabled", "oxi_enabled", oxi_enabled, true),
    KV_BOOL("oxi_auto_start", "oxi_autostart", oxi_auto_start, true),
    KV_BOOL("oxi_feed_therapy_only", "oxi_thronly", oxi_feed_therapy_only, false),
    KV_U8("oxi_device_type", "oxi_devtype", oxi_device_type, 0),
    KV_STR("oxi_device_addr", "oxi_devaddr", oxi_device_addr, ""),
    KV_U16("oxi_interval_ms", "oxi_interval", oxi_interval_ms, 500),
    KV_BOOL("oxi_lframe_continuous", "oxi_lframe_cont", oxi_lframe_continuous, true),
    KV_BOOL("oxi_require_known", "oxi_req_known", oxi_require_known, false),
    KV_U32("uart_baud", "uart_baud", uart_baud, CFG_DEFAULT_BAUD),
    KV_U16("uart_cmd_timeout_ms", "uart_timeout", uart_cmd_timeout_ms, 500),
    KV_U8("uart_max_retries", "uart_retries", uart_max_retries, 3),
    KV_BOOL("allow_transparent_during_therapy", "allow_transp", allow_transparent_during_therapy, false),
    KV_U16("debug_port", "debug_port", debug_port, 8023),
    KV_BOOL("syslog_en", "syslog_en", syslog_enabled, false),
    KV_STR("syslog_host", "syslog_host", syslog_host, ""),
    KV_U16("syslog_port", "syslog_port", syslog_port, 514),
    KV_U16("http_port", "http_port", http_port, 80),
    KV_STR("http_user", "http_user", http_user, "admin"),
    KV_SECRET("http_pass", "http_pass", http_pass, "airbridge"),
    KV_SECRET("ota_password", "ota_pass", ota_password, "airbridge"),
    KV_STR("update_url", "update_url", update_url, AB_DEFAULT_UPDATE_URL),
    KV_STR("ntp_server", "ntp_server", ntp_server, ""),
    KV_STR("tz", "tz", tz, "UTC0"),
    KV_U16("udp_oxi_port", "udp_oxi_port", udp_oxi_port, 8025),
    KV_U8("mitm_mode", "mitm_mode", mitm_mode, 0),
    KV_BOOL("smb_enabled", "smb_enable", smb_enabled, false),
    KV_BOOL("smb_auto_after_therapy", "smb_auto", smb_auto_after_therapy, true),
    KV_STR("smb_endpoint", "smb_ep", smb_endpoint, ""),
    KV_STR("smb_user", "smb_user", smb_user, ""),
    KV_SECRET("smb_password", "smb_pass", smb_password, ""),
    KV_BOOL("sleephq_enabled", "shq_enable", sleephq_enabled, false),
    KV_BOOL("sleephq_auto_after_therapy", "shq_auto", sleephq_auto_after_therapy, true),
    KV_STR("sleephq_client_id", "shq_id", sleephq_client_id, ""),
    KV_SECRET("sleephq_client_secret", "shq_secret", sleephq_client_secret, ""),
    KV_STR("sleephq_team_id", "shq_team", sleephq_team_id, ""),
    KV_STR("sleephq_device_id", "shq_device", sleephq_device_id, ""),
};


static void apply_defaults() {
    for (const KVEntry &entry : kv_table) {
        switch (entry.type) {
            case KVEntry::STR:  *(String *)entry.ptr = entry.initial.text; break;
            case KVEntry::U8:   *(uint8_t *)entry.ptr = entry.initial.number; break;
            case KVEntry::U16:  *(uint16_t *)entry.ptr = entry.initial.number; break;
            case KVEntry::U32:  *(uint32_t *)entry.ptr = entry.initial.number; break;
            case KVEntry::BOOL: *(bool *)entry.ptr = entry.initial.number; break;
        }
    }
    cfg.wifi_net_count = 0;
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        cfg.wifi_nets[i].ssid = "";
        cfg.wifi_nets[i].pass = "";
        cfg.wifi_nets[i].enabled = false;
    }
}

void Config::init() {
    apply_defaults();
    prefs.begin("airbridge", false);
}

static void load_wifi_nets() {
    Preferences wp;
    // writable: we may need to scrub legacy bssid_<i>/chan_<i> keys after
    // migrating them to NetworkHints.
    wp.begin("wnet", false);
    cfg.wifi_net_count = wp.getUChar("count", 0);
    if (cfg.wifi_net_count > WIFI_MAX_NETWORKS) cfg.wifi_net_count = WIFI_MAX_NETWORKS;
    for (int i = 0; i < cfg.wifi_net_count; i++) {
        char key[14];
        snprintf(key, sizeof(key), "ssid_%d", i);
        cfg.wifi_nets[i].ssid = wp.getString(key, "");
        snprintf(key, sizeof(key), "pass_%d", i);
        cfg.wifi_nets[i].pass = wp.getString(key, "");
        snprintf(key, sizeof(key), "ena_%d", i);
        cfg.wifi_nets[i].enabled = wp.getBool(key, true);

        // Legacy hint keys: bssid_<i> + chan_<i> used to live here. Migrate
        // any populated values into NetworkHints, then nuke the old keys.
        // Preferences::remove is a no-op on missing keys.
        uint8_t bssid[6] = {0};
        snprintf(key, sizeof(key), "bssid_%d", i);
        wp.getBytes(key, bssid, 6);
        wp.remove(key);
        snprintf(key, sizeof(key), "chan_%d", i);
        uint8_t channel = wp.getUChar(key, 0);
        wp.remove(key);
        const uint8_t zero[6] = {0};
        if (channel > 0 && memcmp(bssid, zero, 6) != 0 &&
            cfg.wifi_nets[i].ssid.length() > 0) {
            NetworkHints::upsert(cfg.wifi_nets[i].ssid.c_str(),
                                 bssid, channel, false);
        }
    }
    wp.end();

    // migrate from old single wifi_ssid/wifi_pass
    if (cfg.wifi_net_count == 0) {
        String old_ssid = prefs.getString("wifi_ssid", "");
        if (old_ssid.length() > 0) {
            cfg.wifi_nets[0].ssid = old_ssid;
            cfg.wifi_nets[0].pass = prefs.getString("wifi_pass", "");
            cfg.wifi_nets[0].enabled = true;
            cfg.wifi_net_count = 1;
            Config::save_wifi_nets();
            prefs.remove("wifi_ssid");
            prefs.remove("wifi_pass");
        }
    }
}

static void apply_syslog() {
    if (!Log::configure_syslog(cfg.syslog_enabled, cfg.syslog_host.c_str(),
                               cfg.syslog_port, cfg.hostname.c_str())) {
        Log::logf(CAT_GENERAL, LOG_WARN, "[LOG] Cannot enable syslog\n");
    }
}

void Config::load() {
    for (const KVEntry &entry : kv_table) {
        if (!prefs.isKey(entry.nvs_key)) continue;
        switch (entry.type) {
            case KVEntry::STR: {
                auto &value = *(String *)entry.ptr;
                value = prefs.getString(entry.nvs_key, value);
                break;
            }
            case KVEntry::U8: {
                auto &value = *(uint8_t *)entry.ptr;
                value = prefs.getUChar(entry.nvs_key, value);
                break;
            }
            case KVEntry::U16: {
                auto &value = *(uint16_t *)entry.ptr;
                value = prefs.getUShort(entry.nvs_key, value);
                break;
            }
            case KVEntry::U32: {
                auto &value = *(uint32_t *)entry.ptr;
                value = prefs.getULong(entry.nvs_key, value);
                break;
            }
            case KVEntry::BOOL: {
                auto &value = *(bool *)entry.ptr;
                value = prefs.getBool(entry.nvs_key, value);
                break;
            }
        }
    }
    load_wifi_nets();
    apply_syslog();
}

void Config::save() {
    for (const KVEntry &entry : kv_table) {
        switch (entry.type) {
            case KVEntry::STR: {
                const auto &value = *(String *)entry.ptr;
                prefs.putString(entry.nvs_key, value);
                break;
            }
            case KVEntry::U8: {
                const auto &value = *(uint8_t *)entry.ptr;
                prefs.putUChar(entry.nvs_key, value);
                break;
            }
            case KVEntry::U16: {
                const auto &value = *(uint16_t *)entry.ptr;
                prefs.putUShort(entry.nvs_key, value);
                break;
            }
            case KVEntry::U32: {
                const auto &value = *(uint32_t *)entry.ptr;
                prefs.putULong(entry.nvs_key, value);
                break;
            }
            case KVEntry::BOOL: {
                const auto &value = *(bool *)entry.ptr;
                prefs.putBool(entry.nvs_key, value);
                break;
            }
        }
    }
    apply_syslog();
}

void Config::reset_defaults() {
    prefs.clear();
    apply_defaults();
    save();
}

AirBridgeConfig& Config::get() {
    return cfg;
}


void Config::refresh_device_info() {
    char value[64];
    if (cfg.device_pna.isEmpty() &&
        Arbiter::get_var("PNA", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, value, sizeof(value)))
        cfg.device_pna = value;

    if (cfg.device_srn.isEmpty() &&
        Arbiter::get_var("SRN", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, value, sizeof(value)))
        cfg.device_srn = value;
}

void Config::invalidate_device_info() {
    cfg.device_pna = "";
    cfg.device_srn = "";
}


static void format_value(const KVEntry &entry, String &out) {
    switch (entry.type) {
        case KVEntry::STR:  out = *(String *)entry.ptr; break;
        case KVEntry::U8:   out = String(*(uint8_t *)entry.ptr); break;
        case KVEntry::U16:  out = String(*(uint16_t *)entry.ptr); break;
        case KVEntry::U32:  out = String(*(uint32_t *)entry.ptr); break;
        case KVEntry::BOOL: out = *(bool *)entry.ptr ? "1" : "0"; break;
    }
}

bool Config::get_value(const char *key, String &out) {
    for (const KVEntry &entry : kv_table) {
        if (strcasecmp(key, entry.key) == 0) {
            format_value(entry, out);
            return true;
        }
    }
    return false;
}

bool Config::set_value(const char *key, const char *value) {
    if (strcasecmp(key, "syslog_host") == 0 && *value) {
        in_addr address;
        if (inet_pton(AF_INET, value, &address) != 1) return false;
    } else if (strcasecmp(key, "syslog_port") == 0) {
        char *end;
        unsigned long port = strtoul(value, &end, 10);
        if (*value < '0' || *value > '9' || *end || port == 0 || port > 65535)
            return false;
    } else if (strcasecmp(key, "syslog_en") == 0) {
        if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) return false;
    }

    for (const KVEntry &entry : kv_table) {
        if (strcasecmp(key, entry.key) == 0) {
            switch (entry.type) {
            case KVEntry::STR:  *(String*)entry.ptr = value; break;
            case KVEntry::U8:   *(uint8_t*)entry.ptr = atoi(value); break;
            case KVEntry::U16:  *(uint16_t*)entry.ptr = atoi(value); break;
            case KVEntry::U32:  *(uint32_t*)entry.ptr = atol(value); break;
            case KVEntry::BOOL: *(bool*)entry.ptr = (atoi(value) != 0); break;
            }
            return true;
        }
    }
    return false;
}

void Config::foreach_kv(kv_visitor_fn fn, void *ctx) {
    String val;
    for (const KVEntry &entry : kv_table) {
        format_value(entry, val);
        fn(entry.key, val, entry.sensitive, ctx);
    }
}

String Config::dump() {
    String out;
    // wifi networks
    for (int i = 0; i < cfg.wifi_net_count; i++) {
        out += "wifi_net_" + String(i) + "=" + cfg.wifi_nets[i].ssid;
        if (!cfg.wifi_nets[i].enabled) out += " [disabled]";
        out += "\n";
    }
    // KV table entries
    foreach_kv([](const char *key, const String &val, bool sensitive, void *p) {
        String v = val;
        if (sensitive && v.length() > 0) v = "****";
        *(String*)p += String(key) + "=" + v + "\n";
    }, &out);
    return out;
}


void Config::save_wifi_nets() {
    Preferences wp;
    wp.begin("wnet", false);
    wp.putUChar("count", cfg.wifi_net_count);
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        char key[14];
        if (i < cfg.wifi_net_count) {
            snprintf(key, sizeof(key), "ssid_%d", i);
            wp.putString(key, cfg.wifi_nets[i].ssid);
            snprintf(key, sizeof(key), "pass_%d", i);
            wp.putString(key, cfg.wifi_nets[i].pass);
            snprintf(key, sizeof(key), "ena_%d", i);
            wp.putBool(key, cfg.wifi_nets[i].enabled);
        } else {
            snprintf(key, sizeof(key), "ssid_%d", i);
            if (wp.isKey(key)) wp.remove(key);
            snprintf(key, sizeof(key), "pass_%d", i);
            if (wp.isKey(key)) wp.remove(key);
            snprintf(key, sizeof(key), "ena_%d", i);
            if (wp.isKey(key)) wp.remove(key);
        }
    }
    wp.end();
}

bool Config::add_network(const char *ssid, const char *pass) {
    if (cfg.wifi_net_count >= WIFI_MAX_NETWORKS) return false;
    int idx = cfg.wifi_net_count;
    cfg.wifi_nets[idx].ssid = ssid;
    cfg.wifi_nets[idx].pass = pass ? pass : "";
    cfg.wifi_nets[idx].enabled = true;
    cfg.wifi_net_count++;
    save_wifi_nets();
    return true;
}

bool Config::remove_network(uint8_t idx) {
    if (idx >= cfg.wifi_net_count) return false;
    // drop any cached hints for this SSID before the slot goes away
    NetworkHints::clear_for(cfg.wifi_nets[idx].ssid.c_str());
    // shift remaining entries down
    for (int i = idx; i < cfg.wifi_net_count - 1; i++)
        cfg.wifi_nets[i] = cfg.wifi_nets[i + 1];
    cfg.wifi_net_count--;
    // clear the vacated slot
    cfg.wifi_nets[cfg.wifi_net_count].ssid = "";
    cfg.wifi_nets[cfg.wifi_net_count].pass = "";
    cfg.wifi_nets[cfg.wifi_net_count].enabled = false;
    save_wifi_nets();
    return true;
}
