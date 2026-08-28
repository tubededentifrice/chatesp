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
canonical `PWD` and cache path. It also removes only the generated ChatESP device-build
directory when stored build data contains a path alias. It does not remove
source files or a complete tool cache. Device profiles share the ESP-IDF
`managed_components` directory. The wrapper serializes these builds so one
profile cannot replace a component while another profile uses it. Native tests
do not take this device-build lock.

Build each device profile and check its resolved contract before a release:

```sh
uv run --locked python tools/pio.py run -e watch_dev
uv run --locked python tools/firmware_contract.py --environment watch_dev
uv run --locked python tools/pio.py run -e watch_prod
uv run --locked python tools/firmware_contract.py --environment watch_prod
```

Use `tools/device_upload.py` for a device write. It requires an explicit
profile and port. It does not upload until the partition, image-size, and
resolved SDK checks pass. The checked image ceiling keeps at least 20 percent
of one 6 MiB OTA slot free. The report still gives image bytes and slot use.
The tool copies the exact checked flash artifacts to one isolated staging
directory and uploads them without a rebuild. It then erases only the
validated OTA-selection range so slot 0 starts.

## Reproduce product and BLE faults locally

Build the desktop simulator and run all scenarios with memory checks:

```sh
uv run --locked python simulator/tools/build.py --test --sanitize
```

Run `simulator/.build/chatesp-sim` without arguments for the line command
protocol. Use `ble connect`, `ble pair`, `ble provision`, `ble radio`,
`ble reboot`, `ble fuzz`, and `fuzz` to reproduce one flow. Use `expect FIELD
VALUE` in a scenario to stop when state does not match the expected value.
Each command writes one bounded JSON state record. Use an explicit seed for a
repeatable fuzz run. The sanitized suite checks at least 50,000 deterministic
product events and checks the state invariants after each event.

The simulator compiles the real portable app, packet, transfer, settings, and
provisioning-session code. It does not compile ESP-IDF or NimBLE. A local pass
does not close a physical board or iPhone gate.
The current SVG renderer has its own screen layout. It does not share the
firmware LVGL presentation code. Treat an SVG match as a state and desktop
layout check, not as proof of the physical pixel layout.

## Check the live model route

Use the ignored `.secrets/device.env` values to run the exact checked-in model
route contract against OpenRouter:

```sh
uv run --locked python tools/model_conformance.py --trials 10
```

The command derives the prompt, default model, registered tools, descriptions,
and schemas from the firmware source. It uses the firmware streaming request
shape. Each trial checks the exact French reciprocal-plot, multiplication, and
red-apple image requests. It also checks image result selection with current
result IDs. Output contains only trial numbers, tool names, argument-valid
flags, and fixed error categories. It does not print prompts, generated code,
search queries, answers,
or credentials.

Use `--model PROVIDER/MODEL` for a bounded comparison. A pass requires all
three route checks in all trials to pass. This command does not run speech
recognition, the device MicroPython engine, Brave Search, JPEG decode, speech,
or the AMOLED. Run their native and physical gates separately.

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
