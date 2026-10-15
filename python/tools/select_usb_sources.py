"""Keep Arduino Serial/JTAG, but omit its always-on TinyUSB owner."""

from pathlib import Path
import sys

from build_tinyusb import select_core_source

Import("env")
env.AddBuildMiddleware(select_core_source, "*/cores/esp32/*")

if sys.platform == "win32":
    # Fork-local (docs/local_patches.md): on Windows the pioarduino platform
    # wraps all build middlewares in its own integrated_middleware and discards
    # the node they return, so the None above never drops a core source. Leave
    # the same sources out of the core library's src_filter instead. The core
    # is built from a Clone() of this env, which keeps methods added here.
    from platformio.builder.tools.piobuild import (
        BuildLibrary, SRC_BUILD_EXT, SRC_FILTER_DEFAULT,
    )

    def build_core_library(env, variant_dir, src_dir, src_filter=None, nodes=None):
        if nodes is None and Path(env.subst(variant_dir)).name == "FrameworkArduino":
            root = Path(env.subst(src_dir))
            dropped = sorted(
                item for item in env.MatchSourceFiles(src_dir, src_filter, SRC_BUILD_EXT)
                if select_core_source(env, env.File(str(root / item))) is None
            )
            if dropped:
                base = src_filter or SRC_FILTER_DEFAULT
                if isinstance(base, (list, tuple)):
                    base = " ".join(base)
                src_filter = base + "".join(f" -<{item}>" for item in dropped)
        return BuildLibrary(env, variant_dir, src_dir, src_filter, nodes)

    env.AddMethod(build_core_library, "BuildLibrary")
