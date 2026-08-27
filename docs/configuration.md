# Configuration

Settings are stored in NVS (non-volatile storage) and persist across reboots.

## How to configure

**Web UI:** Device tab -> edit fields -> Save

**CLI:** `$CONFIG key value` then `$CONFIG SAVE`

**Provisioning:** Copy `provision.env.example` to `provision.env`, fill in
your values, and flash. `provision.py` runs after the upload completes and
applies each line via the appropriate command:

- Most keys go through `$CONFIG key value`.
- `wifi_net=SSID PASSWORD` is routed through `$WIFI ADD ssid pass`. Use
  shell-style double quotes if either field contains spaces:
  `wifi_net="My Network" "correct horse battery staple"`. Multiple
  `wifi_net=` lines provision multiple networks (up to 4). Provisioning
  appends; existing networks on the device are kept.

## Settings reference

### WiFi & Network

WiFi networks are managed as a list (up to 4 SSIDs) via the web UI's WiFi
Networks card or the `$WIFI ADD ssid pass` / `$WIFI REMOVE N` commands. The
top-level WiFi-related config keys are:

| Key | Default | Description |
|-----|---------|-------------|
| `hostname` | airbridge | Device hostname (mDNS + softAP SSID prefix) |
| `wifi_mode` | 1 | Operating mode, see table below |
| `wifi_roam` | true | Enable hysteresis-based roaming (8 dB threshold) |
| `wifi_country` | 01 | ISO 3166 country code; "01" = worldwide |
| `tcp_port` | 23 | TCP command port |
| `debug_port` | 8023 | Debug log stream port (read-only) |

**`wifi_mode` values:**

| Value | Name | Behavior |
|-------|------|----------|
| 0 | auto | STA-first; on failure, automatic AP+STA fallback. AP tears itself down 2 minutes after STA recovers and the AP is client-free. |
| 1 | AP only | softAP only, no STA attempts. Fast boot, useful as a setup-only or bench device. |
| 2 | off | WiFi disabled entirely (serial/UART only). |
| 3 | STA only | STA only, never falls back to AP. On total STA failure, keeps retrying scans every 30 s. |
| 4 | STA+AP always | softAP up from boot alongside STA. AP is never torn down. |

The softAP (when used) is named `<hostname>_<MAC>` with password `airbridge`,
and serves the web UI at `192.168.4.1`.

When station mode is configured but no networks reachable, the device tries
SmartConfig for 60 seconds, then falls back per the mode table above.

The `wifi_country` key affects channel allocation and TX power limits per
regulatory domain. Use a 2-letter ISO code (`US`, `DE`, `JP`, `PL`, ...) or
`01` to use the worldwide-safe defaults. Bad values are rejected with a
log warning and the previous setting stays in effect.

### Web UI

| Key | Default | Description |
|-----|---------|-------------|
| `http_port` | 80 | Web server port |
| `http_user` | admin | Web UI username |
| `http_pass` | airbridge | Web UI password |

### OTA

| Key | Default | Description |
|-----|---------|-------------|
| `ota_password` | airbridge | ArduinoOTA password |

Set the `AIRBRIDGE_OTA_PASS` environment variable to match when uploading:
```bash
export AIRBRIDGE_OTA_PASS=airbridge
pio run -e ota -t upload
```

### Time & Timezone

| Key | Default | Description |
|-----|---------|-------------|
| `ntp_server` | *(empty)* | NTP server address. Empty = use DHCP-provided server, or pool.ntp.org as fallback |
| `tz` | UTC0 | POSIX timezone string (e.g. `CET-1CEST,M3.5.0,M10.5.0/3`) |

The web UI Device tab has a timezone helper that detects your browser's timezone and generates the POSIX string.

When NTP syncs, the ResMed device clock is set automatically. If NTP is unavailable, the ResMed clock is used as fallback for the ESP system time.

### Oximetry

| Key | Default | Description |
|-----|---------|-------------|
| `oxi_enabled` | true | Enable BLE oximeter support |
| `oxi_auto_start` | true | Start feeding data automatically on connect |
| `oxi_feed_therapy_only` | false | Only inject readings during active therapy |
| `oxi_device_type` | 0 | 0 = auto-detect, 1 = Nonin, 2 = O2Ring, 3 = PLX, 4 = WS20A |
| `oxi_device_addr` | *(empty)* | Preferred oximeter MAC (AA:BB:CC:DD:EE:FF) |
| `oxi_interval_ms` | 500 | Injection interval in milliseconds |
| `oxi_lframe_continuous` | true | Send L-frames even when no valid reading (keeps link alive) |
| `udp_oxi_port` | 8025 | UDP oximetry listener port, 0 = disabled |

BLE oximeters are managed from the **Bluetooth** tab. For UDP oximetry, see [udp_oximetry.md](udp_oximetry.md).

Only one source feeds at a time. First to deliver data wins, 10 seconds of silence releases.

### UART

| Key | Default | Description |
|-----|---------|-------------|
| `uart_baud` | 57600 | AirSense UART baud rate (don't change unless you know what you're doing) |
| `uart_cmd_timeout_ms` | 500 | Command response timeout |
| `uart_max_retries` | 3 | Retry count for failed commands |

### Advanced

| Key | Default | Description |
|-----|---------|-------------|
| `allow_transparent_during_therapy` | false | Allow raw UART passthrough during therapy |
| `mitm_mode` | 0 | 0 = off, 1 = forward, 2 = log, 3 = filter |

## CLI commands

All commands are prefixed with `$`. Anything without `$` is sent to the AirSense as a Q-frame command.

| Command | Description |
|---------|-------------|
| `$STATUS` | System state, oximetry, UART stats, heap |
| `$TIME` | Show UTC, local time, timezone, NTP sync state |
| `$TIMESYNC` | Re-trigger ResMed clock sync from NTP |
| `$OXI SCAN` | Scan for BLE oximeters |
| `$OXI RESULTS` | Show scan results |
| `$OXI CONNECT [addr]` | Connect to oximeter |
| `$OXI DISCONNECT` | Disconnect oximeter |
| `$OXI START` / `STOP` | Start/stop data injection |
| `$OXI STATUS` | Oximeter connection details |
| `$CONFIG` | Dump all config |
| `$CONFIG key` | Get single value |
| `$CONFIG key value` | Set value (not saved until SAVE) |
| `$CONFIG SAVE` | Persist to NVS |
| `$CONFIG RESET` | Reset all to defaults |
| `$WIFI` / `$WIFI STATUS` | Connection state, RSSI, roaming flag |
| `$WIFI LIST` | List configured networks |
| `$WIFI ADD ssid pass` | Add a network (max 4) |
| `$WIFI REMOVE N` | Remove network at index N |
| `$WIFI HINTS` | Show cached BSSID/channel/PMF hints |
| `$WIFI HINTS CLEAR` | Drop all cached hints |
| `$FLASH [block] [FORCE]` | Flash uploaded ResMed firmware |
| `$FLASH STATUS` | Flash progress |
| `$FLASH CANCEL` | Abort flash |
| `$LOG` | Show log levels |
| `$LOG category level` | Set log level (ERROR/WARN/INFO/DEBUG) |
| `$TRANSPARENT` | Raw UART passthrough (60s idle timeout) |
| `$VERSION` | Firmware version |
| `$REBOOT` | Restart device |

Log categories: `GENERAL`, `OXI`, `TCP`, `WIFI`, `OTA`, `WEB`, `ARB`, `HEALTH`, `ALL`
