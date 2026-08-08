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
from collections.abc import Mapping
from configparser import ConfigParser
from pathlib import Path


IDF_ENV_VERSION = "1.0.0"
IDF_FRAMEWORK_VERSION = "5.5.3"
DEPENDENCY_CHECK_CACHE_SECONDS = 60 * 60
WATCH_ENVIRONMENTS = {"watch_dev", "watch_prod"}
KNOWN_ENVIRONMENTS = WATCH_ENVIRONMENTS | {"native"}
REVERSIBLE_SDKCONFIG_VALUES = {
    "CONFIG_NVS_ENCRYPTION": "n",
    "CONFIG_SECURE_BOOT": "n",
    "CONFIG_SECURE_FLASH_ENC_ENABLED": "n",
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK": "n",
    "CONFIG_BOOTLOADER_ANTI_ROLLBACK_ENABLE": "n",
    "CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC": "n",
    "CONFIG_SECURE_DISABLE_ROM_DL_MODE": "n",
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE": "n",
}
IRREVERSIBLE_WRITE_DEFINE = "-DCHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES"
IRREVERSIBLE_WRITE_FLAG = "-DCHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0"
PERMANENT_WRITE_POLICY_DEFINE = "-DCHATESP_PERMANENT_WRITE_POLICY"
PERMANENT_WRITE_POLICY_FLAG = "-DCHATESP_PERMANENT_WRITE_POLICY=FORBID"
PROJECT_OVERRIDE_OPTIONS = (
    "-c",
    "--project-conf",
    "-d",
    "--project-dir",
    "-O",
    "--project-option",
)
PROJECT_OVERRIDE_ENVIRONMENT = (
    "PLATFORMIO_BUILD_FLAGS",
    "PLATFORMIO_DEFAULT_ENVS",
    "PLATFORMIO_PROJECT_CONF",
    "PLATFORMIO_PROJECT_DIR",
    "PLATFORMIO_SRC_BUILD_FLAGS",
)
FORBIDDEN_EFUSE_SOURCE_MARKERS = (
    "EFUSE_",
    "efuse_reg.h",
    "esp_efuse_",
    "esp_rom_efuse_",
    "ets_efuse_",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".s"}


def canonical_platformio_environment(
    environment: Mapping[str, str], root: Path, project: Path
) -> dict[str, str]:
    """Return a PlatformIO environment with one physical path spelling."""
    result = dict(environment)
    configured_core = Path(
        result.get("PLATFORMIO_CORE_DIR", str(root / ".platformio"))
    ).expanduser()
    if not configured_core.is_absolute():
        configured_core = root / configured_core
    result["PLATFORMIO_CORE_DIR"] = str(configured_core.resolve())
    result["PWD"] = str(project.resolve())
    result.setdefault("ESP_IDF_VERSION", IDF_FRAMEWORK_VERSION)
    return result


def _contains_path_alias(value: object, root: Path) -> bool:
    if isinstance(value, dict):
        return any(_contains_path_alias(item, root) for item in value.values())
    if isinstance(value, list):
        return any(_contains_path_alias(item, root) for item in value)
    if not isinstance(value, str) or not os.path.isabs(value):
        return False
    canonical_root = root.resolve()
    candidate = Path(value)
    try:
        resolved = candidate.resolve()
    except OSError:
        return False
    if resolved != canonical_root and canonical_root not in resolved.parents:
        return False
    return os.path.normpath(value) != str(resolved)


