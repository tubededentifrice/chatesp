# Firmware modes

ChatESP has two explicit watch build profiles. The default profile is safe for
normal firmware development.

## Development mode

The `watch_dev` profile defines `CHATESP_DEVELOPMENT_MODE=1`. A sleep request
returns the app to its ready state. It does not turn off the display or request
AXP2101 system-off. USB stays available, so the next upload can reset and flash
the board without a manual button sequence.

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

Development mode changes only the final sleep action. The button state machine,
30-second timer, audio cancellation, network cancellation, and thread reset must
stay equal to production behavior. Logs must show the selected mode but must not
show credentials, chat text, audio, or a stable device identifier.

## Production mode

The `watch_prod` profile defines `CHATESP_DEVELOPMENT_MODE=0`. It is the release
profile. A sleep request stops active work, turns off the display, and requests
AXP2101 system-off. USB can disconnect after system-off.

Build and upload production mode with one command:

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
