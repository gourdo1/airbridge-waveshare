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

static void apply_defaults() {
    cfg.hostname = DEFAULT_HOSTNAME;
    cfg.wifi_net_count = 0;
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        cfg.wifi_nets[i].ssid = "";
        cfg.wifi_nets[i].pass = "";
        cfg.wifi_nets[i].enabled = false;
    }
    cfg.wifi_mode = WIFI_MODE_AP_ONLY;   // virgin device: AP up, user provisions
    cfg.wifi_roam = true;
    cfg.wifi_country = "01";             // worldwide / ESP-IDF default
    cfg.tcp_port = CFG_DEFAULT_TCP_PORT;

    cfg.oxi_enabled = true;
    cfg.oxi_auto_start = true;
    cfg.oxi_feed_therapy_only = false;
    cfg.oxi_device_type = 0;
    cfg.oxi_device_addr = "";
    cfg.oxi_interval_ms = 500;
    cfg.oxi_lframe_continuous = true;
    cfg.oxi_require_known = false;

    cfg.uart_baud = CFG_DEFAULT_BAUD;
    cfg.uart_cmd_timeout_ms = 500;
    cfg.uart_max_retries = 3;

    cfg.allow_transparent_during_therapy = false;

    cfg.debug_port = 8023;
    cfg.syslog_enabled = false;
    cfg.syslog_host = "";
    cfg.syslog_port = 514;

    cfg.http_port = 80;
    cfg.http_user = "admin";
    cfg.http_pass = "airbridge";

    cfg.ota_password = "airbridge";
    cfg.update_url = AB_DEFAULT_UPDATE_URL;

    cfg.ntp_server = "";
    cfg.tz = "UTC0";
    cfg.udp_oxi_port = 8025;

    cfg.mitm_mode = 0;

    cfg.smb_enabled = false;
    cfg.smb_auto_after_therapy = true;
    cfg.smb_endpoint = "";
    cfg.smb_user = "";
    cfg.smb_password = "";

    cfg.sleephq_enabled = false;
    cfg.sleephq_auto_after_therapy = true;
    cfg.sleephq_client_id = "";
    cfg.sleephq_client_secret = "";
    cfg.sleephq_team_id = "";
    cfg.sleephq_device_id = "";
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
    cfg.hostname        = prefs.getString("hostname", cfg.hostname);
    cfg.wifi_mode       = prefs.getUChar("wifi_mode", cfg.wifi_mode);
    cfg.wifi_roam       = prefs.getBool("wifi_roam", cfg.wifi_roam);
    cfg.wifi_country    = prefs.getString("wifi_country", cfg.wifi_country);
    cfg.tcp_port        = prefs.getUShort("tcp_port", cfg.tcp_port);
    load_wifi_nets();

    cfg.oxi_enabled     = prefs.getBool("oxi_enabled", cfg.oxi_enabled);
    cfg.oxi_auto_start  = prefs.getBool("oxi_autostart", cfg.oxi_auto_start);
    cfg.oxi_feed_therapy_only = prefs.getBool("oxi_thronly", cfg.oxi_feed_therapy_only);
    cfg.oxi_device_type = prefs.getUChar("oxi_devtype", cfg.oxi_device_type);
    cfg.oxi_device_addr = prefs.getString("oxi_devaddr", cfg.oxi_device_addr);
    cfg.oxi_interval_ms = prefs.getUShort("oxi_interval", cfg.oxi_interval_ms);
    cfg.oxi_lframe_continuous = prefs.getBool("oxi_lframe_cont", cfg.oxi_lframe_continuous);
    cfg.oxi_require_known = prefs.getBool("oxi_req_known", cfg.oxi_require_known);

    cfg.uart_baud       = prefs.getULong("uart_baud", cfg.uart_baud);
    cfg.uart_cmd_timeout_ms = prefs.getUShort("uart_timeout", cfg.uart_cmd_timeout_ms);
    cfg.uart_max_retries = prefs.getUChar("uart_retries", cfg.uart_max_retries);

    cfg.allow_transparent_during_therapy = prefs.getBool("allow_transp", cfg.allow_transparent_during_therapy);

    cfg.debug_port      = prefs.getUShort("debug_port", cfg.debug_port);
    cfg.syslog_enabled  = prefs.getBool("syslog_en", cfg.syslog_enabled);
    cfg.syslog_host     = prefs.getString("syslog_host", cfg.syslog_host);
    cfg.syslog_port     = prefs.getUShort("syslog_port", cfg.syslog_port);

    cfg.http_port       = prefs.getUShort("http_port", cfg.http_port);
    cfg.http_user       = prefs.getString("http_user", cfg.http_user);
    cfg.http_pass       = prefs.getString("http_pass", cfg.http_pass);

    cfg.ota_password    = prefs.getString("ota_pass", cfg.ota_password);
    cfg.update_url      = prefs.getString("update_url", cfg.update_url);
    cfg.ntp_server      = prefs.getString("ntp_server", cfg.ntp_server);
    cfg.tz              = prefs.getString("tz", cfg.tz);
    cfg.udp_oxi_port    = prefs.getUShort("udp_oxi_port", cfg.udp_oxi_port);
    cfg.mitm_mode       = prefs.getUChar("mitm_mode", cfg.mitm_mode);

    cfg.smb_enabled     = prefs.getBool("smb_enable", cfg.smb_enabled);
    cfg.smb_auto_after_therapy = prefs.getBool("smb_auto", cfg.smb_auto_after_therapy);
    cfg.smb_endpoint    = prefs.getString("smb_ep", cfg.smb_endpoint);
    cfg.smb_user        = prefs.getString("smb_user", cfg.smb_user);
    cfg.smb_password    = prefs.getString("smb_pass", cfg.smb_password);

    cfg.sleephq_enabled = prefs.getBool("shq_enable", cfg.sleephq_enabled);
    cfg.sleephq_auto_after_therapy = prefs.getBool("shq_auto", cfg.sleephq_auto_after_therapy);
    cfg.sleephq_client_id = prefs.getString("shq_id", cfg.sleephq_client_id);
    cfg.sleephq_client_secret = prefs.getString("shq_secret", cfg.sleephq_client_secret);
    cfg.sleephq_team_id = prefs.getString("shq_team", cfg.sleephq_team_id);
    cfg.sleephq_device_id = prefs.getString("shq_device", cfg.sleephq_device_id);
    apply_syslog();
}

