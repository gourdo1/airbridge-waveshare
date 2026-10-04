# Hardware

AirBridge connects to the AirSense 10 edge connector: UART plus +24 V.

## Variants

In order of preference:

| Variant | Build env | SD | Docs |
|---------|-----------|----|------|
| PCB: interface board + XIAO main board | `xiao-esp32s3-plus-sdmmc4` | yes | [hardware_pcb.md](hardware_pcb.md) |
| Hand-wired XIAO ESP32-S3 Plus + microSD | `xiao-esp32s3-plus-sdmmc4` | yes | [hardware_wiring.md](hardware_wiring.md#xiao-esp32-s3-plus--sd) |
| Waveshare ESP32-S3-GEEK | `waveshare-esp32-s3-geek` | onboard | [below](#waveshare-esp32-s3-geek) |
| Waveshare ESP32-S3-LCD-1.54 (UART pads) | `waveshare-s3-lcd154` | onboard | [below](#waveshare-esp32-s3-lcd-154) |
| Waveshare ESP32-S3-LCD-1.54 (SCL/SDA pads) | `waveshare-s3-lcd154-i2cpads` | onboard | [below](#alternate-wiring-sclsda-pads) |
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

## Waveshare ESP32-S3-LCD-1.54

The `waveshare-s3-lcd154` build target runs on the Waveshare
[ESP32-S3-LCD-1.54](https://www.waveshare.com/esp32-s3-lcd-1.54.htm) and its
touch variant. The AirSense UART uses the board's exposed UART pads:

| Signal | Waveshare pad | GPIO |
|--------|---------------|------|
| AirSense Tx -> AirBridge RX | ESP_RXD | GPIO44 |
| AirSense Rx <- AirBridge TX | ESP_TXD | GPIO43 |
| AirSense GND | GND | - |

GPIO43/44 are the ESP32-S3 UART0 pins, so this target runs the console (logs,
`$` commands, provisioning) over the native USB-C port instead. Nothing but
AirSense traffic is sent on GPIO43 once the firmware is running.

### Alternate wiring: SCL/SDA pads

If the ESP_TXD/ESP_RXD pads are damaged, use the `waveshare-s3-lcd154-i2cpads`
build target (and `waveshare-s3-lcd154-i2cpads-ota` for WiFi updates) and wire
the AirSense to the SCL/SDA pads instead:

| Signal | Waveshare pad | GPIO |
|--------|---------------|------|
| AirSense Tx -> AirBridge RX | SCL | GPIO41 |
| AirSense Rx <- AirBridge TX | SDA | GPIO42 |
| AirSense GND | GND | - |

Don't swap these. SCL/SDA are the board's I2C bus, shared with the IMU, the
audio codecs and (touch variant) the touch controller, with 4.7k pull-ups.
The firmware never uses that bus, but those chips still listen to it:

- I2C chips never drive SCL, so the AirSense's output on SCL is never fought.
- An I2C chip can pull SDA low to acknowledge. AirBridge therefore drives
  SDA open-drain and relies on the 4.7k pull-up for the high level, so
  nothing can short.
- The touch controller is held in reset (GPIO47) to take it off the bus.
- Rarely, when both sides transmit at once and the bits happen to match a
  chip's address, a chip may corrupt a byte. Commands fail their checksum and
  are retried, and a lost oximetry sample is replaced half a second later.

A side benefit: GPIO41/42 aren't the ESP32-S3 console pins, so the boot text
described below never reaches the AirSense on this wiring.

### Power

Set the step-down converter to **3.3V** and feed it into the board's **3V3
pad**, the same way as the M5Stamp Pico. This bypasses the board's own
USB/battery power path and its 3.3V regulator. Keep the 47 Ohm inrush resistor
on the +24V input.

- Trim the converter to 3.3V with a meter **before** connecting it. The 3V3
  rail feeds the ESP32-S3, flash, PSRAM and LCD directly with no protection,
  and 3.6V is the ESP32-S3 absolute maximum.
- Size the converter for at least 500mA. WiFi and BLE together draw short
  current peaks well above the average, and a sagging supply causes brownout
  resets.
- Don't leave a USB-C cable connected while the 3.3V feed is powered for long
  periods. With both connected, the onboard regulator and the external
  converter drive the 3V3 rail in parallel. A short USB flashing or serial
  session is fine, but for long debugging runs disconnect one of the two.
- Leave the battery header empty.

```

    AirSense 10                          Waveshare ESP32-S3-LCD-1.54
    Edge Connector                       +----------------------+
                                         |                      |
    Tx  -------------------------------- ESP_RXD pad (GPIO44)   |
    Rx  -------------------------------- ESP_TXD pad (GPIO43)   |
    GND ----------------+      +-------- GND pad                |
                        |      |    +--> 3V3 pad                |
    +24V ---[47R]--+    |      |    |    +----------------------+
                   |    |      |    |
               +---+----+------+----+----+
               |  IN+  IN-    OUT- OUT+  |
               |        MP1584EN         |
               |       (3.3V out)        |
               +-------------------------+
```

Notes:

- The LCD shows a status screen: IP address, time, AirSense state, mask
  pressure during therapy, and SpO2/pulse when an oximeter is feeding.
  The backlight runs at a low dim level. Pressing any of the three buttons
  (BOOT, GPIO4, GPIO5) raises it to full brightness for 10 seconds. It also
  stays bright for 10 seconds after boot so the IP address can be read. Tune
  the dim level with `-DAB_LCD_DIM_DUTY=<0-1023>` in `platformio.ini` if the
  default (8) is too dark or too bright.
- While therapy is running, the screen subscribes to the AirSense's 25 Hz
  pressure stream (the same one the web UI's live graph uses) and shows a
  one-second average.
- GPIO2 (battery power latch) is driven high at boot. This only matters when
  the board runs from a LiPo, and does nothing when powered through the 3V3 pad.
- The ESP32-S3 boot ROM prints a short burst of text on GPIO43 at 115200 baud
  right after reset, before the firmware starts. The AirSense sees it as line
  noise and the firmware's health polling recovers. If it ever causes trouble,
  the ROM output can be disabled permanently by burning the
  `UART_PRINT_CONTROL` eFuse (irreversible).
- The onboard TF card slot is used for SD recording and export (see
  [What needs SD](#what-needs-sd)). It is 4-bit SDMMC at 20 MHz:

  | CLK | CMD | D0 | D1 | D2 | D3 |
  |-----|-----|----|----|----|----|
  | GPIO16 | GPIO15 | GPIO17 | GPIO18 | GPIO13 | GPIO14 |

- Partition table: `partitions_s3_16mb.csv` (two 7.75 MB app slots plus a
  crash-dump partition). A board still on the older 8 MB layout keeps working
  after a WiFi update, but only gets the new layout (and crash dumps) after
  one USB flash of the complete image: `pio run -e <env> -t upload`, or
  `pio run -e <env> -t initialbin` and then `firmware.factory.bin` at
  address `0x0` in a web flasher.

## Enclosure

3D-printable enclosure for the PCB variant:
[airbridge-xiao.stl](../hardware/enclosure/xiao/airbridge-xiao.stl).

## Sources

KiCad projects, PDFs, gerbers and assembly files are in
[`hardware/`](../hardware/README.md).
