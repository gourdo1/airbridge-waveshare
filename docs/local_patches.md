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

## `.patch` files checked out as CRLF on Windows (upstream AirBridge)

| | |
|---|---|
| Patched | 2026-10-09 |
| Where | [`.gitattributes`](../.gitattributes): `*.patch text eol=lf` |
| Affects | Every build on Windows with `core.autocrlf=true`, all targets |
| Symptom | Build fails in `python/tools/library_patches.py` (`git apply` returns an error) while generating `WebResponses.cpp` |
| Cause | Upstream v2.1.1 applies `patches/espasyncwebserver/*.patch` with `git apply` at build time. Git converts the patch to CRLF on checkout; the library source is LF, so no hunk matches |
| Upstream status | No `.gitattributes` in upstream v2.1.1 (2026-10-09) |
| Remove when | Upstream adds an equivalent rule (any `.gitattributes` that keeps `*.patch` LF), or stops applying patches with `git apply`. Then drop this file or merge it with upstream's |

## Build middleware ignored on Windows (pioarduino platform)

| | |
|---|---|
| Patched | 2026-10-09 |
| Where | [`python/tools/extend_asyncwebserver.py`](../python/tools/extend_asyncwebserver.py) (upstream AirBridge script, fork-modified) |
| Affects | Every build on Windows, all targets |
| Symptom | Build fails in `library_patches.py`: `git apply` reports `.../generated/tmpXXXX/WebResponses.cpp: No such file or directory` (the temp folder holds `ColorFormat.c`) |
| Cause | On Windows, pioarduino 55.03.37 (`builder/frameworks/arduino.py`, `integrated_middleware`) replaces all registered build middlewares with one wrapper that calls each of them for every source file, ignoring their patterns, and compiles the original source, ignoring the node they return. Upstream's middleware therefore ran for the wrong file, and its patched replacement would never be compiled anyway |
| Fix | The middleware checks the file name itself, and on Windows applies the patch to the installed library copy in place (once; a reverse `git apply --check` detects an applied patch). Linux/macOS keep upstream's generated-copy behaviour |
| Upstream status | Present in pioarduino 55.03.37 and upstream AirBridge v2.1.1 (2026-10-09) |
| Remove when | pioarduino honours middleware patterns and return values on Windows, or upstream AirBridge handles Windows itself. Then restore upstream's `extend_asyncwebserver.py` |

After adding the `.gitattributes` rule, refresh existing checkouts once:
`git rm --cached -r patches && git checkout -- patches` (or delete the `.patch`
files and `git checkout -- patches`).
