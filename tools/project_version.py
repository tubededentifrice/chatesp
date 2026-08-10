"""Return the Git version that ESP-IDF embeds in the application image."""

from __future__ import annotations

import subprocess
from pathlib import Path


def current_project_version(root: Path) -> str:
    """Return the exact Git description used by the ESP-IDF build."""
    result = subprocess.run(
        ["git", "describe", "--always", "--tags", "--dirty"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()
