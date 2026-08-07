#!/usr/bin/env python3
"""Fail when a repository dependency is mutable or less than two weeks old."""

from __future__ import annotations

import configparser
import json
import os
import re
import sys
import tomllib
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path


MINIMUM_AGE = timedelta(weeks=2)
UV_COOLDOWN = "2 weeks"
UV_REQUIRED_VERSION = ">=0.11.32"
UV_CI_VERSION = "0.11.32"
GIT_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
EXACT_PYTHON_DEPENDENCY_RE = re.compile(
    r"^[A-Za-z0-9_.-]+(?:\[[^]]+\])?==[^\s;]+(?:\s*;.*)?$"
)
GITHUB_SOURCE_RE = re.compile(
    r"^(?:git\+)?https://github\.com/(?P<owner>[^/]+)/(?P<repo>[^/?#]+)"
    r"(?:\?[^#]*)?#(?P<commit>[0-9a-f]{40})$"
)
GITHUB_URL_RE = re.compile(
    r"^https://github\.com/(?P<owner>[^/]+)/(?P<repo>[^/#]+)$"
)
ACTION_RE = re.compile(
    r"^\s*uses:\s*(?P<owner>[^/\s]+)/(?P<repo>[^@/\s]+)(?:/[^@\s]+)?@"
    r"(?P<commit>[0-9a-f]{40})\s*(?:#.*)?$"
)
PLATFORMIO_SPEC_RE = re.compile(
    r"^(?P<owner>[^/@\s]+)/(?P<name>[^@]+?)\s*@\s*(?P<version>[^\s]+)$"
)
HASHED_REQUIREMENT_RE = re.compile(
    r"^(?P<name>[A-Za-z0-9_.-]+)==(?P<version>[^\s;\\]+)"
)
PYPI_REGISTRY = "https://pypi.org/simple"
JSON_CACHE: dict[str, dict] = {}


class PolicyError(RuntimeError):
    """A dependency does not comply with the repository policy."""


def parse_time(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)


def require_old_enough(label: str, released_at: datetime, cutoff: datetime) -> None:
    if released_at > cutoff:
        available_on = released_at + MINIMUM_AGE
        raise PolicyError(
            f"{label} is too new ({released_at.isoformat()}); wait until "
            f"{available_on.isoformat()}"
        )


def load_toml(path: Path) -> dict:
    try:
        with path.open("rb") as source:
            return tomllib.load(source)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise PolicyError(f"cannot read {path}: {error}") from error


def github_reference(source: str) -> tuple[str, str, str]:
    match = GITHUB_SOURCE_RE.fullmatch(source)
    if not match:
        raise PolicyError(
            f"Git source must be a GitHub HTTPS URL with a full commit: {source}"
        )
    return (
        match.group("owner"),
        match.group("repo").removesuffix(".git"),
        match.group("commit"),
    )


def check_pyproject(
    root: Path,
) -> tuple[set[tuple[str, str, str]], dict[str, tuple[str, str]]]:
    project = load_toml(root / "pyproject.toml")
    uv = project.get("tool", {}).get("uv", {})
    if uv.get("exclude-newer") != UV_COOLDOWN:
        raise PolicyError(f"tool.uv.exclude-newer must be {UV_COOLDOWN!r}")
    if uv.get("required-version") != UV_REQUIRED_VERSION:
        raise PolicyError(
            f"tool.uv.required-version must be {UV_REQUIRED_VERSION!r}"
        )

    sources = uv.get("sources", {})
    git_references: set[tuple[str, str, str]] = set()
    expected_packages: dict[str, tuple[str, str]] = {}
    for dependency in project.get("project", {}).get("dependencies", []):
        name_match = re.match(r"^([A-Za-z0-9_.-]+)", dependency)
        if not name_match:
            raise PolicyError(f"invalid Python dependency: {dependency}")
        name = name_match.group(1)
        source = sources.get(name)
        if source:
            git_url = source.get("git", "")
            commit = source.get("rev", "")
            url_match = GITHUB_URL_RE.fullmatch(git_url)
            if not url_match or not GIT_COMMIT_RE.fullmatch(commit):
                raise PolicyError(
                    f"Python Git dependency {name} must use GitHub HTTPS and a full commit"
                )
            git_references.add(
                (
                    url_match.group("owner"),
                    url_match.group("repo").removesuffix(".git"),
                    commit,
                )
            )
            expected_packages[name.lower().replace("_", "-")] = ("git", commit)
        elif not EXACT_PYTHON_DEPENDENCY_RE.fullmatch(dependency):
            raise PolicyError(f"Python dependency is not exactly pinned: {dependency}")
        else:
            version = dependency.split("==", 1)[1].split(";", 1)[0].strip()
            expected_packages[name.lower().replace("_", "-")] = ("registry", version)
    return git_references, expected_packages


