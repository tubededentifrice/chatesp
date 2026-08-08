---
name: maintain-chatesp
description: Maintain, extend, diagnose, or review the ChatESP firmware, iOS companion, hardware integration, secure BLE provisioning, voice pipeline, model harness, tools, tests, and build documentation. Use for work with the Waveshare ESP32-S3-Touch-AMOLED-1.8, PlatformIO or ESP-IDF, LVGL, ES8311 audio, Wi-Fi, OpenRouter, Brave Search, AXP2101 system-off, SwiftUI, Keychain, AccessorySetupKit, or Core Bluetooth in this repository.
---

# Maintain ChatESP

## Load the project contract

Read `AGENTS.md`, `README.md`, `docs/architecture.md`, and `docs/hardware.md`.
For BLE or settings work, also read `docs/provisioning-protocol.md`.
Inspect Git status before an edit. Keep firmware, iOS, protocol, security, and
documentation contracts consistent.

## Classify the task

- Firmware state or UI: update pure logic tests, firmware tests, and visible
  behavior documentation.
- Board or driver: use the official Waveshare BSP, confirm the board revision,
  build, flash, run `tools/device_doctor.py`, and run the affected physical
  gate.
- Audio or cloud provider: keep credentials out of logs, bound audio and HTTP
  data, test failure independence, and measure the user-visible latency.
- Tools or model loop: preserve the small registry, validate arguments, limit
  tool rounds and results, and keep provider details outside conversation code.
- BLE or iOS: update the shared versioned protocol, firmware validation,
  application acknowledgement, Keychain storage, iOS tests, and physical-iPhone
  acceptance gates.
- Power: trace cancellation and peripheral shutdown, then measure current and
  repeat sleep and wake on the board.
- Diagnosis: reproduce or trace the cause before a behavior change.

## Preserve the product invariants

- Hold and release the bottom PWR button for talk. Use a short idle press for
  sleep.
- Keep a thread only during the 30-second awake session. Clear it on sleep.
- Keep the screen black and terminal-like. Use motion only for state feedback.
- Keep answers concise and natural for speech.
- Keep raw audio transient. Do not log or persist private content or secrets.
- Keep optional network, search, image, touch, IMU, RTC, and iOS failures from
  blocking sleep or a later voice interaction.
- Keep the top BOOT button unassigned outside clearly labeled diagnostics.
- Use authenticated encrypted BLE and Keychain for secrets. Production uses
  plaintext NVS because the project forbids the eFuse write that encrypted NVS
  can need. Do not describe device storage as encrypted.
- Never enable an eFuse burn, secure boot, flash encryption, encrypted NVS, or
  another irreversible device write. Keep the explicit zero build flag and
  compile-time checks in place for all device profiles. There is no approval
  bypass for this policy.
- Keep packet encoding, limits, UUIDs, revision rules, fingerprint rules, and
  acknowledgements authoritative in `docs/provisioning-protocol.md`. Store
  non-secret iOS choices in one versioned preferences record.
- Use the BSP as the base. Its current display start supports V2 only. Keep an
  original-board SH8601 path in one board layer. Keep pins in that layer.

## Implement

Use bounded state transitions and subtraction-based monotonic elapsed-time
checks. Validate every BLE and network boundary. Set fixed limits for recording
length, JSON, images, conversation history, output, retries, and tool rounds.
Avoid repeated NVS writes and unbounded heap growth.

Use the repository tools:

```sh
python3 tools/check_dependency_age.py
uv sync --locked
uv run --locked python -m unittest discover -s tests -p 'test_*.py'
uv run --locked python tools/pio.py test -e native
uv run --locked python tools/pio.py run
uv run --locked python tools/check_secrets.py
```

Run only applicable firmware commands while firmware profiles are not yet
present. Run iOS commands from `ios/README.md` for each iOS change. Do not use
bare Python package tools or global PlatformIO.

For a black display, or after a display or power change, run:

```sh
uv run --locked python tools/device_doctor.py --port LOCAL_PORT
```

It uploads development firmware and checks one bounded boot window. A native
USB serial open can reset the ESP32-S3 even when DTR and RTS are inactive. Use
the repository monitor only when this reset is acceptable. Serial command
success does not prove pixel output. Record a visible AMOLED check separately.

## Verify

- Review compiler warnings, flash and PSRAM use, task stacks, pin assignments,
  timeouts, cancellation, and recovery paths.
- Test empty, invalid, timeout, disconnect, cancellation, reboot, sleep, and
  millisecond-wrap cases.
- Build before a flash. Record the serial result and the physical screen,
  button, microphone, speaker, Wi-Fi, BLE, and current gates that ran.
- Separate generic iOS build success from physical iPhone BLE success.
- Run `$selfreview autofix` last. Then let the caller scan, stage, commit, and
  push the complete task.

## Maintain guidance

Apply the guidance-maintenance rule in `AGENTS.md`. Correct task-relevant stale
or missing skill guidance in the same task. Keep this file concise and put
detailed changing facts in the project documents.
