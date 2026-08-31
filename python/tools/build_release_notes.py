#!/usr/bin/env python3
"""Create concise release notes from commits since the previous version tag."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess


PROJECT_DIR = pathlib.Path(__file__).resolve().parents[2]
VERSION_TAG = re.compile(r"^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=PROJECT_DIR, text=True
    ).strip()


def previous_tag(tag: str) -> str | None:
    tags = [
        value for value in git("tag", "--merged", f"{tag}^{{commit}}").splitlines()
        if value != tag and VERSION_TAG.fullmatch(value)
    ]
    if not tags:
        return None
    return git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", f"{tag}^")


def build_notes(tag: str) -> str:
    if not VERSION_TAG.fullmatch(tag):
        raise ValueError(f"invalid release tag: {tag}")
    previous = previous_tag(tag)
    revision = f"{previous}..{tag}" if previous else tag
    subjects = [
        value.strip()
        for value in git("log", "--no-merges", "--reverse", "--format=%s", revision).splitlines()
        if value.strip() and value.strip().casefold() != "sync"
    ]
    heading = f"Changes since {previous}" if previous else "Changes"
    body = "\n".join(f"- {subject}" for subject in subjects)
    return f"## {heading}\n\n{body or '- Initial release'}\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.write_text(build_notes(args.tag), encoding="utf-8")


if __name__ == "__main__":
    main()
