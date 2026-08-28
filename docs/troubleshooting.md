# Device and BLE troubleshooting

Use this record when the display is black, the iPhone stays in `Connecting`,
or a settings transfer times out. These symptoms can have different causes.
Do not remove the device or erase its bond until the logs prove an
authentication failure.

## First checks

Use the repository monitor. Do not use the generic PlatformIO monitor:

```sh
uv run --locked python tools/watch_monitor.py --port LOCAL_PORT --duration 600
```

Opening the native USB serial port can cause one `USB_UART_CHIP_RESET`. Treat
that reset as the start of the test. It is not a product reset. Read the retained
crash trace from the prior boot before you assess the current boot. Poll the
active monitor again before you infer that an event did not occur. Tool output
can remain unread until the next poll or until the bounded monitor closes.

The runtime heartbeat proves that the main loop still runs. A black display is
not enough evidence of an ESP crash. A retained watchdog, panic, brownout, or
other unexpected reset reason is crash evidence.

During a wake test, press and release PWR exactly once, then wait. A second
short press after a successful wake requests sleep again. This can make a good
wake look like an immediate failure.

## Known traps and their fixes

### CO5300 accepts wake commands but stays black

Observed signature:

- the trace contains `display_wake_begin` and `display_wake_complete`;
- the log reports two nonzero brightness commands;
- the ESP remains alive and BLE connects;
- the pixels stay black.

A successful brightness or display-on command does not prove that CO5300 pixels
became visible after a sleep interval. Repeating only the same commands is not
a valid recovery check. Development soft sleep must keep the initialized panel
at its current brightness and draw one full black frame. Each in-session wake
replays the bounded selected-panel initialization table at zero brightness,
forces one complete redraw, and then restores brightness. It does not reset the
panel or touch controller. Production uses brightness zero only after the sleep cancel window,
immediately before AXP2101 system-off.

### Touch NACKs or a disconnected touch controller

A failed touch read is a release. It must not keep the prior pressed point or
restart the device. The board reports only a rate-limited error category and a
saturating counter. It does not log coordinates. If touch stays unavailable,
use PWR for recording and sleep. Voice, playback, later turns, and display
sleep must continue. An unknown board revision uses the V2 display path and
does not start touch. Original-board touch and display gates remain open until
they pass on physical hardware.

### ESP-IDF connection reattempt stops advertising

Observed signature:

```text
NimBLE: Reattempt advertising; reason: 0x3e
NimBLE: Adv reattempt failed; rc= 3
```

The ESP-IDF ESP32-S3 reattempt path calls advertising with a null parameter
record. NimBLE returns invalid argument, and that path suppresses the normal
failed-connection callback. The firmware then has no event that can restart its
complete advertising record. Keep
`CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT=n`. ChatESP receives the normal failed
connection event and calls its own bounded advertising start with valid
parameters. A write response is not connection success.

### A failed connection races with BLE shutdown

Observed signatures include an intentional disconnect followed by one of these
messages:

```text
BLE connection failed (category 1)
BLE advertisement fields failed
BLE advertising start failed
```

The connection callback can arrive after the stop worker has started. It must
not start advertising during that stop. Outside shutdown, a transient
advertising failure gets five retries at 100-millisecond intervals on the
NimBLE host queue. If all retries fail, the idle runtime stops and starts the
complete BLE host. Do not add a delay inside the GAP callback. It would block
the host queue and make the shutdown race worse.

### The prior PWR policy can cause PMIC system-off

AXP2101 register `0x10`, bit 2 enables a separate 16-second PWR shutdown path.
It is not the normal PWR long-hold source control. The correct long-hold source
control is `PWROFF_EN` register `0x22`, bit 1. Firmware must clear the legacy
`0x10`, bit 2 state at every start because PMU state can survive an ESP reset.
It must change `0x22`, bit 1 while it handles a held action button. Do not infer
an ESP panic when the PMIC removed power and the retained RTC trace is empty.
AXP2101 state can survive an ESP reset. Firmware must restore the normal
long-hold shutdown policy at an idle startup. This keeps the hardware recovery
action available if the ESP later becomes unresponsive.

### A short battery wake starts listening and then reports a key error

The battery-powered EXIO4 level can stay active after the user releases PWR.
The old cold-start path treated this level as a continued hold. It could enter
recording before the 500-millisecond idle settings check applied the saved NVS
record. The first request then had no active service key, even though the key
was present in flash.

At startup, a completed AXP2101 release or short-press event must override the
EXIO4 level. Production must also apply its valid NVS settings record before it
queues a startup button command. One short battery wake must show `READY`. A
continued hold must show `LISTENING` at the normal threshold and must use the
saved service key.

Apply the record on the voice-runtime task, before its first startup command.
Do not apply it on the main startup task. The complete record can overflow the
smaller main stack and cause a reset loop after `settings_apply_begin`.

### Production resets after the display starts and the codec initializes

Observed signature:

```text
Display ready
ES8311: Work in Slave mode
***ERROR*** A stack overflow in task main has been detected.
```

