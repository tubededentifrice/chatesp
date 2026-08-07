from __future__ import annotations

import io
import os
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest.mock import patch

from tools.generate_local_config import (
    ConfigError,
    c_string_literal,
    generate,
    main,
    parse_environment,
    render_header,
    validate_settings,
)


NAMES = (
    "CHAT_ENDPOINT",
    "OPENROUTER_API_KEY",
    "BRAVE_API_KEY",
    "WIFI_SSID",
    "WIFI_PASSWORD",
)


def dummy_settings() -> dict[str, str]:
    return {
        NAMES[0]: "https://service.example/v1",
        NAMES[1]: "router-token-placeholder",
        NAMES[2]: "search-token-placeholder",
        NAMES[3]: "Test Network",
        NAMES[4]: "password-placeholder",
    }


def environment_text(settings: dict[str, str]) -> str:
    return "".join(f"{name}={settings[name]}\n" for name in NAMES)


class GenerateLocalConfigTests(unittest.TestCase):
    def test_generates_escaped_private_header(self) -> None:
        settings = dummy_settings()
        settings[NAMES[3]] = "Caf\N{LATIN SMALL LETTER E WITH ACUTE}"
        settings[NAMES[4]] = 'long "pass" \\ value'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "device.env"
            destination = root / "local_config.h"
            source.write_text(environment_text(settings), encoding="utf-8")

            self.assertTrue(generate(source, destination))

            header = destination.read_text(encoding="utf-8")
            self.assertIn(r'CHATESP_LOCAL_WIFI_SSID "Caf\303\251"', header)
            self.assertIn(r'CHATESP_LOCAL_WIFI_PASSWORD "long \"pass\" \\ value"', header)
            self.assertEqual(0o600, destination.stat().st_mode & 0o777)
            self.assertNotIn(str(root), header)

    def test_does_not_rewrite_current_header(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "device.env"
            destination = root / "local_config.h"
            source.write_text(environment_text(dummy_settings()), encoding="utf-8")
            self.assertTrue(generate(source, destination))
            first_stat = destination.stat()

            self.assertFalse(generate(source, destination))

            second_stat = destination.stat()
            self.assertEqual(first_stat.st_ino, second_stat.st_ino)
            self.assertEqual(first_stat.st_mtime_ns, second_stat.st_mtime_ns)

    def test_repairs_permissions_without_rewriting_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "device.env"
            destination = root / "local_config.h"
            source.write_text(environment_text(dummy_settings()), encoding="utf-8")
            self.assertTrue(generate(source, destination))
            first_stat = destination.stat()
            destination.chmod(0o644)

            self.assertFalse(generate(source, destination))

            self.assertEqual(first_stat.st_ino, destination.stat().st_ino)
            self.assertEqual(0o600, destination.stat().st_mode & 0o777)

    def test_refuses_symbolic_link_destination(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "device.env"
            target = root / "target.h"
            destination = root / "local_config.h"
            source.write_text(environment_text(dummy_settings()), encoding="utf-8")
            target.write_text("unchanged", encoding="utf-8")
            destination.symlink_to(target)

            with self.assertRaisesRegex(ConfigError, "symbolic link"):
                generate(source, destination)

            self.assertEqual("unchanged", target.read_text(encoding="utf-8"))

    def test_requires_each_exact_name_once(self) -> None:
        valid = environment_text(dummy_settings())
        with self.assertRaisesRegex(ConfigError, "is missing"):
            parse_environment(valid.replace(f"{NAMES[2]}=search-token-placeholder\n", ""))
        with self.assertRaisesRegex(ConfigError, "is repeated"):
            parse_environment(valid + f"{NAMES[2]}=second-placeholder\n")
        with self.assertRaisesRegex(ConfigError, "unknown setting name"):
            parse_environment(valid + "EXTRA_SETTING=value\n")

    def test_rejects_invalid_lengths_controls_and_endpoint(self) -> None:
        cases = (
            (NAMES[0], "http://service.example/v1"),
            (NAMES[0], "https://user@service.example/v1"),
            (NAMES[0], "https://service.example/v1?private=yes"),
            (NAMES[0], "https://invalid-.example/v1"),
            (NAMES[0], "https://service.example/bad path"),
            (NAMES[0], "https://service.example\\bad"),
            (NAMES[1], "short"),
            (NAMES[2], "x" * 129),
            (NAMES[3], "x" * 33),
            (NAMES[4], "short"),
            (NAMES[4], "valid-length\x00bad"),
        )
        for name, value in cases:
            with self.subTest(name=name):
                settings = dummy_settings()
                settings[name] = value
                with self.assertRaises(ConfigError) as raised:
                    validate_settings(settings)
                self.assertNotIn(value, str(raised.exception))

    def test_accepts_empty_optional_search_key(self) -> None:
        settings = dummy_settings()
        settings[NAMES[2]] = ""
        validate_settings(settings)

    def test_errors_do_not_echo_values(self) -> None:
        private_value = "private-control-value\x01"
        settings = dummy_settings()
        settings[NAMES[1]] = private_value
        with self.assertRaises(ConfigError) as raised:
            validate_settings(settings)
        self.assertNotIn(private_value, str(raised.exception))

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "device.env"
            destination = root / "local_config.h"
            source.write_text(environment_text(settings), encoding="utf-8")
            stderr = io.StringIO()
            with patch(
                "sys.argv",
                [
                    "generate_local_config.py",
                    "--source",
                    os.fspath(source),
                    "--destination",
                    os.fspath(destination),
                ],
            ), redirect_stderr(stderr), self.assertRaises(SystemExit) as exited:
                main()
            self.assertEqual(1, exited.exception.code)
            self.assertNotIn(private_value, stderr.getvalue())

    def test_rejects_an_oversize_source_without_echoing_content(self) -> None:
        private_value = "private-oversize-value"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "device.env"
            destination = root / "local_config.h"
            source.write_text(private_value * 300, encoding="utf-8")
            with self.assertRaises(ConfigError) as raised:
                generate(source, destination)
            self.assertNotIn(private_value, str(raised.exception))

    def test_production_object_excludes_development_values(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("A C++ compiler is not available")

        repository = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("device_settings.cpp", "device_settings.hpp"):
                shutil.copyfile(repository / "firmware/main" / name, root / name)
            settings = dummy_settings()
            (root / "local_config.h").write_text(
                render_header(settings),
                encoding="utf-8",
            )
            production_object = root / "production.o"
            development_object = root / "development.o"
            common = [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{root}",
                "-c",
                os.fspath(root / "device_settings.cpp"),
            ]
            subprocess.run(
                common
                + ["-DCHATESP_DEVELOPMENT_MODE=0", "-o", os.fspath(production_object)],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                common
                + ["-DCHATESP_DEVELOPMENT_MODE=1", "-o", os.fspath(development_object)],
                check=True,
                capture_output=True,
            )
            production = production_object.read_bytes()
            development = development_object.read_bytes()
            for value in settings.values():
                if value:
                    encoded = value.encode("utf-8")
                    self.assertNotIn(encoded, production)
                    self.assertIn(encoded, development)

    def test_development_build_is_unconfigured_without_local_header(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("A C++ compiler is not available")

        repository = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("device_settings.cpp", "device_settings.hpp"):
                shutil.copyfile(repository / "firmware/main" / name, root / name)
            main_source = root / "main.cpp"
            main_source.write_text(
                '#include "device_settings.hpp"\n'
                "int main() {\n"
                "    const auto settings = chatesp::device_settings();\n"
                "    return settings.configured ? 1 : 0;\n"
                "}\n",
                encoding="utf-8",
            )
            executable = root / "settings-check"
            subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DCHATESP_DEVELOPMENT_MODE=1",
                    f"-I{root}",
                    os.fspath(root / "device_settings.cpp"),
                    os.fspath(main_source),
                    "-o",
                    os.fspath(executable),
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run([executable], check=True, capture_output=True)

    def test_c_literal_uses_fixed_octets(self) -> None:
        self.assertEqual(r'"A\001f\303\251"', c_string_literal("A\x01f\N{LATIN SMALL LETTER E WITH ACUTE}"))


if __name__ == "__main__":
    unittest.main()
