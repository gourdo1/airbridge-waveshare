#!/usr/bin/env python3
"""Build patched WebResponses.cpp without changing the installed library."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from library_patches import apply_patches, patch_files


TARGET = ("espasyncwebserver", "src", "webresponses.cpp")


def is_target(path: Path) -> bool:
    return tuple(part.lower() for part in path.parts[-3:]) == TARGET


def patch_in_place(original: Path, patches: list[Path]) -> None:
    # Fork-local (docs/local_patches.md): on Windows the pioarduino platform
    # wraps all build middlewares in its own integrated_middleware, which
    # calls them for every source and discards the node they return, so a
    # generated replacement never reaches the compiler. Patch the installed
    # copy instead, once; a reverse check detects an already applied patch.
    for patch in patches:
        patch_path = str(patch.resolve())
        applied = subprocess.run(
            ["git", "apply", "--no-index", "--reverse", "--check", patch_path],
            cwd=original.parent, capture_output=True,
        ).returncode == 0
        if not applied:
            subprocess.run(
                ["git", "apply", "--no-index", patch_path],
                cwd=original.parent, check=True,
            )


def build_source(env, node):
    original = Path(node.srcnode().get_abspath())
    # The middleware pattern is not honoured on Windows (see patch_in_place).
    if not is_target(original):
        return node
    patches = patch_files(Path(env["PROJECT_DIR"]) / "patches" / "espasyncwebserver")
    if not patches:
        raise RuntimeError("ESPAsyncWebServer patches are missing")

    if sys.platform == "win32":
        patch_in_place(original, patches)
        return node

    output = Path(env.subst("$BUILD_DIR")) / "generated" / "WebResponses.cpp"
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        staging = Path(temporary)
        source = staging / original.name
        shutil.copyfile(original, source)
        apply_patches(staging, patches)
        content = source.read_bytes()

    if not output.exists() or output.read_bytes() != content:
        output.write_bytes(content)
    return env.File(str(output))


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(build_source, "*/ESPAsyncWebServer/src/WebResponses.cpp")
