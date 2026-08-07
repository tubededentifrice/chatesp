#!/usr/bin/env python3
"""Run the locked PlatformIO tool after repository policy checks."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    project = root / "firmware"
    subprocess.run(
        [sys.executable, str(root / "tools" / "check_dependency_age.py")],
        check=True,
        cwd=root,
    )
    if not project.is_dir():
        print("The firmware directory does not exist.", file=sys.stderr)
        return 1

    pio = shutil.which("pio")
    if not pio:
        print("PlatformIO is not in the locked uv environment.", file=sys.stderr)
        return 1

    environment = os.environ.copy()
    environment.setdefault("PLATFORMIO_CORE_DIR", str(root / ".platformio"))
    return subprocess.run(
        [pio, *sys.argv[1:]], cwd=project, env=environment, check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
