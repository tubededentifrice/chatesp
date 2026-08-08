#!/usr/bin/env python3
"""Create one canonical, same-volume Git worktree for an agent task."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


TASK_NAME = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,47}[a-z0-9])?")


def validate_task_name(value: str) -> str:
    """Accept a short task name that is safe in a path and branch."""
    if not TASK_NAME.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "use 1 to 49 lowercase letters, numbers, or internal hyphens"
        )
    return value


def git_output(repository: Path, *arguments: str) -> str:
    """Run one read-only Git query and return its output."""
    result = subprocess.run(
        ["git", *arguments],
        cwd=repository,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def common_repository_root(repository: Path) -> Path:
    """Return the primary repository root from any linked worktree."""
    common_git_dir = Path(
        git_output(
            repository,
            "rev-parse",
            "--path-format=absolute",
            "--git-common-dir",
        )
    ).resolve()
    return common_git_dir.parent


def task_worktree_path(repository_root: Path, task_name: str) -> Path:
    """Put task worktrees next to the repository on the same volume."""
    parent = repository_root.parent / f".{repository_root.name}-worktrees"
    return (parent / task_name).resolve()


def is_expected_task_worktree(
    destination: Path, repository_root: Path, branch: str
) -> bool:
    """Accept only the registered worktree that this command would create."""
    try:
        worktree_root = Path(
            git_output(destination, "rev-parse", "--show-toplevel")
        ).resolve()
        worktree_branch = git_output(
            destination, "branch", "--show-current"
        )
        worktree_repository = common_repository_root(destination)
    except (OSError, subprocess.CalledProcessError):
        return False
    return (
        worktree_root == destination
        and worktree_branch == branch
        and worktree_repository == repository_root
    )


def create_task_worktree(
    repository: Path, task_name: str, base: str
) -> tuple[Path, str]:
    """Create one named worktree and its standard task branch."""
    repository_root = common_repository_root(repository)
    destination = task_worktree_path(repository_root, task_name)
    branch = f"vc/{task_name}"
    if destination.exists():
        if not is_expected_task_worktree(
            destination, repository_root, branch
        ):
            raise FileExistsError(
                f"another path already exists at: {destination}"
            )
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                "git",
                "worktree",
                "add",
                "-b",
                branch,
                str(destination),
                base,
            ],
            cwd=repository_root,
            check=True,
        )
    subprocess.run(
        [
            "git",
            "-C",
            str(destination),
            "submodule",
            "update",
            "--init",
            "--checkout",
        ],
        check=True,
    )
    return destination, branch


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create a canonical task worktree next to the primary repository."
        )
    )
    parser.add_argument("task_name", type=validate_task_name)
    parser.add_argument(
        "--base",
        default="origin/main",
        help="Git revision for the new worktree (default: origin/main)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    destination, branch = create_task_worktree(
        Path.cwd(), arguments.task_name, arguments.base
    )
    print(f"Task worktree: {destination}")
    print(f"Task branch: {branch}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