def check_uv_lock(
    root: Path, cutoff: datetime, expected_packages: dict[str, tuple[str, str]]
) -> set[tuple[str, str, str]]:
    lock = load_toml(root / "uv.lock")
    if lock.get("options", {}).get("exclude-newer-span") != "P2W":
        raise PolicyError("uv.lock does not contain the two-week exclude-newer span")

    git_references: set[tuple[str, str, str]] = set()
    locked_packages: dict[str, dict] = {}
    for package in lock.get("package", []):
        name = package.get("name", "unknown")
        locked_packages[name.lower().replace("_", "-")] = package
        source = package.get("source", {})
        if "git" in source:
            git_references.add(github_reference(source["git"]))
            continue
        if source == {"virtual": "."}:
            continue
        if source.get("registry") != PYPI_REGISTRY:
            raise PolicyError(f"locked package has an unapproved source: {name}")

        artifacts = []
        if package.get("sdist"):
            artifacts.append(package["sdist"])
        artifacts.extend(package.get("wheels", []))
        if not artifacts:
            raise PolicyError(f"locked registry package has no artifacts: {name}")
        quoted_name = urllib.parse.quote(name, safe="")
        quoted_version = urllib.parse.quote(package.get("version", ""), safe="")
        metadata = fetch_json(
            f"https://pypi.org/pypi/{quoted_name}/{quoted_version}/json"
        )
        registry_artifacts = {
            item.get("digests", {}).get("sha256"): item
            for item in metadata.get("urls", [])
            if item.get("digests", {}).get("sha256")
        }
        for artifact in artifacts:
            artifact_url = artifact.get("url", "unknown artifact")
            artifact_hash = artifact.get("hash", "")
            uploaded_at = artifact.get("upload-time")
            if not artifact_hash.startswith("sha256:"):
                raise PolicyError(f"locked artifact has no SHA-256 hash: {artifact_url}")
            if not uploaded_at:
                raise PolicyError(f"locked artifact has no upload time: {artifact_url}")
            registry_artifact = registry_artifacts.get(artifact_hash.removeprefix("sha256:"))
            if not registry_artifact or registry_artifact.get("url") != artifact_url:
                raise PolicyError(
                    f"locked artifact is not in current PyPI metadata: {artifact_url}"
                )
            registry_time = parse_time(registry_artifact["upload_time_iso_8601"])
            if abs(parse_time(uploaded_at) - registry_time) >= timedelta(seconds=1):
                raise PolicyError(f"locked artifact has a false upload time: {artifact_url}")
            require_old_enough(
                f"Python artifact {name}", registry_time, cutoff
            )

    for name, (source_type, expected_value) in expected_packages.items():
        package = locked_packages.get(name)
        if not package:
            raise PolicyError(f"direct Python dependency is absent from uv.lock: {name}")
        if source_type == "registry" and package.get("version") != expected_value:
            raise PolicyError(
                f"uv.lock has {name} {package.get('version')!r}; expected {expected_value!r}"
            )
        if source_type == "git":
            locked_source = package.get("source", {}).get("git", "")
            if github_reference(locked_source)[2] != expected_value:
                raise PolicyError(f"uv.lock has a different Git commit for {name}")
    return git_references


