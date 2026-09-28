# Quick Start

## What you need
<!-- TODO: rework with SD/non-SD hardware selection -->
- M5Stamp Pico (ESP32-PICO-D4) or XIAO ESP32S3 Plus
- MP1584 buck converter (24V to 3.3V)
- AirSense 10 with edge connector access
- USB-to-serial adapter (3.3V) for initial flash
- PlatformIO installed

## Wiring

See [hardware.md](hardware.md) for the pinout, wiring diagram, and power notes.

## Flash firmware

```bash
pio run -e m5stamp-pico -t upload
```

For the XIAO ESP32S3 Plus SDMMC4 build:

```bash
pio run -e xiao-esp32s3-plus-sdmmc4 -t upload
```

## Configure WiFi

On a fresh installation, connect to the `airbridge_...` WiFi access point
(password `airbridge`) and open `http://192.168.4.1/`.

Log in with username `admin` and password `airbridge`. The setup wizard guides
you through your WiFi network, AirSense connection, time, optional sync
destinations, and Web UI credentials. Use **Save & next**, then **Finish**.
You can reopen the wizard from **Config > Initial setup**.

Alternatively, copy `provision.env.example` to `provision.env` and fill in
your WiFi credentials before flashing. Provisioning runs automatically after
a serial upload.

## Verify it works

Once connected to your WiFi, open `http://airbridge/`. If the hostname does
not resolve, use the device IP from your router's DHCP leases.

The **Dashboard** should show the AirSense name and serial number. With the
AirSense powered on, connected, and not running therapy, its state is
`IDLE`.

See [Configuration](configuration.md) for available settings.
