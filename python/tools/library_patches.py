"""Apply ordered patches to generated library sources, never upstream files.

Use plain unified diffs: Git-format headers can be skipped when applying from
a subdirectory of another repository, even with --no-index.
"""

from pathlib import Path
import subprocess


def patch_files(directory: Path) -> list[Path]:
    return sorted(directory.glob("*.patch"))


def apply_patches(source_dir: Path, patches: list[Path]) -> None:
    for patch in patches:
        subprocess.run(
            ["git", "apply", "--no-index", str(patch.resolve())],
            cwd=source_dir,
            check=True,
        )
