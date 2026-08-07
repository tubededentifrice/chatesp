#!/usr/bin/env python3
"""Run the locked PlatformIO tool after repository policy checks."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import venv
from pathlib import Path


IDF_ENV_VERSION = "1.0.0"
IDF_FRAMEWORK_VERSION = "5.5.3"
DEPENDENCY_CHECK_CACHE_SECONDS = 60 * 60
WATCH_ENVIRONMENTS = {"watch_dev", "watch_prod"}
PRODUCTION_EFUSE_APPROVAL = "CHATESP_ALLOW_PRODUCTION_EFUSE_PROVISION"


def dependency_policy_digest(root: Path) -> str:
    """Return a digest of each file that controls dependency policy."""
    candidates = [
        root / "pyproject.toml",
        root / "uv.lock",
        root / "firmware" / "platformio.ini",
        root / "firmware" / "sdkconfig.defaults",
        root / "firmware" / "dependencies.lock",
        root / "tools" / "check_dependency_age.py",
        root / "tools" / "espidf-python-requirements.txt",
    ]
    candidates.extend(sorted((root / ".github" / "workflows").glob("*.yml")))
    candidates.extend(sorted((root / ".github" / "workflows").glob("*.yaml")))
    candidates.append(root / "firmware" / "main" / "idf_component.yml")
    candidates.extend(sorted((root / "firmware" / "config").glob("*.defaults")))
    candidates.extend(
        sorted((root / "firmware" / "components").glob("*/idf_component.yml"))
    )

    digest = hashlib.sha256()
    for path in candidates:
        if not path.is_file():
            continue
        digest.update(path.relative_to(root).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def dependency_check_is_fresh(
    cache_path: Path, policy_digest: str, now: float | None = None
) -> bool:
    """Accept only a recent successful check for the same policy inputs."""
    try:
        record = json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    checked_at = record.get("checked_at")
    if not isinstance(checked_at, (int, float)):
        return False
    age = (time.time() if now is None else now) - checked_at
    return (
        record.get("policy_sha256") == policy_digest
        and 0 <= age <= DEPENDENCY_CHECK_CACHE_SECONDS
    )


def verify_dependency_age(root: Path, core_dir: Path) -> None:
    """Run the online age check at most once per unchanged gate batch."""
    cache_path = core_dir / "dependency-age-check.json"
    policy_digest = dependency_policy_digest(root)
    if dependency_check_is_fresh(cache_path, policy_digest):
        print("Dependency age check reused a recent verified result.")
        return
    subprocess.run(
        [sys.executable, str(root / "tools" / "check_dependency_age.py")],
        check=True,
        cwd=root,
    )
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(
        json.dumps(
            {
                "checked_at": time.time(),
                "policy_sha256": policy_digest,
            },
            indent=2,
        ),
        encoding="utf-8",
    )


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
    return not selected or bool(selected & WATCH_ENVIRONMENTS)


def selected_targets(arguments: list[str]) -> set[str]:
    """Return each PlatformIO target from the command line."""
    selected: set[str] = set()
    for index, argument in enumerate(arguments):
        if argument in ("-t", "--target") and index + 1 < len(arguments):
            selected.add(arguments[index + 1])
        elif argument.startswith("--target="):
            selected.add(argument.split("=", 1)[1])
    return selected


def requested_watch_environments(arguments: list[str]) -> set[str]:
    """Return selected watch profiles, including the PlatformIO default."""
    selected = selected_environments(arguments)
    if not selected:
        return {"watch_dev"}
    return selected & WATCH_ENVIRONMENTS


def production_upload_is_allowed(
    arguments: list[str], environment: dict[str, str]
) -> bool:
    """Require explicit consent before an upload can create an eFuse key."""
    is_production_upload = (
        "watch_prod" in requested_watch_environments(arguments)
        and "upload" in selected_targets(arguments)
    )
    return not is_production_upload or environment.get(
        PRODUCTION_EFUSE_APPROVAL
    ) == "1"


def profile_sdkconfig_text(project: Path, profile: str) -> str:
    """Create one deterministic SDK configuration seed for a watch profile."""
    common = (project / "sdkconfig.defaults").read_text(encoding="utf-8")
    profile_path = project / "config" / f"{profile}.defaults"
    overlay = profile_path.read_text(encoding="utf-8")
    return (
        "# Generated by tools/pio.py. Do not edit.\n"
        f"# Source: sdkconfig.defaults and config/{profile}.defaults\n\n"
        f"{common.rstrip()}\n\n{overlay.rstrip()}\n"
    )


def prepare_profile_sdkconfigs(project: Path, arguments: list[str]) -> None:
    """Replace stale ignored profile settings before each watch command."""
    for profile in sorted(requested_watch_environments(arguments)):
        destination = project / f"sdkconfig.{profile}"
        expected = profile_sdkconfig_text(project, profile)
        try:
            current = destination.read_text(encoding="utf-8")
        except OSError:
            current = ""
        if current != expected:
            destination.write_text(expected, encoding="utf-8")


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
    if not project.is_dir():
        print("The firmware directory does not exist.", file=sys.stderr)
        return 1

    pio = shutil.which("pio")
    if not pio:
        print("PlatformIO is not in the locked uv environment.", file=sys.stderr)
        return 1

    environment = os.environ.copy()
    if not production_upload_is_allowed(sys.argv[1:], environment):
        print(
            "Production upload is blocked because its first start can write "
            "an irreversible HMAC key to eFuse. Set "
            f"{PRODUCTION_EFUSE_APPROVAL}=1 only after explicit approval.",
            file=sys.stderr,
        )
        return 2
    environment.setdefault("PLATFORMIO_CORE_DIR", str(root / ".platformio"))
    environment.setdefault("ESP_IDF_VERSION", IDF_FRAMEWORK_VERSION)
    core_dir = Path(environment["PLATFORMIO_CORE_DIR"])
    verify_dependency_age(root, core_dir)
    if requires_idf_python(sys.argv[1:]):
        prepare_profile_sdkconfigs(project, sys.argv[1:])
        prepare_idf_python(root, core_dir)
    return subprocess.run(
        [pio, *sys.argv[1:]], cwd=project, env=environment, check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