This signature has two start limits. The LVGL task can run as soon as the
default display exists. Hold the LVGL lock until panel polling commands and
brightness setup finish. Release the completed storage startup stack before the
persistent passkey and 28 KiB voice-runtime stack allocations. The runtime
stack must fit the measured largest internal-memory block, not only the total
free memory. Give storage startup a five-second completion limit.
Load the small preference record after hidden panel start and before brightness
rises. A successful start must reach `runtime_ready`.

### The iPhone misses a short advertising window

The selected-device reconnect scan runs for 30 seconds. A failed scan has only
a one-second gap before the next scan. A long reconnect backoff can miss the
complete device wake window and leave the app in `Connecting`. Initial pairing
and a selected-device reconnect are different operations. A saved bond does not
require a new pairing code.

### A wake-and-record hold starts Wi-Fi before the phone reconnects

Observed signature:

- a button wake starts microphone capture;
- no BLE start appears before the recording;
- two seconds after microphone capture starts, the runtime stops BLE or starts
  Wi-Fi;
- a later request works when one press wakes the device and a second press
  records.

The old path started BLE only from the 500-millisecond idle settings check. A
wake hold entered recording before that check. The recording-time fallback also
measured its two-second proxy limit from microphone start. A long recording
therefore removed its own reconnect window and selected Wi-Fi. Start BLE in the
button-wake path. Keep it for the full recording when a complete bond exists.
Start the bounded proxy limit only after button release. The trace events
`phone_proxy_wake_start`, `phone_proxy_grace_begin`, `phone_proxy_ready`, and
`phone_proxy_fallback` identify this path without request data or a device ID.

Settings indications are not a proxy-readiness gate. The secure proxy can be
ready before a settings indication starts. The app keeps a confirmed settings
fingerprint for 10 minutes in the active app session. It sends changed settings
at once. It delays an eligible unchanged refresh by two seconds, so the proxy
gets the first use of a reconnect.

### Settings success needs application evidence

The following device facts show that the saved bond and settings path work:

- stored local and peer bond keys are present;
- the link becomes secure;
- the proxy and acknowledgement notifications become ready;
- the settings acknowledgement is confirmed;
- settings apply starts and completes.

The iPhone must also report `settings_acknowledged` for the same connection. A
Wi-Fi connection does not prove that the current settings transfer completed.
It can use settings from an earlier transfer.

### Speech stops after a longer answer

`THE SPEECH SERVICE STOPPED SENDING AUDIO` means that no PCM block arrived
within the current audio wait limit. An HTTP response header does not start the
idle-audio timer. The first-audio timer continues until the first PCM byte
arrives. Later PCM blocks restart the idle-audio timer.

Use the `TTS request`, `first PCM byte`, PCM ingress mode, transport byte-count,
and final speech error events. Do not log the answer or audio. A missing first
PCM event identifies model or service preparation. A partial byte count
identifies a stopped network or phone-proxy transfer. A complete transport byte
count followed by a playback error identifies the device audio path.

### Clock restarts when it opens

An immediate production restart after a BOOT-button press can have the retained
event `ble_memory_recovery_restart`. Clock tried to stop BLE for optional time
sync after LVGL allocations could split the controller restart block. This was
not a Clock sleep request.

The runtime now reserves the BLE restart block before it creates the Clock
view. If the reservation is not available, it skips that one optional time-sync
attempt, requests BLE recovery, and keeps Clock active. A required voice cloud
request keeps the controlled-restart policy because it must restore a usable
radio state.

## Physical regression procedure

Keep the iPhone unlocked, keep the app in front, and keep both traces active.

1. Flash `watch_dev` and install and launch the current Debug iOS app.
2. Confirm the saved local and peer bond keys, a secure link, proxy readiness,
   and both settings and device-context acknowledgements.
3. Wait for the 30-second automatic sleep. Confirm the BLE stop completes.
4. Press and release PWR once. Do not press it again while the first wake is in
   progress. Confirm visible pixels, advertising, a secure reconnect, and
   proxy readiness. A recent confirmed settings fingerprint must not transfer
   again during this reconnect.
5. Repeat the sleep and wake cycle. A failed connection with reason `0x3e` must
   lead to a new ChatESP advertising start. The old `Adv reattempt failed`
   message must not occur.
6. Let the device sleep. Hold PWR to wake and record in one press. Confirm that
   BLE starts before recording, reconnects during the hold, and stays active
   for a recording longer than two seconds. Release PWR and confirm that the
   request uses the secure iPhone proxy without a Wi-Fi start.
7. Hold PWR for more than 16 seconds during one recording. Confirm that the PMIC
   does not remove power, then release the button and confirm that the runtime
   continues through the phone proxy.
8. Change one valid setting. Confirm one settings packet and its application
   acknowledgement. Repeat a sleep and wake cycle within 10 minutes. Confirm
   that the unchanged settings packet is not sent again.

Do not put credentials, addresses, request text, audio, or stable device
identifiers in a test record.
