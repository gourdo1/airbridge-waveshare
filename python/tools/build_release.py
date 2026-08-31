#!/usr/bin/env python3
"""Build and package AirBridge firmware release artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess


PROJECT_DIR = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_ENVIRONMENTS = (
    "m5stamp-pico",
    "xiao-esp32s3-plus",
    "xiao-esp32s3-plus-sdmmc4",
)
MANIFEST_FILENAME = "airbridge-release.json"
MANIFEST_SCHEMA = 1
READ_CHUNK_BYTES = 256 * 1024


def git_output(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=PROJECT_DIR, text=True
    ).strip()


def source_identity() -> tuple[str, str, int]:
    version = git_output("describe", "--tags", "--always", "--dirty")
    commit = git_output("rev-parse", "HEAD")
    commit_epoch = int(git_output("show", "-s", "--format=%ct", "HEAD"))
    return version, commit, commit_epoch


def filename_component(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-.")
    return sanitized or "unknown"


def manifest_version(source_version: str) -> str:
    release_pattern = (
        r"v\d+\.\d+\.\d+"
        r"(?:-[0-9A-Za-z.-]+)?"
        r"(?:\+[0-9A-Za-z.-]+)?"
    )
    if re.fullmatch(release_pattern, source_version):
        return source_version[1:]
    return source_version


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(READ_CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def validate_esp32_image(path: pathlib.Path) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise RuntimeError(f"firmware image is missing or empty: {path}")
    with path.open("rb") as source:
        if source.read(1) != b"\xe9":
            raise RuntimeError(f"not an ESP32 image: {path}")


def build_firmware(
    pio: str, environment_name: str, source_date_epoch: int, version: str
) -> pathlib.Path:
    build_env = os.environ.copy()
    build_env["AIRBRIDGE_VERSION"] = version
    build_env["SOURCE_DATE_EPOCH"] = str(source_date_epoch)
    subprocess.run(
        [pio, "run", "-e", environment_name],
        cwd=PROJECT_DIR,
        env=build_env,
        check=True,
    )

    firmware_path = (
        PROJECT_DIR / ".pio" / "build" / environment_name / "firmware.bin"
    )
    validate_esp32_image(firmware_path)
    return firmware_path


def package_firmware(
    firmware_path: pathlib.Path,
    output_dir: pathlib.Path,
    environment_name: str,
    version: str,
) -> dict[str, object]:
    stem = "-".join((
        "airbridge",
        filename_component(version),
        filename_component(environment_name),
    ))
    output_path = output_dir / f"{stem}.bin"
    shutil.copyfile(firmware_path, output_path)
    return {
        "raw": {
            "url": output_path.name,
            "size": output_path.stat().st_size,
        }
    }


def write_release_manifest(
    output_dir: pathlib.Path,
    version: str,
    targets: dict[str, dict[str, object]],
) -> pathlib.Path:
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "product": "airbridge",
        "version": manifest_version(version),
        "targets": targets,
    }
    path = output_dir / MANIFEST_FILENAME
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return path


def write_checksums(output_dir: pathlib.Path) -> None:
    artifacts = sorted(
        path for path in output_dir.iterdir()
        if path.is_file() and path.name != "SHA256SUMS"
    )
    lines = [f"{sha256_file(path)}  {path.name}" for path in artifacts]
    (output_dir / "SHA256SUMS").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--env", action="append", dest="environments", metavar="NAME",
        help="PlatformIO environment to build; repeat to select targets",
    )
    parser.add_argument("--pio", default="pio", help="PlatformIO executable")
    parser.add_argument(
        "--output", type=pathlib.Path, default=PROJECT_DIR / "dist",
        help="artifact output directory",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_dir = args.output.resolve()
    version, commit, source_date_epoch = source_identity()
    environments = args.environments or DEFAULT_ENVIRONMENTS

    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    targets = {}
    for environment_name in environments:
        firmware_path = build_firmware(
            args.pio, environment_name, source_date_epoch, version
        )
        targets[environment_name] = package_firmware(
            firmware_path, output_dir, environment_name, version
        )

    write_release_manifest(output_dir, version, targets)
    write_checksums(output_dir)
    print(
        f"packaged {version} ({commit[:12]}) for "
        f"{', '.join(environments)} in {output_dir}"
    )


if __name__ == "__main__":
    main()
