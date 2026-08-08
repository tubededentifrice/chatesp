# ChatESP agent instructions

These instructions apply to the complete repository.

## Mission

Build an autonomous voice-first ChatGPT client for the Waveshare
ESP32-S3-Touch-AMOLED-1.8 board. Keep the product fast, minimal, private, and
simple at the point of use.

Before a change, read:

1. `.codex/skills/maintain-chatesp/SKILL.md`
2. `README.md`
3. `docs/architecture.md`
4. `docs/hardware.md`
5. `docs/provisioning-protocol.md` for a BLE or settings change

Some files can be absent during initial setup. Read them when they exist.

## Communication

- Use ASD-STE100 Simplified Technical English in user reports, commits, pull
  requests, comments, documentation, and agent instructions.
- Ask the user only about high-level choices that change visible product
  behavior. Make normal technical choices and verify them.
- Do not report a build as a physical test. State each unverified hardware gate.

## Product contract

- Hold the bottom PWR button to record. Release it to submit. A short press,
  without a recording, starts sleep.
- Transcribe the request on the display. Show progress while the model works.
  Show the short answer and speak it.
- Keep one chat thread while the device is awake. After 30 seconds without
  interaction, request AXP2101 system-off and discard the thread. A PWR-button
  cold start starts a new thread.
- Use an all-black terminal-style interface. Use motion only to communicate
  state or progress. Avoid decorative motion.
- Instruct the chat model to answer in concise, natural speech.
- Keep the agent harness small. Tools must use narrow interfaces and a registry
  so a provider can change without changes to the chat state machine.
- Support web search and image search first. A selected image can fill the
  screen. Do not add more tools without a clear product need.
- Keep the top BOOT button unassigned in the app. Diagnostic firmware can use
  it when this is clearly documented.

## Hardware and security constraints

- Use the official `waveshare/esp32_s3_touch_amoled_1_8` BSP as the base. Its
  current display start supports V2 only. Keep an original-board SH8601 path in
  the board adapter. Do not copy pin values into many modules.
- The display is 368 by 448. The original board uses SH8601 and FT3168. The V2
  board uses CO5300 and CST820. The ESP-IDF component uses its compatible
  CST816S driver-family API. Detect the revision in the board adapter. Use the
  BSP hardware helpers after detection.
- The board has an ES8311 audio codec, QMI8658 IMU, AXP2101 PMU, PCF85063A RTC,
  16 MB flash, and 8 MB PSRAM.
- Use bounded network operations. One failed optional service must not block
  the UI, sleep, or a later interaction.
- Keep raw microphone audio only in memory. Delete it after transcription. Do
  not put request audio, chat text, credentials, stable device identifiers, or
  precise location in logs.
- Store iOS secrets in Keychain. Provision device secrets only over an
  authenticated and encrypted BLE connection. Production stores them in
  plaintext NVS because encrypted NVS can burn an eFuse key. State this
  physical-access risk in security documents.
- Store non-secret iOS choices in one versioned preferences record. Do not put
  secrets in that record.
- Never put API keys, Wi-Fi credentials, Apple team IDs, personal paths, names,
  email addresses, device identifiers, or signing data in tracked files.
- Never burn an eFuse or enable another irreversible device write. This rule
  applies to development, production, tests, and recovery. Keep
  `CHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0` explicit in each device build.
  Keep source checks that reject NVS encryption, flash encryption, secure boot,
  or a nonzero irreversible-write flag. Do not add an approval bypass.

## Tooling and dependency policy

- Use `uv` for each Python environment, dependency, and command. Do not use
  bare `pip`, a manual virtual environment, Poetry, or Conda.
- The standard-library command `python3 tools/check_dependency_age.py` is the
  only pre-`uv` exception.
- Use `uv sync --locked`. Run PlatformIO only through
  `uv run --locked python tools/pio.py`.
- Keep CI uv, direct Python, and PlatformIO dependencies exactly pinned. Pin Git
  dependencies to a full 40-character commit.
- Keep the two-week dependency cooldown. A young urgent security update needs
  an explicit policy change and review. Do not silently bypass the cooldown.
- Use `apply_patch` for hand edits.

## Change workflow

1. Inspect Git status and preserve changes that do not belong to the task.
2. Identify user-visible, hardware, protocol, privacy, and power effects.
3. Update source, tests, and the authoritative documents together.
4. Run the applicable native tests, firmware builds, iOS builds, and hardware
   tests.
5. Run `$selfreview autofix` as the last implementation step.
6. Run the secret scan. Stage only task files. Commit one complete task with a
   short imperative subject and push `main`.

Do not commit an incomplete task. If a push is blocked, keep the verified local
commit and report the exact blocker.

## Engineering quality

- Keep hardware access, transport, providers, tools, conversation state,
  presentation, and power control in separate modules.
- Put pure state and protocol logic in native tests.
- Use monotonic time and subtraction-based elapsed-time checks.
- Bound buffers, request bodies, response bodies, tool rounds, retries, and
  timeouts. Validate all BLE and network input.
- Avoid repeated flash writes and unbounded `String` or container growth.
- Use application-level BLE acknowledgements with a version, revision, and
  content fingerprint. A write response alone is not success.
- Build the firmware after each firmware change. Flash and test the connected
  board when the change affects physical behavior.
- Build the unsigned generic iOS target and its test bundle after each iOS
  change. Pairing, restoration, and background BLE tests need a physical iPhone.

## Keep guidance current

Correct a task-relevant instruction or local skill in the same task when
evidence shows that it is wrong, stale, duplicated, or incomplete. Keep this
file for stable repository rules. Keep detailed and changing facts in project
documents. Keep task procedures in skills.
