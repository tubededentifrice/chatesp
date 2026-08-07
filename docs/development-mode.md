# Firmware modes

ChatESP has two explicit watch build profiles. The default profile is safe for
normal firmware development.

## Development mode

The `watch_dev` profile defines `CHATESP_DEVELOPMENT_MODE=1`. A sleep request
turns off the display and radio work, but it does not request AXP2101
system-off. USB stays available, so the next upload can reset and flash the
board without a manual button sequence. A PWR-button press wakes the app.

Build and upload development mode with one command:

```sh
uv run --locked python tools/pio.py run -e watch_dev -t upload
```

`watch_dev` is the default PlatformIO environment. A command without `-e` also
uses development mode. Use the explicit environment name in scripts and test
records.

Each watch upload ends with an ESP32 watchdog reset. This reset starts the app
without a button action and does not leave the chip in the ROM loader.

Use the repository monitor for device logs. It sets DTR and RTS before it opens
the port, so an open operation cannot request the ROM loader:

```sh
uv run --locked python tools/watch_monitor.py --port LOCAL_PORT --duration 10
```

Do not use the generic PlatformIO serial monitor on this board. It can assert
the USB control lines while it opens the port and request the ROM loader. Pass
the local port to the repository monitor on the command line. Do not write it
to a tracked file.

Development mode uses a five-minute automatic idle timer. This keeps the ready
screen visible during a test. A short PWR-button press still requests sleep at
once. Production mode keeps the 30-second automatic idle timer. Audio
cancellation, network cancellation, and thread reset stay equal in both modes.
Logs must show the selected mode but must not show credentials, chat text,
audio, or a stable device identifier.

If the voice runtime cannot start, development mode keeps the error on the
screen and keeps USB available. It does not enter a permanent black state.

BLE-provisioned development settings stay in volatile memory. Local values
from the ignored `.secrets/device.env` file are compiled into a local firmware
image. They stay in device flash until an erase or a replacement image removes
them. They must never enter a tracked file or a shared firmware artifact. The
firmware also initializes NVS because the Wi-Fi and Bluetooth drivers use it
for radio data. Development mode does not enable NVS encryption or persistent
BLE bonds.

## Production mode

The `watch_prod` profile defines `CHATESP_DEVELOPMENT_MODE=0`. It is the release
profile. A sleep request stops active work, turns off the display, and requests
AXP2101 system-off. USB can disconnect after system-off. This profile enables
HMAC-protected NVS and persistent BLE bonds.

Build production mode without a device change:

```sh
uv run --locked python tools/pio.py run -e watch_prod
```

On the first production start, ESP-IDF can create a random NVS HMAC key in the
configured eFuse key block. An eFuse write cannot be reversed. Inspect the
device eFuse use and get explicit user approval before this operation. The
wrapper blocks the production upload unless the approval variable is set for
that command:

```sh
CHATESP_ALLOW_PRODUCTION_EFUSE_PROVISION=1 uv run --locked python tools/pio.py run -e watch_prod -t upload
```

Do not put this variable in a shell profile or a tracked file. The variable is
only an upload guard. It does not change the firmware security settings.

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