def check_platformio(
    root: Path,
) -> tuple[set[tuple[str, str, str, str]], set[tuple[str, str, str]]]:
    candidates = [root / "platformio.ini", root / "firmware" / "platformio.ini"]
    config_path = next((path for path in candidates if path.exists()), None)
    if config_path is None:
        return set(), set()

    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    try:
        config.read(config_path)
    except configparser.Error as error:
        raise PolicyError(f"cannot read {config_path}: {error}") from error

    registry_references: set[tuple[str, str, str, str]] = set()
    git_references: set[tuple[str, str, str]] = set()
    option_types = {
        "platform": "platform",
        "platform_packages": "tool",
        "lib_deps": "library",
    }
    for section in config.sections():
        for option, package_type in option_types.items():
            if not config.has_option(section, option):
                continue
            for raw_spec in config.get(section, option).splitlines():
                spec = raw_spec.strip()
                if not spec or spec.startswith(";"):
                    continue
                if spec.startswith("${"):
                    raise PolicyError(
                        f"PlatformIO {option} contains an unresolved substitution: {spec}"
                    )
                if "github.com" in spec:
                    git_references.add(github_reference(spec))
                    continue
                match = PLATFORMIO_SPEC_RE.fullmatch(spec)
                if not match:
                    raise PolicyError(
                        f"PlatformIO {option} is not an exact owner/name pin: {spec}"
                    )
                version = match.group("version")
                if version[0] in "^~<>=*" or "," in version:
                    raise PolicyError(f"PlatformIO dependency is not exact: {spec}")
                registry_references.add(
                    (package_type, match.group("owner"), match.group("name"), version)
                )
    return registry_references, git_references


def check_github_actions(root: Path) -> set[tuple[str, str, str]]:
    references: set[tuple[str, str, str]] = set()
    workflow_root = root / ".github" / "workflows"
    for path in sorted(workflow_root.glob("*.y*ml")):
        content = path.read_text()
        if "astral-sh/setup-uv@" in content and f'version: "{UV_CI_VERSION}"' not in content:
            raise PolicyError(
                f"{path} must pin setup-uv to uv {UV_CI_VERSION}"
            )
        for line_number, line in enumerate(content.splitlines(), start=1):
            if "uses:" not in line or line.lstrip().startswith("#"):
                continue
            value = line.split("uses:", 1)[1].strip()
            if value.startswith("./"):
                continue
            match = ACTION_RE.fullmatch(line)
            if not match:
                raise PolicyError(
                    f"GitHub Action is not pinned to a full commit at "
                    f"{path}:{line_number}: {value}"
                )
            references.add(
                (match.group("owner"), match.group("repo"), match.group("commit"))
            )
    return references


def check_hashed_requirements(root: Path, cutoff: datetime) -> int:
    path = root / "tools" / "espidf-python-requirements.txt"
    if not path.exists():
        return 0

    blocks: list[list[str]] = []
    current: list[str] = []
    for line in path.read_text().splitlines():
        if line and not line[0].isspace() and not line.startswith("#"):
            if current:
                blocks.append(current)
            current = [line]
        elif current:
            current.append(line)
    if current:
        blocks.append(current)

    checked = 0
    for block in blocks:
        match = HASHED_REQUIREMENT_RE.match(block[0])
        if not match:
            raise PolicyError(f"ESP-IDF requirement is not exactly pinned: {block[0]}")
        name = match.group("name")
        version = match.group("version")
        locked_hashes = set(
            re.findall(r"--hash=sha256:([0-9a-f]{64})", "\n".join(block))
        )
        if not locked_hashes:
            raise PolicyError(f"ESP-IDF requirement has no SHA-256 hash: {name}")
        quoted_name = urllib.parse.quote(name, safe="")
        quoted_version = urllib.parse.quote(version, safe="")
        data = fetch_json(f"https://pypi.org/pypi/{quoted_name}/{quoted_version}/json")
        registry_artifacts = {
            item.get("digests", {}).get("sha256"): item
            for item in data.get("urls", [])
            if item.get("digests", {}).get("sha256")
        }
        for artifact_hash in locked_hashes:
            artifact = registry_artifacts.get(artifact_hash)
            if not artifact:
                raise PolicyError(
                    f"ESP-IDF hash is not in PyPI metadata: {name}=={version}"
                )
            require_old_enough(
                f"ESP-IDF Python artifact {name}",
                parse_time(artifact["upload_time_iso_8601"]),
                cutoff,
            )
        checked += 1
    return checked


