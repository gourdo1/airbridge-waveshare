# SD_MMC (locally patched copy)

This is the Arduino-ESP32 `SD_MMC` library, copied unchanged from
**arduino-esp32 3.3.7** (`libraries/SD_MMC/src`, the core bundled with the
pinned pioarduino platform 55.03.37) except for the two lines marked
`AirBridge local patch` in `src/SD_MMC.cpp`. PlatformIO uses a library in the
project `lib/` folder in preference to the framework copy with the same name,
so every SD-capable build picks this one up. The dependency graph shows it as
`SD_MMC @ 3.3.7+airbridge.1`.

Licence: LGPL-2.1, as the rest of arduino-esp32
(<https://github.com/espressif/arduino-esp32/blob/master/LICENSE.md>).
Authors: Hristo Gochkov, Ivan Grokhtkov (see `library.properties`).

## The bug

`SDMMCFS::begin()` starts from `SDMMC_HOST_DEFAULT()` and then overwrites the
flags:

```cpp
host.flags = SDMMC_HOST_FLAG_4BIT;   // or SDMMC_HOST_FLAG_1BIT
```

That drops `SDMMC_HOST_FLAG_DEINIT_ARG`. ESP-IDF's `call_host_deinit()`
(`components/fatfs/vfs/vfs_fat_sdmmc.c`) then calls the `deinit` member of a
union whose actual function is `sdmmc_host_deinit_slot(int slot)`, without an
argument. The slot number is whatever is left in a CPU register.

When that leftover value is 0, ESP-IDF releases SDMMC slot 0, which AirBridge
never configured (it uses slot 1). Slot 0's pin record is all zeros, so
`reset_pin_if_valid()` resets GPIO0 several times: input buffer off, pull-up
on. On the Waveshare ESP32-S3-LCD-1.54, GPIO0 is the left button, which then
reads as permanently pressed until the next reset. Other values can leave the
real slot not deinitialized.

It happens on any SD unmount or failed mount attempt (the EDF recorder
retries the mount every 5 s while no card is mounted).

## The patch

Keep the flag, so ESP-IDF calls `deinit_p(host.slot)` with the real slot:

```cpp
host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
```

## When to remove this copy

Check upstream `libraries/SD_MMC/src/SD_MMC.cpp` in arduino-esp32:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/<tag>/libraries/SD_MMC/src/SD_MMC.cpp
```

Delete `lib/SD_MMC/` once the version AirBridge builds against keeps
`SDMMC_HOST_FLAG_DEINIT_ARG` (for example `host.flags |= ...`, or the flag
included in both assignments). Status when patched (2026-10-07): still
missing in 3.3.7, 3.3.12 and `master`.

Also re-check this copy whenever `platformio.ini` moves to a different
pioarduino platform / Arduino core: it is a 3.3.7 copy, and a newer core may
change the library's API. If upstream is not fixed yet, refresh the copy from
the new core and re-apply the two lines.
