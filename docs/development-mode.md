# Firmware modes

ChatESP has two explicit ChatESP device build profiles. The default profile is safe for
normal firmware development.

## Development mode

The `watch_dev` profile defines `CHATESP_DEVELOPMENT_MODE=1`. A sleep request
turns off the display and radio work, but it does not request AXP2101
system-off. USB stays available, so the next upload can reset and flash the
board without a manual button sequence. A PWR-button press wakes the app. The
top mode button does not wake it and has no effect while it sleeps.

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
and reads one bounded boot window. It requires a V2 probe, panel-on, two equal
nonzero brightness commands, display readiness, and voice runtime readiness.
It redacts network addresses and the device address. Its automatic checks do
not prove pixel output. Confirm that `CHAT ESP` or `READY` is visible before a
physical display gate passes. Use `--no-upload` only to check an image that is
already installed.

Development ChatESP mode uses a five-minute automatic idle timer. This keeps
the ready screen visible during a test. Clock mode has no automatic sleep
timer. A short PWR-button press still requests sleep at once. Production
ChatESP mode keeps the 30-second automatic idle timer. Audio
cancellation, network cancellation, and thread reset stay equal in both modes.
Logs must show the selected mode but must not show credentials, chat text,
audio, or a stable device identifier.

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
for radio data. Development mode does not enable NVS encryption or persistent
BLE bonds. Brightness and volume are non-secret device preferences. They use a
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
