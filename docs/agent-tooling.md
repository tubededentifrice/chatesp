# Agent tooling

This document gives detailed procedures for agent task workspaces and
repeatable workflow failures. `AGENTS.md` contains only the stable rules.

## Create a user-requested branch worktree

Work directly on `main` by default. Never create or switch to another branch
unless the user explicitly asks for a branch. The procedure in this section is
only for that explicit request.

First update the remote reference when the network is available:

```sh
git fetch origin main
```

From any ChatESP worktree, create the task workspace:

```sh
uv run --locked python tools/task_worktree.py TASK-NAME
```

The command accepts a short lowercase name. It creates branch
`vc/TASK-NAME` from `origin/main`. It puts the worktree in a hidden directory
next to the primary repository. Thus, the repository and worktree stay on the
same physical volume and use one canonical path. It also initializes the
tracked top-level submodules at their pinned commits. It does not initialize
optional nested submodules.
If submodule setup stops because the network is unavailable, run the same
command again. It reuses only the exact registered task worktree and branch.
It rejects any unrelated directory at the target path.

Do not put a build worktree in `/tmp`, `/private/tmp`, or another system
temporary directory. On macOS, `/tmp` and `/private/tmp` can name the same
directory. ESP-IDF can keep one spelling while PlatformIO uses the other. The
result can be two source files that incorrectly map to one object target.

## PlatformIO path recovery

Always run PlatformIO through `tools/pio.py`. The wrapper gives PlatformIO a
canonical `PWD` and cache path. It also removes only the generated watch-build
directory when stored build data contains a path alias. It does not remove
source files or a complete tool cache.

## Improve a repeatable workflow

When a tool or workflow fails:

1. Reproduce or trace the cause before a change.
2. Complete the current task with the smallest safe recovery.
3. If the cause can occur again, add a narrow checked-in guard and a regression
   test. Update detailed guidance if a tool cannot enforce the rule.
4. Keep stable rules in `AGENTS.md`, task procedures in a skill, and changing
   details in project documents.
5. Run the new regression test and the normal applicable quality gates.

Do not add a bypass for a permission, security, dependency, or irreversible
write policy. Do not convert a one-time external service outage into a source
change unless the change gives a safe general recovery.

## Build and install the iOS app locally

Never save a personal Apple Team ID in the tracked Xcode project. If Xcode
added one, move it to the ignored local configuration and remove it from the
project:

```sh
uv run --locked python tools/ios.py configure-signing
```

Build, sign, and install the current Debug app on the one available physical
iPhone:

```sh
uv run --locked python tools/ios.py install
```

Use `--launch` only when the task also requires an app launch. The tool uses
bounded device, build, install, and launch commands. It sanitizes personal
signing and device data from failure output. It stops if device selection is
not unique.
