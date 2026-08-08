from __future__ import annotations

import argparse
import tempfile
import unittest
from pathlib import Path
from unittest.mock import call, patch

from tools.task_worktree import (
    create_task_worktree,
    is_expected_task_worktree,
    task_worktree_path,
    validate_task_name,
)


class TaskWorktreeTests(unittest.TestCase):
    def test_accepts_a_safe_task_name(self) -> None:
        self.assertEqual("clock-mode", validate_task_name("clock-mode"))

    def test_rejects_a_path_or_branch_escape(self) -> None:
        for value in ("../clock", "Clock", "clock_mode", "clock--"):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    validate_task_name(value)

    def test_destination_is_next_to_the_repository(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory).resolve() / "chatesp"

            destination = task_worktree_path(repository, "clock-mode")

            self.assertEqual(
                repository.parent / ".chatesp-worktrees" / "clock-mode",
                destination,
            )

    def test_create_uses_the_standard_branch_and_same_volume_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory).resolve() / "chatesp"
            repository.mkdir()
            expected = (
                repository.parent / ".chatesp-worktrees" / "clock-mode"
            )
            with (
                patch(
                    "tools.task_worktree.common_repository_root",
                    return_value=repository,
                ),
                patch("tools.task_worktree.subprocess.run") as run,
            ):
                destination, branch = create_task_worktree(
                    repository, "clock-mode", "origin/main"
                )

            self.assertEqual(expected, destination)
            self.assertEqual("vc/clock-mode", branch)
            self.assertEqual(
                [
                    call(
                        [
                            "git",
                            "worktree",
                            "add",
                            "-b",
                            "vc/clock-mode",
                            str(expected),
                            "origin/main",
                        ],
                        cwd=repository,
                        check=True,
                    ),
                    call(
                        [
                            "git",
                            "-C",
                            str(expected),
                            "submodule",
                            "update",
                            "--init",
                            "--checkout",
                        ],
                        check=True,
                    ),
                ],
                run.call_args_list,
            )

    def test_retry_reuses_only_the_registered_task_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory).resolve() / "chatesp"
            destination = (
                repository.parent / ".chatesp-worktrees" / "clock-mode"
            )
            destination.mkdir(parents=True)
            with (
                patch(
                    "tools.task_worktree.git_output",
                    side_effect=[str(destination), "vc/clock-mode"],
                ),
                patch(
                    "tools.task_worktree.common_repository_root",
                    return_value=repository,
                ),
            ):
                self.assertTrue(
                    is_expected_task_worktree(
                        destination, repository, "vc/clock-mode"
                    )
                )

    def test_existing_unrelated_directory_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory).resolve() / "chatesp"
            destination = (
                repository.parent / ".chatesp-worktrees" / "clock-mode"
            )
            destination.mkdir(parents=True)
            with (
                patch(
                    "tools.task_worktree.common_repository_root",
                    return_value=repository,
                ),
                patch(
                    "tools.task_worktree.is_expected_task_worktree",
                    return_value=False,
                ),
            ):
                with self.assertRaises(FileExistsError):
                    create_task_worktree(
                        repository, "clock-mode", "origin/main"
                    )


if __name__ == "__main__":
    unittest.main()
