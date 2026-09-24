# Quick Start

## What you need

- One of the supported boards:
  - Waveshare ESP32-S3-LCD-1.54 (with or without touch)
  - M5Stamp Pico (ESP32-PICO-D4)
  - XIAO ESP32S3 Plus
- MP1584 buck converter (24V to 3.3V)
- AirSense 10 with edge connector access
- USB-C cable (Waveshare, XIAO) or a 3.3V USB-to-serial adapter (M5Stamp Pico) for the initial flash
- PlatformIO installed

## Wiring

See [hardware.md](hardware.md) for the pinout, wiring diagram, and power notes.

## Flash firmware

For the Waveshare ESP32-S3-LCD-1.54:

```bash
pio run -e waveshare-s3-lcd154 -t upload
```

The board flashes over its USB-C port. If the upload can't connect, hold the
**BOOT** button while plugging in the USB cable, then retry. The serial
console for this board is the same USB-C port.

For the M5Stamp Pico:

```bash
pio run -e m5stamp-pico -t upload
```

For the XIAO ESP32S3 Plus SDMMC4 build:

```bash
pio run -e xiao-esp32s3-plus-sdmmc4 -t upload
```

## Configure WiFi

Three options, pick whichever suits you:

**SmartConfig (no cable needed):** On first boot the device waits 60 seconds for SmartConfig. Install the [EspTouch](https://github.com/EspressifApp/EsptouchForAndroid/releases) app on your phone. Connect your phone to the target WiFi network, select **EspTouch v1**, enter the WiFi password, and hit confirm. The device picks up the credentials and connects automatically. Credentials are saved.

**Provisioning file:** Create `provision.env` from the example and flash:

```bash
cp provision.env.example provision.env
# Edit provision.env with your WiFi credentials
python provision.py <serial port>
```

Provisioning also runs automatically after every serial flash (`pio run -t upload`).
On boards with native USB (Waveshare, XIAO), the USB port briefly disappears
while the board resets after flashing. If automatic provisioning reports that
the device is not responding, run `python provision.py <serial port>` again by hand.

**Manual via AP:** If SmartConfig times out, the device creates a WiFi access point (`airbridge_XXXXXX`, password `airbridge`). Connect to it, open `http://192.168.4.1/`, go to the **Device** tab, set your WiFi SSID and password, wifi_mode to `0`. Save and reboot.

## Verify it works

After reboot, the device connects to your WiFi. Open `http://airbridge/` in your browser.

If mDNS doesn't resolve, check your router's DHCP leases for the device IP.

Default credentials:
- Username: `admin`
- Password: `airbridge`

The **Status** tab shows the system state. If the AirSense is powered on and connected, you should see `system: IDLE` along with the device name and serial number.

## Ports

| Port | Purpose |
|------|---------|
| 80   | Web UI |
| 23   | TCP command port (telnet) |
| 8023 | Debug log stream (read-only) |
| 3232 | OTA firmware updates |

## OTA updates

After initial setup, you can update firmware over WiFi:

```bash
export AIRBRIDGE_OTA_PASS=airbridge
pio run -e ota -t upload                    # M5Stamp Pico
pio run -e waveshare-s3-lcd154-ota -t upload  # Waveshare ESP32-S3-LCD-1.54
```

## Oximetry

The device scans for BLE pulse oximeters automatically. Supported devices:
- Nonin 3230 (BLE)
- Wellue/Viatom devices: O2Ring, Checkme O2, SleepU, O2M
- OxyII devices, including O2Ring-S
- ACCARE WS20A
- Generic BLE PLX / Heart Rate sensors

Go to the **Bluetooth** tab in the web UI to scan, connect, and manage oximeter devices. When connected, SpO2 and pulse data are injected into the AirSense data stream.

## Command line

Connect via telnet to port 23 for direct control:

```bash
telnet airbridge 23
```

Commands use `$` prefix. Type `$HELP` for the full list. Anything without `$` is sent directly to the AirSense as a UART command.

Debug logs stream on port 8023:

```bash
nc airbridge 8023
```
