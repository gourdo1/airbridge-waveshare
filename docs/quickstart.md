# Quick Start

## What you need
- AirBridge hardware: the PCB, or a hand-wired XIAO ESP32-S3 Plus with or
  without SD. See [hardware.md](hardware.md) for variants and which features
  need SD.
- AirSense 10 with edge connector access
- PlatformIO installed

## Wiring

PCB: [hardware_pcb.md](hardware_pcb.md).
Hand-wired: [hardware_wiring.md](hardware_wiring.md).

## Flash firmware

XIAO with SD (PCB or hand-wired):

```bash
pio run -e xiao-esp32s3-plus-sdmmc4 -t upload
```

XIAO without SD:

```bash
pio run -e xiao-esp32s3-plus -t upload
```

M5Stamp Pico (legacy; needs a 3.3 V USB-to-serial adapter):

```bash
pio run -e m5stamp-pico -t upload
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
