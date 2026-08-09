# Firmware modes

ChatESP has two explicit ChatESP device build profiles. The default profile is safe for
normal firmware development.

## Development mode

The `watch_dev` profile defines `CHATESP_DEVELOPMENT_MODE=1`. A sleep request
draws a full black frame and stops radio work, but it does not change panel
brightness or request AXP2101 system-off. A zero-brightness CO5300 can accept
later brightness and display-on commands while its pixels stay black. Keeping
the initialized panel at its current brightness avoids this false-success wake
state. Black AMOLED pixels emit no visible light. USB stays available, so the
next upload can reset and flash the board without a manual button sequence. A
PWR-button press removes the black frame and redraws the display. The top mode
button does not wake it and has no effect while it sleeps.

Build and upload development mode with one command:

```sh
uv run --locked python tools/pio.py run -e watch_dev -t upload
```

`watch_dev` is the default PlatformIO environment. A command without `-e` also
uses development mode. Use the explicit environment name in scripts and test
records.

Each ChatESP device upload ends with an ESP32 watchdog reset. This reset starts the app
without a button action and does not leave the chip in the ROM loader.

Use the repository monitor for device logs. It sets DTR and RTS to inactive
values before it opens the port. On the ESP32-S3 native USB serial interface,
the host open or reconnect can still cause a `USB_UART_CHIP_RESET`. Use it only
when a reset is acceptable. The monitor disables the close-time terminal
hangup action. It closes with a bounded reset while the boot line is inactive,
so the app starts instead of the ROM loader:

```sh
uv run --locked python tools/watch_monitor.py --port LOCAL_PORT --duration 10
```

Do not use the generic PlatformIO serial monitor on this board. It can assert
the USB control lines while it opens the port and request the ROM loader. Pass
the local port to the repository monitor on the command line. Do not write it
to a tracked file.

For a black screen, or after a display or power change, use the device doctor:

```sh
uv run --locked python tools/device_doctor.py --port LOCAL_PORT
```

It uploads `watch_dev`, checks that the image matches the current Git commit,
and reads one bounded boot window. It requires a V2 probe, two equal nonzero
brightness commands, a completed display wake sequence, and voice runtime
readiness. The board logs `Panel on` only when its tracked panel state changes,
so that optional record is not a separate pass condition.
It redacts network addresses and the device address. Its automatic checks do
not prove pixel output. Confirm that `CHAT ESP` or `READY` is visible before a
physical display gate passes. Use `--no-upload` only to check an image that is
already installed.

Development and production ChatESP modes use the same 30-second automatic idle
timer. Clock mode has no automatic sleep timer. A short PWR-button press still
requests sleep at once. Audio
cancellation, network cancellation, and thread reset stay equal in both modes.
Logs must show the selected mode but must not show credentials, chat text,
audio, or a stable device identifier.

The NimBLE shutdown completion wait has a one-second limit. If the host does
not stop in that time, automatic or button sleep still completes. The runtime
stays available while the stop worker finishes. The task watchdog restarts the
device if the worker stays blocked for five seconds. The watchdog also restarts
the device if a task prevents the scheduler from running for five seconds.

The firmware keeps a small crash trace in RTC memory. The trace contains the
three most recent boot records and the 16 most recent event codes in each boot.
It records reset reasons and bounded firmware stages, such as BLE stop and BLE
start. It does not record credentials, network addresses, chat text, audio,
locations, or device identifiers. It does not write to flash. The trace stays
available after software, watchdog, panic, and USB resets. A full loss of power
clears it. At the next boot, the device writes the earlier boot records to the
serial log. Thus, a monitor reset can show evidence from the reset that occurred
before it. A separate five-second runtime heartbeat shows if the main runtime
loop continued after its last recorded stage. The heartbeat does not use an
event slot and does not change the event checksum.

For a local reproduction before a board test, run:

```sh
uv run --locked python simulator/tools/build.py --test --sanitize
```

This runs the production portable app and BLE provisioning cores with address
and undefined-behavior checks. It covers pairing state, insecure-link rejection,
disconnects, retry, acknowledgement loss, storage failure, and malformed
frames. It does not run NimBLE, Core Bluetooth, the ESP32 controller, or the
physical memory layout. Use the retained crash trace and a physical board for
those gates.

A model `power_off` request uses the same development soft-sleep path. The
model gives a short confirmation first. A PWR-button action can cancel the
pending request before cleanup starts. After soft sleep, one PWR-button press
wakes the app.

If the voice runtime cannot start, development mode keeps the error on the
screen and keeps USB available. It does not enter a permanent black state.

BLE-provisioned development settings stay in volatile memory. Local values
from the ignored `.secrets/device.env` file are compiled into a local firmware
image. They stay in device flash until an erase or a replacement image removes
them. They must never enter a tracked file or a shared firmware artifact. The
firmware also initializes NVS because the Wi-Fi and Bluetooth drivers use it
for radio data. Development mode keeps BLE bonds in plaintext NVS so a normal
firmware upload does not break an iPhone pairing. It does not enable NVS
encryption. Brightness and volume are non-secret device preferences. They use a
separate fixed-size NVS record in development and production. Their defaults
are 65 and 70 percent. If the record cannot be stored, the new value applies
only to the current session and the tool result reports this state.

User-requested memories are different from provisioned development settings.
They persist in plaintext NVS in the `chesp_mem_dev` namespace. Production uses
`chesp_mem_prod`, so a development image cannot read the production memory
list. NVS erase removes both lists. A normal firmware update and settings
provisioning do not remove them.

## Production mode

The `watch_prod` profile defines `CHATESP_DEVELOPMENT_MODE=0`. It is the release
profile. A sleep request stops active work, turns off the display, and requests
AXP2101 system-off. USB can disconnect after system-off. This profile enables
persistent settings and persistent BLE bonds in plaintext NVS.

A model `power_off` request gives a short confirmation and then uses this same
system-off path. One bottom PWR-button press starts the board again. A held wake
continues into recording at the normal hold threshold.

Build production mode without a device change:

```sh
uv run --locked python tools/pio.py run -e watch_prod
```

Production and development both set
`CHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0`. Source checks reject a missing or
nonzero flag. The wrapper rejects a duplicate flag, an unreviewed environment,
or a project and build-flag override. CMake separately requires
`CHATESP_PERMANENT_WRITE_POLICY=FORBID`. SDK and source checks reject NVS
encryption, flash encryption, secure boot, anti-rollback eFuse writes, permanent
ROM download-mode changes, and direct first-party eFuse write APIs. These
features can make permanent hardware changes. A user request does not bypass
this policy.

Production stores provisioned credentials and BLE bonds in plaintext flash.
This is less secure than HMAC-protected NVS. A person with physical flash
access can read the values. The BLE provisioning link stays authenticated and
encrypted, and the iOS app keeps its copy of secrets in Keychain.

Upload production with the normal command:

```sh
uv run --locked python tools/pio.py run -e watch_prod -t upload
```

Use production mode for final sleep, wake, and battery-current tests. Do not use
a development-mode result as evidence for production power behavior.

## Recovery from a production image

If a production image is off, one PWR press must start it. Start an upload before
the 30-second timer expires. The upload tool can then reset the running device.

If normal upload cannot find the running device, use the documented hardware
loader sequence once. Upload `watch_dev` immediately. Later uploads can use the
normal one-command development path.

Do not store a USB port, device identifier, Wi-Fi value, API key, or other local
value in either profile.
