#!/usr/bin/env python3
"""Configure local iOS signing, then build and install the companion app."""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "ios" / "ChatESP.xcodeproj" / "project.pbxproj"
LOCAL_CONFIG = ROOT / "ios" / "Local.xcconfig"
DERIVED_DATA = ROOT / ".build" / "ios-device"
BUNDLE_ID = "org.chatesp.companion"
TEAM_ASSIGNMENT = re.compile(
    r"(?m)^[ \t]*DEVELOPMENT_TEAM\s*=\s*(?P<team>[A-Z0-9]{10})\s*;[ \t]*\n?"
)
LOCAL_TEAM_ASSIGNMENT = re.compile(
    r"(?m)^[ \t]*DEVELOPMENT_TEAM\s*=\s*(?P<team>[A-Z0-9]{10})[ \t]*$"
)
HOME_PATH = re.compile(r"(?:/Users|/home)/[^/\s]+/")
EMAIL_ADDRESS = re.compile(
    r"\b[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
    r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
    r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+\b"
)
APPLE_CERTIFICATE = re.compile(
    r"(?m)Apple (?:Development|Distribution):[^\n]+"
)
DEVICE_IDENTIFIER = re.compile(
    r"\b(?:[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}"
    r"|[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}"
    r"|[0-9A-Fa-f]{24,40})\b"
)


class IOSToolError(RuntimeError):
    """Report a safe, useful iOS tool failure."""


@dataclass(frozen=True)
class IOSDevice:
    identifier: str
    name: str


def project_team_ids(text: str) -> set[str]:
    """Return each non-empty Apple Team ID stored in an Xcode project."""
    return {match.group("team") for match in TEAM_ASSIGNMENT.finditer(text)}


def local_team_id(config: Path = LOCAL_CONFIG) -> str:
    """Read one Apple Team ID from the ignored local Xcode configuration."""
    try:
        text = config.read_text(encoding="utf-8")
    except FileNotFoundError as error:
        raise IOSToolError(
            "Local iOS signing is not configured. Run configure-signing first."
        ) from error
    teams = [
        match.group("team") for match in LOCAL_TEAM_ASSIGNMENT.finditer(text)
    ]
    if len(teams) != 1:
        raise IOSToolError(
            "The local iOS signing file must contain one valid development team."
        )
    return teams[0]


def configure_signing(
    project: Path = PROJECT, config: Path = LOCAL_CONFIG
) -> bool:
    """Move one project Team ID to the ignored local Xcode configuration."""
    text = project.read_text(encoding="utf-8")
    teams = project_team_ids(text)
    if len(teams) > 1:
        raise IOSToolError(
            "The Xcode project contains more than one development team."
        )
    if not teams:
        local_team_id(config)
        os.chmod(config, 0o600)
        return False

    team = teams.pop()
    if config.exists() and local_team_id(config) != team:
        raise IOSToolError(
            "The project and local signing file contain different development teams."
        )

    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text(
        "// Local iOS signing data. Git ignores this file.\n"
        f"DEVELOPMENT_TEAM = {team}\n"
        "CODE_SIGN_STYLE = Automatic\n",
        encoding="utf-8",
    )
    os.chmod(config, 0o600)

    scrubbed = TEAM_ASSIGNMENT.sub("", text)
    if scrubbed == text:
        raise IOSToolError("The tool could not remove the project development team.")
    project.write_text(scrubbed, encoding="utf-8")
    return True


def available_ios_devices(data: object) -> list[IOSDevice]:
    """Select available physical iOS devices from xcdevice JSON data."""
    if not isinstance(data, list):
        raise IOSToolError("xcdevice returned an invalid device list.")
    devices: list[IOSDevice] = []
    for item in data:
        if not isinstance(item, dict):
            continue
        if (
            item.get("platform") != "com.apple.platform.iphoneos"
            or item.get("simulator") is True
            or item.get("available") is not True
            or item.get("ignored") is True
        ):
            continue
        identifier = item.get("identifier")
        name = item.get("name")
        if isinstance(identifier, str) and identifier and isinstance(name, str):
            devices.append(IOSDevice(identifier=identifier, name=name))
    return devices


