# Vendored libraries

Third-party libraries that require project-specific PlatformIO build metadata
live here. Each library retains its upstream licence and attribution files.

`SD_MMC/` is a locally patched copy of the Arduino-ESP32 library that
overrides the framework's own; see [`SD_MMC/README.md`](SD_MMC/README.md)
and [`docs/local_patches.md`](../docs/local_patches.md) for when to remove it.
