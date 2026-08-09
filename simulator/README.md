# ChatESP simulator

The ChatESP simulator is a portable desktop package for deterministic product
tests. It uses the production `app_core`, `provisioning_core`, and `ble_core`
logic. It replaces the physical display, buttons, touch controller, battery,
radio, audio, and power controller with bounded desktop adapters.

The simulator has no credential input and does not make network requests. A
scenario supplies synthetic transcripts and answers. Standard output contains
only non-private state metadata and byte counts. It does not contain transcript
or answer text. A requested SVG screen artifact can contain the synthetic
display text.

## Build and test

Use the repository wrapper. It uses the system C++ compiler and does not add a
Python dependency.

```sh
uv run --locked python simulator/tools/build.py --test
```

Use address and undefined-behavior sanitizers for protocol and crash work:

```sh
uv run --locked python simulator/tools/build.py --test --sanitize
```

This command runs all BLE scenarios and 30,000 deterministic malformed-frame
cases. The `ble fuzz CASES SEED` command accepts up to 100,000 cases for a
focused repeatable run.

Run one scenario and write the final display artifact:

```sh
uv run --locked python simulator/tools/build.py
simulator/.build/chatesp-sim \
  --scenario simulator/scenarios/direct-answer.sim \
  --render simulator/.build/direct-answer.svg
```

Run without `--scenario` for the line command protocol. Use `help` to list the
commands. Each accepted command writes one JSON state record. The command
protocol is stable within a simulator major version.

## Package boundary

The three `CHATESP_*_CORE_DIR` cache values point to `app_core`,
`provisioning_core`, and `ble_core`. Their defaults use the firmware portable
libraries. A separate repository can vendor these libraries, add them as
submodules, or set the CMake cache values to installed copies:

```sh
cmake -S simulator -B simulator/.build/cmake \
  -DCHATESP_APP_CORE_DIR=/path/to/app_core \
  -DCHATESP_PROVISIONING_CORE_DIR=/path/to/provisioning_core \
  -DCHATESP_BLE_CORE_DIR=/path/to/ble_core
cmake --build simulator/.build/cmake
ctest --test-dir simulator/.build/cmake --output-on-failure
```

The simulator owns its command protocol, desktop runtime, SVG renderer,
scenarios, tests, build wrapper, documentation, and license. It does not
include an ESP-IDF header or a board pin.

## Commands

Text after `transcript` and `answer` is private simulator input. The program
keeps it in bounded memory. It does not write it to standard output. `reset`, a
new action-button press, and sleep clear it.

```text
version
status
reset
ready
advance MILLISECONDS
action down|up
mode DURATION_MILLISECONDS
transcript TEXT
tool
answer TEXT
finish
fail
pairing show SIX_DIGIT_CODE
pairing hide
ble connect
ble pair SIX_DIGIT_CODE
ble reject
ble disconnect
ble radio on|off|restart
ble reboot
ble provision REVISION [FAULT]
ble fuzz CASES SEED
touch down X Y
touch up X Y
controls brightness PERCENT
controls volume PERCENT
clock HH:MM:SS|unavailable
wifi setup|off|connecting|online|failed
battery PERCENT|unavailable
render PATH
quit
```

One `advance` command accepts at most ten simulated minutes. The simulator
processes shorter internal ticks so a large accepted advance does not skip an
error recovery or an idle timeout.

## Scope

The simulator verifies user flow, state transitions, button duration rules,
BOOT-only Clock entry, continuous Clock mode, display orientation,
pairing-code priority, touch gestures, control values, the bounded Clock
network window, timeout behavior, privacy of command output, and SVG display
artifacts. It also runs the production BLE
packet validation, transfer assembly, settings ownership, and provisioning
session. The simulated link covers passkey confirmation, secure-link rejection,
bond retention, radio and cold restarts, bounded phone retry, dropped
acknowledgements, disconnects, corrupt frames, and storage failure. Transcript
input has the production 2,048-byte limit. Answer input has the production
1,280-byte limit.

The phone model matches the iOS provisioning policy. It retries one complete
transfer on the same secure link when an acknowledgement is lost. A link
disconnect fails the active request. A later request can start after the bonded
device reconnects.

It does not verify the CO5300, CST820, ES8311, AXP2101, QMI8658, PCF85063A,
Wi-Fi radio, NimBLE controller and host integration, Core Bluetooth system
pairing prompts, DMA behavior, acoustic quality, AMOLED output, current, or
physical wake behavior. Those checks still need the target board and a physical
iPhone when BLE is in scope.
