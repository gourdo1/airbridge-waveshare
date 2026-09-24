# Hardware

## AirSense 10 edge connector


| Left | Right |
|------|-------|
| **Tx**   | nc    |
| **Rx**   | **GND**   |
| nc   | nc    |
| nc   | nc    |
| nc   | **+24V**  |

UART: 3.3V logic, 57600 8N1.

## Power

+24V rail from the edge connector. Use a buck converter (e.g. MP1584) to step down to 3.3V for the ESP32.

Hot-plugging the stepdown module may trip the AirSense overcurrent protection, causing it to shut down. A ~47 Ohm series resistor on the +24V input limits inrush current and prevents this.

## Bare minimum hardware diagram

```

    AirSense 10                          M5Stamp Pico
    Edge Connector                       +-----------+
                                         |           |
    Tx  -------------------------------- G36 (RX)    |  (pin definitions can be set
    Rx  -------------------------------- G26 (TX)    |   in include/board.h)
    GND ----------------+--------------- GND         |
                        |                |           |
    +24V ---[47R]--+    |    +--------> 3V3          |
                   |    |    |           |           |
                   |    |    |           +-----------+
                   |    |    |
                   |    |    |
               +---+----+----+---+
               |  IN+  GND  OUT+ |
               |    MP1584EN     |
               |   (3.3V out)    |
               +-----------------+


    Notes:
    - 47 ohm resistor in series with +24V prevents
      overcurrent trip when hot-plugging the step-down module
    - MP1584EN is a cheap ready-made module. Any 24V-to-3.3V
      step-down will work as long as it can supply 300mA+
    - TODO: design proper board closer to IEC 60601
      for medical electrical equipment
```

## XIAO ESP32S3 Plus

The `xiao-esp32s3-plus` and `xiao-esp32s3-plus-sdmmc4` build targets keep
the same AirSense UART role, but move the pins to match the AirCANnect-style
XIAO layout:

| Signal | XIAO ESP32S3 Plus GPIO |
|--------|-------------------------|
| AirSense TX -> AirBridge RX | GPIO2 |
| AirSense RX <- AirBridge TX | GPIO1 |

The `xiao-esp32s3-plus-sdmmc4` target enables SDMMC4 as an optional
compile-time capability:

| SD signal | XIAO ESP32S3 Plus GPIO |
|-----------|-------------------------|
| CLK       | GPIO13 |
| CMD       | GPIO11 |
| D0        | GPIO12 |
| D1        | GPIO38 |
| D2        | GPIO39 |
| D3        | GPIO40 |

Use pull-ups on CMD and D0-D3. If early prototypes are wired with long leads,
drop `AB_SDMMC_FREQ_KHZ` from `40000` to `20000` in the build flags.

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
    GND ----------------+--------------- GND pad                |
                        |                |                      |
    +24V ---[47R]--+    |    +---------> 3V3 pad                |
                   |    |    |           +----------------------+
               +---+----+----+---+
               |  IN+  GND  OUT+ |
               |    MP1584EN     |
               |   (3.3V out)    |
               +-----------------+
```

Notes:

- The LCD backlight (GPIO46) is driven off at boot. The screen stays dark;
  there is no display output yet.
- GPIO2 (battery power latch) is driven high at boot. This only matters when
  the board runs from a LiPo, and does nothing when powered through the 3V3 pad.
- The ESP32-S3 boot ROM prints a short burst of text on GPIO43 at 115200 baud
  right after reset, before the firmware starts. The AirSense sees it as line
  noise and the firmware's health polling recovers. If it ever causes trouble,
  the ROM output can be disabled permanently by burning the
  `UART_PRINT_CONTROL` eFuse (irreversible).
- The onboard TF card slot is not used by the firmware yet.

## Enclosure

STL files for 3D-printed case in [`docs/stl/`](stl/).