def discover_device(selector: str | None = None) -> IOSDevice:
    """Return one available physical iPhone without printing its identifier."""
    try:
        result = subprocess.run(
            ["xcrun", "xcdevice", "list", "--timeout", "10"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            timeout=20,
        )
        devices = available_ios_devices(json.loads(result.stdout))
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise IOSToolError("The tool could not list local Apple devices.") from error
    except json.JSONDecodeError as error:
        raise IOSToolError("xcdevice returned invalid JSON.") from error

    if selector is not None:
        matches = [
            device
            for device in devices
            if selector in {device.identifier, device.name}
        ]
        if len(matches) != 1:
            raise IOSToolError("The requested iPhone is not uniquely available.")
        return matches[0]
    if not devices:
        raise IOSToolError(
            "Connect and unlock one iPhone with Developer Mode enabled."
        )
    if len(devices) > 1:
        raise IOSToolError(
            "More than one iPhone is available. Use --device to select one."
        )
    return devices[0]


def sanitized_output(text: str, private_values: Sequence[str]) -> str:
    """Remove local signing, device, and home-directory values from output."""
    safe = text
    for value in sorted(set(private_values), key=len, reverse=True):
        if value:
            safe = safe.replace(value, "<local-value>")
    safe = HOME_PATH.sub("<local-home>/", safe)
    safe = EMAIL_ADDRESS.sub("<local-email>", safe)
    safe = APPLE_CERTIFICATE.sub("Apple signing certificate: <local-value>", safe)
    return DEVICE_IDENTIFIER.sub("<local-device>", safe)


def run_private(
    command: list[str],
    *,
    private_values: Sequence[str],
    timeout: int,
    action: str,
) -> None:
    """Run a command and show sanitized details only when it fails."""
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise IOSToolError(f"{action} did not complete.") from error
    if result.returncode == 0:
        return
    details = sanitized_output(
        "\n".join(part for part in (result.stdout, result.stderr) if part),
        private_values,
    ).strip()
    message = f"{action} failed."
    if details:
        message += f"\n{details}"
    raise IOSToolError(message)


def built_app(configuration: str) -> Path:
    """Return the expected app bundle for one device build configuration."""
    return (
        DERIVED_DATA
        / "Build"
        / "Products"
        / f"{configuration}-iphoneos"
        / "ChatESP.app"
    )


def build_for_device(device: IOSDevice, configuration: str) -> Path:
    """Build and verify one signed app for the selected physical iPhone."""
    team = local_team_id()
    private = [team, device.identifier, device.name]
    DERIVED_DATA.mkdir(parents=True, exist_ok=True)
    print("Building the signed iOS app...", flush=True)
    run_private(
        [
            "xcodebuild",
            "-project",
            "ios/ChatESP.xcodeproj",
            "-scheme",
            "ChatESP",
            "-configuration",
            configuration,
            "-destination",
            f"platform=iOS,id={device.identifier}",
            "-destination-timeout",
            "30",
            "-derivedDataPath",
            str(DERIVED_DATA),
            "-xcconfig",
            str(LOCAL_CONFIG),
            "-allowProvisioningUpdates",
            "-allowProvisioningDeviceRegistration",
            "-quiet",
            "build",
        ],
        private_values=private,
        timeout=600,
        action="The signed iOS build",
    )
    app = built_app(configuration)
    info_path = app / "Info.plist"
    if not info_path.is_file():
        raise IOSToolError("The signed build did not create the app bundle.")
    try:
        with info_path.open("rb") as stream:
            info = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException) as error:
        raise IOSToolError("The built app has an invalid property list.") from error
    if info.get("CFBundleIdentifier") != BUNDLE_ID:
        raise IOSToolError("The built app has an unexpected bundle identifier.")
    run_private(
        ["codesign", "--verify", "--deep", "--strict", str(app)],
        private_values=private,
        timeout=30,
        action="The app signature check",
    )
    print("Signed iOS build passed.", flush=True)
    return app


def install_app(
    device: IOSDevice, app: Path, configuration: str, launch: bool
) -> None:
    """Install one app bundle and optionally launch it on the selected iPhone."""
    team = local_team_id()
    private = [team, device.identifier, device.name]
    print("Installing the iOS app...", flush=True)
    run_private(
        [
            "xcrun",
            "devicectl",
            "device",
            "install",
            "app",
            "--device",
            device.identifier,
            "--quiet",
            "--timeout",
            "120",
            str(app),
        ],
        private_values=private,
        timeout=150,
        action="The iOS app installation",
    )
    print(f"{configuration} app installation passed.", flush=True)
    if not launch:
        return
    print("Launching the iOS app...", flush=True)
    run_private(
        [
            "xcrun",
            "devicectl",
            "device",
            "process",
            "launch",
            "--device",
            device.identifier,
            "--terminate-existing",
            "--quiet",
            "--timeout",
            "60",
            BUNDLE_ID,
        ],
        private_values=private,
        timeout=90,
        action="The iOS app launch",
    )
    print("iOS app launch passed.", flush=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Configure, build, and install the ChatESP iOS app."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser(
        "configure-signing",
        help="move a project Team ID to the ignored local configuration",
    )
    install = subparsers.add_parser(
        "install", help="build and install the latest app on a physical iPhone"
    )
    install.add_argument(
        "--configuration",
        choices=("Debug", "Release"),
        default="Debug",
    )
    install.add_argument(
        "--device",
        help="local iPhone name or identifier when more than one is available",
    )
    install.add_argument(
        "--launch",
        action="store_true",
        help="launch the app after installation",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "configure-signing":
            moved = configure_signing()
            if moved:
                print(
                    "Saved local iOS signing and removed it from the tracked project."
                )
            else:
                print("Local iOS signing is already configured.")
            return 0
        device = discover_device(arguments.device)
        app = build_for_device(device, arguments.configuration)
        install_app(device, app, arguments.configuration, arguments.launch)
        return 0
    except IOSToolError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