def remove_aliased_watch_builds(
    project: Path, root: Path, arguments: list[str]
) -> list[str]:
    """Remove generated watch data that contains an aliased project path."""
    root = root.resolve()
    removed: list[str] = []
    for profile in sorted(requested_watch_environments(arguments)):
        build_dir = project / ".pio" / "build" / profile
        description = build_dir / "project_description.json"
        try:
            data = json.loads(description.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if _contains_path_alias(data, root):
            shutil.rmtree(build_dir)
            removed.append(profile)
    return removed


def dependency_policy_digest(root: Path) -> str:
    """Return a digest of each file that controls dependency policy."""
    candidates = [
        root / ".gitmodules",
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


def requested_watch_environments(arguments: list[str]) -> set[str]:
    """Return selected watch profiles, including the PlatformIO default."""
    selected = selected_environments(arguments)
    if not selected:
        return {"watch_dev"}
    return selected & WATCH_ENVIRONMENTS


def invocation_policy_errors(
    arguments: list[str], environment: Mapping[str, str]
) -> list[str]:
    """Reject options that can replace the reviewed watch project policy."""
    errors: list[str] = []
    selected = selected_environments(arguments)
    for profile in sorted(selected - KNOWN_ENVIRONMENTS):
        errors.append(f"unknown PlatformIO environment: {profile}")

    if not requested_watch_environments(arguments):
        return errors

    for argument in arguments:
        for option in PROJECT_OVERRIDE_OPTIONS:
            if argument == option or argument.startswith(f"{option}="):
                errors.append(
                    f"watch builds forbid the project override option: {option}"
                )
                break
    for name in PROJECT_OVERRIDE_ENVIRONMENT:
        if environment.get(name):
            errors.append(
                f"watch builds forbid the project override variable: {name}"
            )
    return errors


def first_party_write_api_errors(project: Path) -> list[str]:
    """Reject direct permanent-write APIs in first-party firmware sources."""
    roots = [project / "main", project / "lib"]
    components = project / "components"
    if components.is_dir():
        roots.extend(
            path for path in components.iterdir()
            if path.is_dir() and path.name.startswith("chatesp")
        )

    errors: list[str] = []
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError):
                errors.append(
                    f"cannot inspect first-party source: {path.relative_to(project)}"
                )
                continue
            for marker in FORBIDDEN_EFUSE_SOURCE_MARKERS:
                if marker in text:
                    errors.append(
                        f"{path.relative_to(project)} uses a forbidden "
                        f"eFuse source marker: {marker}"
                    )
    return errors


def device_write_policy_errors(project: Path) -> list[str]:
    """Report a device profile that can permit an irreversible write."""
    errors: list[str] = []
    parser = ConfigParser(interpolation=None)
    parser.read(project / "platformio.ini", encoding="utf-8")
    for profile in sorted(WATCH_ENVIRONMENTS):
        section = f"env:{profile}"
        flags = parser.get(section, "build_flags", fallback="").split()
        policy_defines = [
            flag for flag in flags
            if flag.startswith(IRREVERSIBLE_WRITE_DEFINE)
        ]
        if policy_defines != [IRREVERSIBLE_WRITE_FLAG]:
            errors.append(
                f"{profile} must set the zero irreversible-write flag exactly once"
            )

        cmake_arguments = parser.get(
            section, "board_build.cmake_extra_args", fallback=""
        ).split()
        cmake_policy_defines = [
            argument for argument in cmake_arguments
            if argument.startswith(PERMANENT_WRITE_POLICY_DEFINE)
        ]
        if cmake_policy_defines != [PERMANENT_WRITE_POLICY_FLAG]:
            errors.append(
                f"{profile} must set the CMake permanent-write lock exactly once"
            )

        values: dict[str, str] = {}
        profile_path = project / "config" / f"{profile}.defaults"
        try:
            lines = profile_path.read_text(encoding="utf-8").splitlines()
        except OSError:
            errors.append(f"{profile} has no SDK configuration overlay")
            continue
        for line in lines:
            if line.startswith("CONFIG_") and "=" in line:
                key, value = line.split("=", 1)
                values[key] = value
        for key, expected in REVERSIBLE_SDKCONFIG_VALUES.items():
            if values.get(key) != expected:
                errors.append(f"{profile} must set {key}={expected}")
    errors.extend(first_party_write_api_errors(project))
    return errors


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

    policy_errors = invocation_policy_errors(sys.argv[1:], os.environ)
    policy_errors.extend(device_write_policy_errors(project))
    if policy_errors:
        print(
            "The irreversible device-write policy failed: "
            + "; ".join(policy_errors),
            file=sys.stderr,
        )
        return 2

    pio = shutil.which("pio")
    if not pio:
        print("PlatformIO is not in the locked uv environment.", file=sys.stderr)
        return 1

    environment = canonical_platformio_environment(
        os.environ, root, project
    )
    core_dir = Path(environment["PLATFORMIO_CORE_DIR"])
    for profile in remove_aliased_watch_builds(
        project, root, sys.argv[1:]
    ):
        print(
            f"Removed {profile} build data that used a non-canonical path."
        )
    verify_dependency_age(root, core_dir)
    if requires_idf_python(sys.argv[1:]):
        prepare_profile_sdkconfigs(project, sys.argv[1:])
        prepare_idf_python(root, core_dir)
    return subprocess.run(
        [pio, *sys.argv[1:]], cwd=project, env=environment, check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