def fetch_json(url: str) -> dict:
    if url in JSON_CACHE:
        return JSON_CACHE[url]
    headers = {
        "Accept": "application/json",
        "User-Agent": "repository-dependency-age-check/1",
    }
    token = os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            data = json.load(response)
            JSON_CACHE[url] = data
            return data
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as error:
        raise PolicyError(f"cannot read dependency metadata from {url}: {error}") from error


def check_github_reference(
    reference: tuple[str, str, str], cutoff: datetime
) -> None:
    owner, repo, commit = reference
    quoted_owner = urllib.parse.quote(owner, safe="")
    quoted_repo = urllib.parse.quote(repo, safe="")
    quoted_commit = urllib.parse.quote(commit, safe="")
    data = fetch_json(
        f"https://api.github.com/repos/{quoted_owner}/{quoted_repo}/commits/{quoted_commit}"
    )
    if data.get("sha") != commit:
        raise PolicyError(f"GitHub did not return the requested commit: {owner}/{repo}@{commit}")
    released_at = parse_time(data["commit"]["committer"]["date"])
    require_old_enough(f"Git commit {owner}/{repo}@{commit}", released_at, cutoff)


def check_github_release(
    owner: str, repo: str, tag: str, cutoff: datetime
) -> None:
    quoted_owner = urllib.parse.quote(owner, safe="")
    quoted_repo = urllib.parse.quote(repo, safe="")
    quoted_tag = urllib.parse.quote(tag, safe="")
    data = fetch_json(
        f"https://api.github.com/repos/{quoted_owner}/{quoted_repo}/releases/tags/{quoted_tag}"
    )
    if data.get("tag_name") != tag:
        raise PolicyError(f"GitHub did not return release {owner}/{repo}@{tag}")
    require_old_enough(
        f"GitHub release {owner}/{repo}@{tag}",
        parse_time(data["published_at"]),
        cutoff,
    )


def check_platformio_reference(
    reference: tuple[str, str, str, str], cutoff: datetime
) -> None:
    package_type, owner, name, version = reference
    path = "/".join(
        urllib.parse.quote(value.lower(), safe="")
        for value in (owner, package_type, name)
    )
    query = urllib.parse.urlencode({"version": version})
    data = fetch_json(f"https://api.registry.platformio.org/v3/packages/{path}?{query}")
    returned_version = data.get("version", {}).get("name")
    if returned_version != version:
        raise PolicyError(
            f"PlatformIO returned {returned_version!r} for {owner}/{name}@{version}"
        )
    released_at = parse_time(data["version"]["released_at"])
    require_old_enough(
        f"PlatformIO {owner}/{name}@{version}", released_at, cutoff
    )


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    cutoff = datetime.now(timezone.utc) - MINIMUM_AGE
    try:
        git_references, expected_packages = check_pyproject(root)
        git_references.update(check_uv_lock(root, cutoff, expected_packages))
        git_references.update(check_github_actions(root))
        hashed_requirement_count = check_hashed_requirements(root, cutoff)
        platformio_references, platformio_git_references = check_platformio(root)
        git_references.update(platformio_git_references)
        for reference in sorted(git_references):
            check_github_reference(reference, cutoff)
        check_github_release("astral-sh", "uv", UV_CI_VERSION, cutoff)
        for reference in sorted(platformio_references):
            check_platformio_reference(reference, cutoff)
    except (KeyError, PolicyError, ValueError) as error:
        print(f"Dependency age check failed: {error}", file=sys.stderr)
        return 1

    print(
        "Dependency age check passed: "
        f"two-week cutoff {cutoff.isoformat()}, "
        f"{len(git_references)} Git pin(s), "
        f"{len(platformio_references)} PlatformIO pin(s), "
        f"uv {UV_CI_VERSION}, "
        f"{hashed_requirement_count} hashed ESP-IDF Python pin(s)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
