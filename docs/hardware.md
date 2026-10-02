# Hardware

AirBridge connects to the AirSense 10 edge connector: UART plus +24 V.

## Variants

In order of preference:

| Variant | Build env | SD | Docs |
|---------|-----------|----|------|
| PCB: interface board + XIAO main board | `xiao-esp32s3-plus-sdmmc4` | yes | [hardware_pcb.md](hardware_pcb.md) |
| Hand-wired XIAO ESP32-S3 Plus + microSD | `xiao-esp32s3-plus-sdmmc4` | yes | [hardware_wiring.md](hardware_wiring.md#xiao-esp32-s3-plus--sd) |
| Waveshare ESP32-S3-GEEK | `waveshare-esp32-s3-geek` | onboard | [below](#waveshare-esp32-s3-geek) |
| Hand-wired XIAO ESP32-S3 Plus | `xiao-esp32s3-plus` | no | [hardware_wiring.md](hardware_wiring.md#xiao-esp32-s3-plus-without-sd) |
| Hand-wired M5Stamp Pico (legacy) | `m5stamp-pico` | no | [hardware_wiring.md](hardware_wiring.md#m5stamp-pico-legacy) |

The PCB and the hand-wired XIAO + SD use the same GPIOs and the same firmware.

The M5Stamp Pico has no PSRAM and no SD support. It is kept building, but new
features are not designed around it.

## What needs SD

Requires SD:

- local therapy recording in AirSense EDF format (`DATALOG`, `STR.edf`,
  `Identification`)
- SMB export
- SleepHQ export
- Storage browser (list, download, ZIP, rename, delete)

Works without SD:

- UART/TCP bridge
- live pressure/flow charts
- oximetry (BLE, UDP)
- clinical settings
- Sleep Report
- ResMed OTA and AirBridge OTA

On builds without SD the Sync and Storage tabs are hidden and the SMB/SleepHQ
setup steps are skipped.

## AirSense 10 edge connector

| Left | Right |
|------|-------|
| **Tx**   | nc    |
| **Rx**   | **GND**   |
| nc   | nc    |
| nc   | nc    |
| nc   | **+24V**  |

UART: 3.3 V logic, 57600 8N1. AirSense Tx goes to AirBridge RX.

## GPIO

| Signal | XIAO ESP32-S3 Plus | M5Stamp Pico |
|--------|--------------------|--------------|
| UART RX (from AirSense Tx) | GPIO2 (D1) | G36 |
| UART TX (to AirSense Rx) | GPIO1 (D0) | G26 |
| LED | GPIO21 | G27 |

SD, 4-bit SDMMC (`xiao-esp32s3-plus-sdmmc4`):

| CLK | CMD | D0 | D1 | D2 | D3 |
|-----|-----|----|----|----|----|
| GPIO13 | GPIO11 | GPIO12 | GPIO38 | GPIO39 | GPIO40 |

Pins are set by build flags in `platformio.ini`; defaults are in
`include/board.h`. The XIAO SD clock is 40 MHz (`AB_SDMMC_FREQ_KHZ=40000`).

### Waveshare ESP32-S3-GEEK

16 MB flash, 2 MB PSRAM, onboard microSD. The LCD is not used; its backlight
is switched off.

| UART connector | AirSense |
|----------------|----------|
| RX (GPIO44) | Tx |
| TX (GPIO43) | Rx |
| GND | GND |

The onboard card uses 4-bit SDMMC at 40 MHz:

| CLK | CMD | D0 | D1 | D2 | D3 |
|-----|-----|----|----|----|----|
| GPIO36 | GPIO35 | GPIO37 | GPIO33 | GPIO38 | GPIO34 |

Power through the USB-A connector with 5 V, not the AirSense 24 V supply.
Pinout: [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-S3-GEEK/ESP32-S3-GEEK-Schematic1.pdf).

## Power

### XIAO through BAT

The XIAO is fed from a 5 V buck into its **BAT** pad, through a diode
(Schottky when hand-wired, LM66100 ideal diode on the PCB). No battery is
fitted.

- The **5V** pad is USB VBUS. Feeding it would push current into the USB host
  whenever a cable is connected. BAT is the board's own supply input.
- The diode keeps the XIAO charger and USB from backfeeding the buck.
- With the buck powered, BAT is held above the charger's regulation voltage,
  so the charger does not supply charging current.
- USB stays usable for flashing and logs with the device installed.

### Series resistor on +24 V

Hot-plugging a buck module into the AirSense charges its input capacitors hard
enough to trip the AirSense overcurrent protection, which shuts it down.
A ~47 Ω resistor in series with +24 V limits that inrush.

It works, but it is not an optimal solution: it drops voltage and dissipates
power continuously, so it needs a 2 W part. The PCB replaces it with the
eFuse soft-start; see [hardware_pcb.md](hardware_pcb.md#power).

## Enclosure

3D-printable enclosure for the PCB variant:
[airbridge-xiao.stl](../hardware/enclosure/xiao/airbridge-xiao.stl).

## Sources

KiCad projects, PDFs, gerbers and assembly files are in
[`hardware/`](../hardware/README.md).
