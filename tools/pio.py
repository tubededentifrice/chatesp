#!/usr/bin/env python3
"""Run the locked PlatformIO tool after repository policy checks."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import venv
from pathlib import Path


IDF_ENV_VERSION = "1.0.0"
IDF_FRAMEWORK_VERSION = "5.5.3"


def selected_environments(arguments: list[str]) -> set[str]:
    selected: set[str] = set()
    for index, argument in enumerate(arguments):
        if argument in ("-e", "--environment") and index + 1 < len(arguments):
            selected.add(arguments[index + 1])
        elif argument.startswith("--environment="):
            selected.add(argument.split("=", 1)[1])
    return selected


def requires_idf_python(arguments: list[str]) -> bool:
    selected = selected_environments(arguments)
    return not selected or bool(selected & {"watch_dev", "watch_prod"})


def prepare_idf_python(root: Path, core_dir: Path) -> None:
    requirements = root / "tools" / "espidf-python-requirements.txt"
    lock_digest = hashlib.sha256(requirements.read_bytes()).hexdigest()
    environment = core_dir / "penv" / f".espidf-{IDF_FRAMEWORK_VERSION}"
    scripts = "Scripts" if os.name == "nt" else "bin"
    python = environment / scripts / ("python.exe" if os.name == "nt" else "python")
    metadata_path = environment / "pio-idf-venv.json"
    expected_python_version = (
        f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}-"
        f"{sys.version_info.releaselevel}.{sys.version_info.serial}"
    )

    metadata: dict[str, str] = {}
    if metadata_path.exists():
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            metadata = {}
    if (
        python.is_file()
        and metadata.get("version") == IDF_ENV_VERSION
        and metadata.get("python_version") == expected_python_version
        and metadata.get("requirements_sha256") == lock_digest
    ):
        return

    environment.parent.mkdir(parents=True, exist_ok=True)
    venv.EnvBuilder(with_pip=True, clear=True).create(environment)
    subprocess.run(
        [
            "uv",
            "pip",
            "install",
            "--python",
            str(python),
            "--require-hashes",
            "--no-deps",
            "--requirement",
            str(requirements),
        ],
        check=True,
        cwd=root,
    )
    metadata_path.write_text(
        json.dumps(
            {
                "version": IDF_ENV_VERSION,
                "python_version": expected_python_version,
                "requirements_sha256": lock_digest,
            },
            indent=2,
        ),
        encoding="utf-8",
    )


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
    environment.setdefault("ESP_IDF_VERSION", IDF_FRAMEWORK_VERSION)
    core_dir = Path(environment["PLATFORMIO_CORE_DIR"])
    if requires_idf_python(sys.argv[1:]):
        prepare_idf_python(root, core_dir)
    return subprocess.run(
        [pio, *sys.argv[1:]], cwd=project, env=environment, check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
