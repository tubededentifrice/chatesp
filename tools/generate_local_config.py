from __future__ import annotations

import argparse
import os
import re
import stat
import tempfile
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit


EXPECTED_NAMES = (
    "CHAT_ENDPOINT",
    "OPENROUTER_API_KEY",
    "BRAVE_API_KEY",
    "WIFI_SSID",
    "WIFI_PASSWORD",
)
MAX_SOURCE_BYTES = 4096


class ConfigError(ValueError):
    """A safe error that does not contain a setting value."""


@dataclass(frozen=True)
class Limits:
    minimum: int
    maximum: int
    ascii_only: bool


LIMITS = {
    "CHAT_ENDPOINT": Limits(12, 192, True),
    "OPENROUTER_API_KEY": Limits(8, 256, True),
    "BRAVE_API_KEY": Limits(0, 128, True),
    "WIFI_SSID": Limits(1, 32, False),
    "WIFI_PASSWORD": Limits(8, 63, False),
}


def _parse_line(line: str, line_number: int) -> tuple[str, str]:
    if "=" not in line:
        raise ConfigError(f"line {line_number} has invalid syntax")
    name, value = line.split("=", 1)
    if name not in EXPECTED_NAMES:
        raise ConfigError(f"line {line_number} has an unknown setting name")
    return name, value


def parse_environment(text: str) -> dict[str, str]:
    settings: dict[str, str] = {}
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not line or line.startswith("#"):
            continue
        name, value = _parse_line(line, line_number)
        if name in settings:
            raise ConfigError(f"{name} is repeated")
        settings[name] = value

    missing = [name for name in EXPECTED_NAMES if name not in settings]
    if missing:
        raise ConfigError(f"{missing[0]} is missing")
    return settings


def _validate_text(name: str, value: str) -> None:
    limits = LIMITS[name]
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ConfigError(f"{name} is not valid UTF-8") from error

    if not limits.minimum <= len(encoded) <= limits.maximum:
        raise ConfigError(f"{name} has an invalid byte length")

    if any(unicodedata.category(character).startswith("C") for character in value):
        raise ConfigError(f"{name} contains a control character")

    if limits.ascii_only and any(byte < 0x20 or byte > 0x7E for byte in encoded):
        raise ConfigError(f"{name} must contain visible ASCII only")


def _validate_endpoint(value: str) -> None:
    try:
        endpoint = urlsplit(value)
        port = endpoint.port
    except ValueError as error:
        raise ConfigError("CHAT_ENDPOINT is not a valid HTTPS endpoint") from error

    hostname = endpoint.hostname or ""
    labels = hostname.split(".")
    valid_hostname = (
        len(hostname) <= 253
        and len(labels) >= 2
        and all(
            1 <= len(label) <= 63
            and re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?", label)
            for label in labels
        )
    )
    if (
        endpoint.scheme != "https"
        or not valid_hostname
        or any(character.isspace() for character in value)
        or "\\" in value
        or endpoint.username is not None
        or endpoint.password is not None
        or endpoint.query
        or endpoint.fragment
        or port is not None and not 1 <= port <= 65535
    ):
        raise ConfigError("CHAT_ENDPOINT is not a valid HTTPS endpoint")


def validate_settings(settings: dict[str, str]) -> None:
    if tuple(settings.keys()) != EXPECTED_NAMES:
        if set(settings) != set(EXPECTED_NAMES):
            raise ConfigError("the setting names are not complete")
    for name in EXPECTED_NAMES:
        _validate_text(name, settings[name])
    _validate_endpoint(settings["CHAT_ENDPOINT"])


def c_string_literal(value: str) -> str:
    escaped: list[str] = ['"']
    for byte in value.encode("utf-8"):
        if byte == ord('"'):
            escaped.append(r'\"')
        elif byte == ord("\\"):
            escaped.append(r"\\")
        elif 0x20 <= byte <= 0x7E:
            escaped.append(chr(byte))
        else:
            escaped.append(f"\\{byte:03o}")
    escaped.append('"')
    return "".join(escaped)


def render_header(settings: dict[str, str]) -> str:
    lines = [
        "// Generated local development settings. Do not commit this file.",
        "#pragma once",
        "",
    ]
    for name in EXPECTED_NAMES:
        lines.append(f"#define CHATESP_LOCAL_{name} {c_string_literal(settings[name])}")
    lines.append("")
    return "\n".join(lines)


def _read_settings(source: Path) -> dict[str, str]:
    try:
        with source.open("rb") as settings_file:
            encoded = settings_file.read(MAX_SOURCE_BYTES + 1)
    except OSError as error:
        raise ConfigError("the settings file cannot be read") from error
    if len(encoded) > MAX_SOURCE_BYTES:
        raise ConfigError("the settings file is too large")
    try:
        text = encoded.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ConfigError("the settings file is not valid UTF-8") from error
    settings = parse_environment(text)
    validate_settings(settings)
    return settings


def write_if_changed(destination: Path, content: str) -> bool:
    encoded = content.encode("utf-8")
    try:
        if destination.is_symlink():
            raise ConfigError("the local header path must not be a symbolic link")
        if destination.is_file() and destination.read_bytes() == encoded:
            destination.chmod(stat.S_IRUSR | stat.S_IWUSR)
            return False
        destination.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{destination.name}.",
            dir=destination.parent,
        )
        try:
            os.fchmod(descriptor, stat.S_IRUSR | stat.S_IWUSR)
            with os.fdopen(descriptor, "wb") as temporary_file:
                descriptor = -1
                temporary_file.write(encoded)
                temporary_file.flush()
                os.fsync(temporary_file.fileno())
            os.replace(temporary_name, destination)
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
    except ConfigError:
        raise
    except OSError as error:
        raise ConfigError("the local header cannot be written") from error
    return True


def generate(source: Path, destination: Path) -> bool:
    return write_if_changed(destination, render_header(_read_settings(source)))


def main() -> int:
    repository = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Generate the ignored firmware development settings header."
    )
    parser.add_argument("--source", type=Path, default=repository / ".secrets/device.env")
    parser.add_argument(
        "--destination",
        type=Path,
        default=repository / "firmware/main/local_config.h",
    )
    arguments = parser.parse_args()
    try:
        changed = generate(arguments.source, arguments.destination)
    except ConfigError as error:
        parser.exit(1, f"Local configuration error: {error}\n")
    print("Local development header updated." if changed else "Local development header is current.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
