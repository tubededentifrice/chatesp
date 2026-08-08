from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.check_secrets import (
    MAX_FILE_SIZE,
    candidate_blobs,
    forbidden_path,
    scan_candidates,
    scan_paths,
    scan_text,
)


class SecretScanTests(unittest.TestCase):
    def test_accepts_empty_example_values(self) -> None:
        self.assertEqual([], scan_text(Path(".env.example"), "API_KEY=\nPASSWORD=\n"))

    def test_detects_openrouter_shape(self) -> None:
        key = "sk-or-v1-" + "x" * 48
        findings = scan_text(Path("config.txt"), f"value={key}\n")
        self.assertEqual("OpenRouter API key", findings[0].reason)

    def test_detects_generic_secret_assignment(self) -> None:
        findings = scan_text(Path("config.txt"), "BRAVE_API_KEY=not-a-real-value\n")
        self.assertTrue(findings)

    def test_detects_quoted_code_secret(self) -> None:
        source = "let pass" + 'word = "' + "not-a-real-value" + '"\n'
        findings = scan_text(Path("Config.swift"), source)
        self.assertTrue(findings)

    def test_detects_personal_home_path(self) -> None:
        home_path = "/" + "Users/example/work/chatesp\n"
        findings = scan_text(Path("notes.md"), home_path)
        self.assertTrue(findings)

    def test_rejects_secret_paths(self) -> None:
        self.assertTrue(forbidden_path(Path(".secrets/device.env")))
        self.assertTrue(forbidden_path(Path("ios/AppSecrets.swift")))

    def test_skips_binary_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "asset.bin"
            binary.write_bytes(b"\x00sk-or-v1-" + b"x" * 48)
            self.assertEqual([], scan_paths(root, [binary]))

    def test_scans_stored_symlink_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            link = root / "config-link"
            target = "/" + "Users/example/config"
            os.symlink(target, link)
            self.assertTrue(scan_paths(root, [link]))

    def test_rejects_large_text_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            large = root / "large.txt"
            large.write_text("x" * (MAX_FILE_SIZE + 1))
            findings = scan_paths(root, [large])
            self.assertEqual("text file exceeds the secret-scan limit", findings[0].reason)

    def test_reads_tracked_content_from_index(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            path = root / "config.txt"
            key = "sk-or-v1-" + "x" * 48
            path.write_text(key)
            subprocess.run(["git", "add", "config.txt"], cwd=root, check=True)
            path.write_text("safe working tree value")
            findings = scan_candidates(candidate_blobs(root))
            self.assertEqual("OpenRouter API key", findings[0].reason)

    def test_reads_unstaged_tracked_content_from_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            path = root / "config.txt"
            path.write_text("safe index value")
            subprocess.run(["git", "add", "config.txt"], cwd=root, check=True)
            key = "sk-or-v1-" + "x" * 48
            path.write_text(key)
            findings = scan_candidates(candidate_blobs(root))
            self.assertEqual("OpenRouter API key", findings[0].reason)

    def test_skips_an_untracked_nested_repository_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            nested = root / "third_party" / "runtime"
            nested.mkdir(parents=True)
            subprocess.run(["git", "init", "-q"], cwd=nested, check=True)
            (nested / "source.c").write_text("safe source", encoding="utf-8")
            self.assertEqual([], candidate_blobs(root))
            object_id = subprocess.run(
                ["git", "hash-object", "-w", "source.c"],
                cwd=nested,
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            subprocess.run(
                [
                    "git",
                    "update-index",
                    "--add",
                    "--cacheinfo",
                    f"160000,{object_id},third_party/runtime",
                ],
                cwd=root,
                check=True,
                capture_output=True,
            )
            self.assertEqual([], candidate_blobs(root))


if __name__ == "__main__":
    unittest.main()