void Config::save() {
    prefs.putString("hostname", cfg.hostname);
    prefs.putUChar("wifi_mode", cfg.wifi_mode);
    prefs.putBool("wifi_roam", cfg.wifi_roam);
    prefs.putString("wifi_country", cfg.wifi_country);
    prefs.putUShort("tcp_port", cfg.tcp_port);
    // wifi_nets saved separately via save_wifi_nets()

    prefs.putBool("oxi_enabled", cfg.oxi_enabled);
    prefs.putBool("oxi_autostart", cfg.oxi_auto_start);
    prefs.putBool("oxi_thronly", cfg.oxi_feed_therapy_only);
    prefs.putUChar("oxi_devtype", cfg.oxi_device_type);
    prefs.putString("oxi_devaddr", cfg.oxi_device_addr);
    prefs.putUShort("oxi_interval", cfg.oxi_interval_ms);
    prefs.putBool("oxi_lframe_cont", cfg.oxi_lframe_continuous);
    prefs.putBool("oxi_req_known", cfg.oxi_require_known);

    prefs.putULong("uart_baud", cfg.uart_baud);
    prefs.putUShort("uart_timeout", cfg.uart_cmd_timeout_ms);
    prefs.putUChar("uart_retries", cfg.uart_max_retries);

    prefs.putBool("allow_transp", cfg.allow_transparent_during_therapy);

    prefs.putUShort("debug_port", cfg.debug_port);
    prefs.putBool("syslog_en", cfg.syslog_enabled);
    prefs.putString("syslog_host", cfg.syslog_host);
    prefs.putUShort("syslog_port", cfg.syslog_port);

    prefs.putUShort("http_port", cfg.http_port);
    prefs.putString("http_user", cfg.http_user);
    prefs.putString("http_pass", cfg.http_pass);

    prefs.putString("ota_pass", cfg.ota_password);
    prefs.putString("update_url", cfg.update_url);
    prefs.putString("ntp_server", cfg.ntp_server);
    prefs.putString("tz", cfg.tz);
    prefs.putUShort("udp_oxi_port", cfg.udp_oxi_port);
    prefs.putUChar("mitm_mode", cfg.mitm_mode);

    prefs.putBool("smb_enable", cfg.smb_enabled);
    prefs.putBool("smb_auto", cfg.smb_auto_after_therapy);
    prefs.putString("smb_ep", cfg.smb_endpoint);
    prefs.putString("smb_user", cfg.smb_user);
    prefs.putString("smb_pass", cfg.smb_password);

    prefs.putBool("shq_enable", cfg.sleephq_enabled);
    prefs.putBool("shq_auto", cfg.sleephq_auto_after_therapy);
    prefs.putString("shq_id", cfg.sleephq_client_id);
    prefs.putString("shq_secret", cfg.sleephq_client_secret);
    prefs.putString("shq_team", cfg.sleephq_team_id);
    prefs.putString("shq_device", cfg.sleephq_device_id);
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

struct KVEntry {
    const char *key;
    enum { STR, U8, U16, U32, BOOL } type;
    void *ptr;
};

#define KV_STR(k, f)  { k, KVEntry::STR,  &cfg.f }
#define KV_U8(k, f)   { k, KVEntry::U8,   &cfg.f }
#define KV_U16(k, f)  { k, KVEntry::U16,  &cfg.f }
#define KV_U32(k, f)  { k, KVEntry::U32,  &cfg.f }
#define KV_BOOL(k, f) { k, KVEntry::BOOL, &cfg.f }

static const KVEntry kv_table[] = {
    KV_STR("hostname", hostname),
    KV_U8("wifi_mode", wifi_mode),
    KV_BOOL("wifi_roam", wifi_roam),
    KV_STR("wifi_country", wifi_country),
    KV_U16("tcp_port", tcp_port),
    KV_BOOL("oxi_enabled", oxi_enabled),
    KV_BOOL("oxi_auto_start", oxi_auto_start),
    KV_BOOL("oxi_feed_therapy_only", oxi_feed_therapy_only),
    KV_U8("oxi_device_type", oxi_device_type),
    KV_STR("oxi_device_addr", oxi_device_addr),
    KV_U16("oxi_interval_ms", oxi_interval_ms),
    KV_BOOL("oxi_lframe_continuous", oxi_lframe_continuous),
    KV_BOOL("oxi_require_known", oxi_require_known),
    KV_U32("uart_baud", uart_baud),
    KV_U16("uart_cmd_timeout_ms", uart_cmd_timeout_ms),
    KV_U8("uart_max_retries", uart_max_retries),
    KV_BOOL("allow_transparent_during_therapy", allow_transparent_during_therapy),
    KV_U16("debug_port", debug_port),
    KV_BOOL("syslog_en", syslog_enabled),
    KV_STR("syslog_host", syslog_host),
    KV_U16("syslog_port", syslog_port),
    KV_U16("http_port", http_port),
    KV_STR("http_user", http_user),
    KV_STR("http_pass", http_pass),
    KV_STR("ota_password", ota_password),
    KV_STR("update_url", update_url),
    KV_STR("ntp_server", ntp_server),
    KV_STR("tz", tz),
    KV_U16("udp_oxi_port", udp_oxi_port),
    KV_U8("mitm_mode", mitm_mode),
    KV_BOOL("smb_enabled", smb_enabled),
    KV_BOOL("smb_auto_after_therapy", smb_auto_after_therapy),
    KV_STR("smb_endpoint", smb_endpoint),
    KV_STR("smb_user", smb_user),
    KV_STR("smb_password", smb_password),
    KV_BOOL("sleephq_enabled", sleephq_enabled),
    KV_BOOL("sleephq_auto_after_therapy", sleephq_auto_after_therapy),
    KV_STR("sleephq_client_id", sleephq_client_id),
    KV_STR("sleephq_client_secret", sleephq_client_secret),
    KV_STR("sleephq_team_id", sleephq_team_id),
    KV_STR("sleephq_device_id", sleephq_device_id),
    { nullptr, KVEntry::U8, nullptr }
};

bool Config::get_value(const char *key, String &out) {
    for (const KVEntry *e = kv_table; e->key; e++) {
        if (strcasecmp(key, e->key) == 0) {
            switch (e->type) {
            case KVEntry::STR:  out = *(String*)e->ptr; break;
            case KVEntry::U8:   out = String(*(uint8_t*)e->ptr); break;
            case KVEntry::U16:  out = String(*(uint16_t*)e->ptr); break;
            case KVEntry::U32:  out = String(*(uint32_t*)e->ptr); break;
            case KVEntry::BOOL: out = (*(bool*)e->ptr) ? "1" : "0"; break;
            }
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

    for (const KVEntry *e = kv_table; e->key; e++) {
        if (strcasecmp(key, e->key) == 0) {
            switch (e->type) {
            case KVEntry::STR:  *(String*)e->ptr = value; break;
            case KVEntry::U8:   *(uint8_t*)e->ptr = atoi(value); break;
            case KVEntry::U16:  *(uint16_t*)e->ptr = atoi(value); break;
            case KVEntry::U32:  *(uint32_t*)e->ptr = atol(value); break;
            case KVEntry::BOOL: *(bool*)e->ptr = (atoi(value) != 0); break;
            }
            return true;
        }
    }
    return false;
}

void Config::foreach_kv(kv_visitor_fn fn, void *ctx) {
    String val;
    for (const KVEntry *e = kv_table; e->key; e++) {
        val = "";
        get_value(e->key, val);
        fn(e->key, val, ctx);
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
    foreach_kv([](const char *key, const String &val, void *p) {
        String v = val;
        if ((strstr(key, "pass") || strstr(key, "secret")) &&
            v.length() > 0) v = "****";
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
