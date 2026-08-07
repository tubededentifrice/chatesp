#!/usr/bin/env python3
"""Reject likely secrets and personal build data in commit candidate files."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


MAX_FILE_SIZE = 2 * 1024 * 1024
FORBIDDEN_PARTS = {
    ".secrets",
    "secrets",
    "xcuserdata",
    "Local.xcconfig",
}
FORBIDDEN_NAMES = re.compile(
    r"(?i)(?<!check_)secrets?\.(?:h|hpp|c|cpp|swift|json|toml|ya?ml|env|xcconfig)$"
)
SECRET_PATTERNS = (
    ("OpenRouter API key", re.compile(r"\bsk-or-v1-[A-Za-z0-9_-]{20,}\b")),
    ("Brave API key", re.compile(r"\bBSA[A-Za-z0-9_-]{20,}\b")),
    ("GitHub token", re.compile(r"\b(?:ghp|github_pat)_[A-Za-z0-9_]{20,}\b")),
    ("AWS access key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("private key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    (
        "non-empty secret assignment",
        re.compile(
            r"(?m)^\s*(?:export\s+)?[A-Z0-9_]*(?:API_KEY|TOKEN|PASSWORD|SECRET)"
            r"[ \t]*[:=][ \t]*[\"']?([^\s\"'#][^\s\"'#]{7,})"
        ),
    ),
    (
        "quoted secret assignment",
        re.compile(
            r"(?i)(?:api[_-]?key|token|password|secret)[ \t]*[:=][ \t]*"
            r"[\"'][^\s\"']{8,}[\"']"
        ),
    ),
    ("Apple development team", re.compile(r"\bDEVELOPMENT_TEAM\s*=\s*[A-Z0-9]{10}\b")),
    ("personal home path", re.compile(r"(?:/Users|/home)/[^/\s]+/")),
)


@dataclass(frozen=True)
class Candidate:
    path: Path
    data: bytes


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    reason: str


def git_names(root: Path, arguments: list[str]) -> list[Path]:
    result = subprocess.run(
        ["git", *arguments, "-z"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    return [Path(os.fsdecode(item)) for item in result.stdout.split(b"\0") if item]


def candidate_blobs(root: Path) -> list[Candidate]:
    candidates: list[Candidate] = []
    for relative in git_names(root, ["ls-files", "--cached"]):
        result = subprocess.run(
            ["git", "show", "--no-textconv", f":./{relative.as_posix()}"],
            cwd=root,
            check=True,
            capture_output=True,
        )
        index_data = result.stdout
        candidates.append(Candidate(relative, index_data))

        # Inspect unstaged edits too. The index is the commit candidate, but
        # this check normally runs before staging and must not miss a secret
        # in a modified tracked file.
        path = root / relative
        if os.path.lexists(path):
            worktree_data = (
                os.fsencode(os.readlink(path))
                if path.is_symlink()
                else path.read_bytes()
            )
            if worktree_data != index_data:
                candidates.append(Candidate(relative, worktree_data))

    for relative in git_names(
        root, ["ls-files", "--others", "--exclude-standard"]
    ):
        path = root / relative
        if path.is_symlink():
            data = os.fsencode(os.readlink(path))
        else:
            data = path.read_bytes()
        candidates.append(Candidate(relative, data))
    return candidates


def forbidden_path(path: Path) -> bool:
    return bool(FORBIDDEN_PARTS.intersection(path.parts)) or bool(
        FORBIDDEN_NAMES.search(path.name)
    )


def scan_text(path: Path, text: str) -> list[Finding]:
    findings: list[Finding] = []
    for reason, pattern in SECRET_PATTERNS:
        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            findings.append(Finding(path, line, reason))
    return findings


def scan_candidates(candidates: list[Candidate]) -> list[Finding]:
    findings: list[Finding] = []
    for candidate in candidates:
        if forbidden_path(candidate.path):
            findings.append(
                Finding(candidate.path, 1, "forbidden secret or user-data path")
            )
            continue
        if b"\0" in candidate.data:
            continue
        sample = candidate.data[: MAX_FILE_SIZE + 1]
        try:
            text = sample.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if len(candidate.data) > MAX_FILE_SIZE:
            findings.append(
                Finding(candidate.path, 1, "text file exceeds the secret-scan limit")
            )
            continue
        findings.extend(scan_text(candidate.path, text))
    return findings


def scan_paths(root: Path, paths: list[Path]) -> list[Finding]:
    """Scan explicit paths for tests and narrow local checks."""
    candidates: list[Candidate] = []
    findings: list[Finding] = []
    for path in paths:
        relative = path.relative_to(root)
        try:
            data = os.fsencode(os.readlink(path)) if path.is_symlink() else path.read_bytes()
        except OSError as error:
            findings.append(Finding(relative, 1, f"cannot read file: {error}"))
            continue
        candidates.append(Candidate(relative, data))
    findings.extend(scan_candidates(candidates))
    return findings


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    try:
        findings = scan_candidates(candidate_blobs(root))
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Secret scan failed: {error}", file=sys.stderr)
        return 1

    if findings:
        for finding in findings:
            print(f"{finding.path}:{finding.line}: {finding.reason}", file=sys.stderr)
        print("Secret scan failed. Remove or ignore the listed data.", file=sys.stderr)
        return 1

    print("Secret scan passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
