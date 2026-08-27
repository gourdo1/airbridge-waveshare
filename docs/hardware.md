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

## Enclosure

STL files for 3D-printed case in [`docs/stl/`](stl/).
