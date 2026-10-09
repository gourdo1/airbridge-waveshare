# Local patches to third-party code

Workarounds for bugs in libraries or frameworks that AirBridge builds
against. Each entry says how to tell when the upstream fix has landed, so the
local copy can be removed. Check this list whenever the pioarduino platform
or Arduino core version in `platformio.ini` changes, and before merging
upstream AirBridge.

## SD_MMC drops `SDMMC_HOST_FLAG_DEINIT_ARG` (arduino-esp32)

| | |
|---|---|
| Patched | 2026-10-07 |
| Where | [`lib/SD_MMC/`](../lib/SD_MMC/README.md), copied from arduino-esp32 3.3.7 |
| Affects | Every SD-capable build. Visible on Waveshare ESP32-S3-LCD-1.54, where GPIO0 is a button |
| Symptom | After an SD unmount or failed mount, GPIO0's input buffer is switched off and the left button reads as permanently pressed. `$LCD` shows `ie=0 pu=1` for GPIO0 |
| Cause | `SDMMCFS::begin()` overwrites `host.flags`, losing `SDMMC_HOST_FLAG_DEINIT_ARG`; ESP-IDF then calls `sdmmc_host_deinit_slot()` without a slot number and may release the unused slot 0, resetting GPIO0 |
| Upstream status | Not fixed in arduino-esp32 3.3.7, 3.3.12 or `master` (2026-10-07) |
| Remove when | The arduino-esp32 version AirBridge builds against keeps `SDMMC_HOST_FLAG_DEINIT_ARG` in `SD_MMC.cpp`. Then delete `lib/SD_MMC/` and this entry |

Details and the exact two-line change: [`lib/SD_MMC/README.md`](../lib/SD_MMC/README.md).
