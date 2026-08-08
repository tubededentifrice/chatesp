# Simulator architecture

## Purpose

The simulator gives automated tools one deterministic control surface for
ChatESP product behavior. It is also a package boundary for a possible future
repository split.

## Components

- `chatesp_sim_app_core` compiles the production interaction state machine.
- `chatesp_simulator_core` owns simulated time, input adapters, display state,
  private bounded text, and SVG rendering.
- `chatesp-sim` owns the line command protocol and scenario-file reader.
- `build.py` supplies a repository-compatible build when CMake is not on the
  command path.

The command program never logs transcript or answer text. It reports only
bounded metadata. The SVG renderer writes text only after an explicit render
command. Scenarios must use synthetic content.

## Portability rules

- Do not include ESP-IDF or board headers.
- Do not use a pin, driver, or board revision value.
- Keep production dependencies behind `CHATESP_APP_CORE_DIR`.
- Use only the C++ standard library in the desktop package.
- Keep all input text, paths, command lines, and artifacts bounded.
- Use subtraction-based 32-bit monotonic time checks.
- Keep command output machine-readable and free of private text.

## Later adapters

The package can add an LVGL desktop adapter, host microphone and speaker
adapters, or a recorded HTTP adapter. These adapters must remain optional.
They must not change the deterministic command protocol or make the simulator
require credentials for its normal test mode.
