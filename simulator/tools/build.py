#!/usr/bin/env python3
"""Build and test the portable ChatESP simulator."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


SIMULATOR_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = SIMULATOR_ROOT.parent
BUILD_ROOT = SIMULATOR_ROOT / ".build"


def compiler() -> str:
    configured = os.environ.get("CXX")
    if configured:
        resolved = shutil.which(configured)
        if resolved:
            return resolved
        raise RuntimeError("CXX does not identify an available compiler")
    for candidate in ("clang++", "g++", "c++"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise RuntimeError("A C++17 compiler is required")


def default_core_root(environment_name: str, directory_name: str, marker: str) -> Path:
    configured = os.environ.get(environment_name)
    candidates = [
        Path(configured).expanduser() if configured else None,
        REPOSITORY_ROOT / "firmware" / "lib" / directory_name,
        SIMULATOR_ROOT / directory_name,
    ]
    for candidate in candidates:
        if candidate is not None and (candidate / "src" / marker).is_file():
            return candidate.resolve()
    raise RuntimeError(
        f"Set {environment_name} to the portable ChatESP {directory_name} library"
    )


def common_sources(
    app_core_root: Path,
    provisioning_core_root: Path,
    ble_core_root: Path,
) -> list[Path]:
    return [
        app_core_root / "src" / "interaction_state.cpp",
        provisioning_core_root / "src" / "provisioning_packet.cpp",
        provisioning_core_root / "src" / "provisioning_transfer.cpp",
        ble_core_root / "src" / "ble_settings.cpp",
        ble_core_root / "src" / "provisioning_session.cpp",
        SIMULATOR_ROOT / "src" / "ble_simulator.cpp",
        SIMULATOR_ROOT / "src" / "simulator.cpp",
        SIMULATOR_ROOT / "src" / "svg_renderer.cpp",
    ]


def compile_target(
    compiler_path: str,
    output: Path,
    entry: Path,
    *,
    app_core_root: Path,
    provisioning_core_root: Path,
    ble_core_root: Path,
    sanitize: bool,
) -> None:
    flags = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-O1" if sanitize else "-O2",
        "-g",
        f"-I{SIMULATOR_ROOT / 'include'}",
        f"-I{app_core_root / 'include'}",
        f"-I{provisioning_core_root / 'include'}",
        f"-I{ble_core_root / 'include'}",
    ]
    if sanitize:
        flags.extend(
            [
                "-fno-omit-frame-pointer",
                "-fsanitize=address,undefined",
            ]
        )
    command = [
        compiler_path,
        *flags,
        *(
            str(source)
            for source in common_sources(
                app_core_root, provisioning_core_root, ble_core_root
            )
        ),
        str(entry),
        "-o",
        str(output),
    ]
    subprocess.run(command, check=True, cwd=REPOSITORY_ROOT)


def run_tests(simulator_binary: Path, test_binary: Path) -> None:
    subprocess.run([str(test_binary)], check=True, cwd=REPOSITORY_ROOT)
    scenarios = sorted((SIMULATOR_ROOT / "scenarios").glob("*.sim"))
    if not scenarios:
        raise RuntimeError("No simulator scenarios are available")
    for scenario in scenarios:
        artifact = BUILD_ROOT / f"{scenario.stem}.svg"
        result = subprocess.run(
            [
                str(simulator_binary),
                "--scenario",
                str(scenario),
                "--render",
                str(artifact),
            ],
            check=True,
            cwd=REPOSITORY_ROOT,
            text=True,
            capture_output=True,
        )
        for private_fixture in ("What is two plus two?", "Four."):
            if private_fixture in result.stdout or private_fixture in result.stderr:
                raise RuntimeError("Simulator command output contains private text")
        records = [json.loads(line) for line in result.stdout.splitlines()]
        if not records or not all(
            record.get("protocol") == 1 and record.get("ok") is True
            for record in records
        ):
            raise RuntimeError(f"Simulator scenario failed: {scenario.name}")
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise RuntimeError("Simulator display artifact was not created")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test", action="store_true", help="Run all tests")
    parser.add_argument(
        "--app-core-dir",
        type=Path,
        help="Path to the portable ChatESP app_core library",
    )
    parser.add_argument(
        "--provisioning-core-dir",
        type=Path,
        help="Path to the portable ChatESP provisioning_core library",
    )
    parser.add_argument(
        "--ble-core-dir",
        type=Path,
        help="Path to the portable ChatESP ble_core library",
    )
    parser.add_argument(
        "--sanitize",
        action="store_true",
        help="Enable address and undefined-behavior sanitizers",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    app_core_root = (
        args.app_core_dir.expanduser().resolve()
        if args.app_core_dir is not None
        else default_core_root(
            "CHATESP_APP_CORE_DIR", "app_core", "interaction_state.cpp"
        )
    )
    provisioning_core_root = (
        args.provisioning_core_dir.expanduser().resolve()
        if args.provisioning_core_dir is not None
        else default_core_root(
            "CHATESP_PROVISIONING_CORE_DIR",
            "provisioning_core",
            "provisioning_packet.cpp",
        )
    )
    ble_core_root = (
        args.ble_core_dir.expanduser().resolve()
        if args.ble_core_dir is not None
        else default_core_root(
            "CHATESP_BLE_CORE_DIR", "ble_core", "provisioning_session.cpp"
        )
    )
    if not (app_core_root / "src" / "interaction_state.cpp").is_file():
        raise RuntimeError("The ChatESP app_core source is not available")
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    compiler_path = compiler()
    simulator_binary = BUILD_ROOT / "chatesp-sim"
    compile_target(
        compiler_path,
        simulator_binary,
        SIMULATOR_ROOT / "src" / "main.cpp",
        app_core_root=app_core_root,
        provisioning_core_root=provisioning_core_root,
        ble_core_root=ble_core_root,
        sanitize=args.sanitize,
    )
    if args.test:
        test_binary = BUILD_ROOT / "chatesp-sim-tests"
        compile_target(
            compiler_path,
            test_binary,
            SIMULATOR_ROOT / "tests" / "test_simulator.cpp",
            app_core_root=app_core_root,
            provisioning_core_root=provisioning_core_root,
            ble_core_root=ble_core_root,
            sanitize=args.sanitize,
        )
        run_tests(simulator_binary, test_binary)
    print(simulator_binary)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Simulator build failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
