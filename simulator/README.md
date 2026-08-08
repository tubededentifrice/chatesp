# ChatESP simulator

The ChatESP simulator is a portable desktop package for deterministic product
tests. It uses the production `app_core` state and gesture logic. It replaces
the physical display, buttons, touch controller, battery, radio, audio, and
power controller with bounded desktop adapters.

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

`CHATESP_APP_CORE_DIR` is the only source-tree dependency. Its default value
points to `../firmware/lib/app_core`. A separate repository can vendor that
portable library, add it as a submodule, or set the CMake cache value to an
installed copy:

```sh
cmake -S simulator -B simulator/.build/cmake \
  -DCHATESP_APP_CORE_DIR=/path/to/app_core
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
error recovery, an idle timeout, or a Clock return.

## Scope

The simulator verifies user flow, state transitions, button duration rules,
Clock return, display orientation, pairing-code priority, touch gestures,
control values, the bounded Clock network window, timeout behavior, privacy of
command output, and SVG display artifacts. Transcript input has the production
2,048-byte limit. Answer input has the production 1,280-byte limit.

It does not verify the CO5300, CST820, ES8311, AXP2101, QMI8658, PCF85063A,
Wi-Fi radio, BLE radio, DMA behavior, acoustic quality, AMOLED output, current,
or physical wake behavior. Those checks still need the target board and a
physical iPhone when BLE is in scope.
