# ChatESP iOS companion

The companion app supports any number of ChatESP devices. It saves one set of
global settings. Each device inherits the global values and can override any
setting. The app saves each edit at once. It does not require a complete valid
form before it saves another field.

The Chat Display section sets chat text and icon size from 100% through 200%
in five-percent steps. Each device can override the global value. This setting
does not change the Clock face.

The app saves API keys and Wi-Fi details in Keychain. It saves all non-secret
global values, device records, overrides, active-device selection, and
per-device provisioning revisions in one versioned preferences record. The
app can migrate the prior single-device record. It restores the active
ChatESP device after iOS restarts it for Bluetooth work. Scan, connect,
reconnect-scan, frame, and confirmation operations have fixed time limits. A
reconnect retry scans for the saved Core Bluetooth identifier for 30 seconds
before it starts a new connection. A failed scan has a one-second gap before
the next bounded scan. It does not require device removal when the selected
device wakes after a long sleep. While the device sleeps, the status says that
it is asleep or unavailable instead of claiming an active connection attempt.
Remove a device from the last row of its settings page to stop its BLE work and
delete its overrides.
The app does not send settings after a Keychain read error. It retries the read
when the app becomes active.

The Models section explains each model role. Its searchable OpenRouter browser
uses the public all-modality catalog and does not send the saved API key. It
shows only compatible models. Chat and tool models require text input, text
output, and tool calling. Transcription models require audio input and
transcription output. Speech models require text input, speech output, and a
published voice list. Each model result shows its catalog input and output
price. Separate searchable English and French voice browsers show the voices
for the selected speech model. Models and voices support global values and
per-device overrides. The catalog request has fixed time and response-size
limits.

The app opens device discovery in a separate Add page. A new record has the
default name `ChatESP`. The main device list shows each connection status on
the same row as its device. It does not keep a second nearby-device section on
the main page. A device page contains settings, memories, and one status
section. The remove row is last and owns its confirmation dialog. The page has
no manual provisioning controls.

The app automatically sends one atomic settings packet over the authenticated
and encrypted BLE service when the effective settings fingerprint changes.
After a confirmed packet, it refreshes the same fingerprint at most once every
10 minutes. An unchanged reconnect gives the phone proxy two seconds before an
eligible refresh starts. A failed automatic transfer can retry after 30
seconds while the link stays ready. A disconnect or settings change cancels
the old schedule. Empty Wi-Fi, OpenRouter, and Brave values are valid. The firmware
reports a clear runtime error if a feature needs missing Wi-Fi or OpenRouter
credentials. An empty Brave key turns search off. Empty endpoint or model edits
use the built-in defaults when the app sends settings. Invalid nonempty values
remain local and the status section identifies the field that needs attention.
Selecting another device cancels an active settings transfer before the app
changes the Bluetooth peripheral.

While the secure BLE link is active, the app supplies the preferred network
path for ChatESP cloud requests. The app accepts only bounded HTTPS request
envelopes. It uses an ephemeral URL session, rejects non-HTTPS redirects, and
follows no more than the device-specified two-redirect limit. It sends bulk
response data with Core Bluetooth write-without-response flow control. It
confirms response boundary frames. It forwards bounded `audio/pcm` data while
a declared-length HTTPS body arrives. It keeps other responses bounded before
transfer. A device timeout cannot exceed the 180-second product request limit.
Each local operation has a separate identity. The complete BLE response has a
180-second limit after its first response frame is ready. A stalled response
clears its background task and disconnects the link. If this proxy is not
ready, the firmware uses its configured Wi-Fi path.

If the app lost its local revision record, current firmware can return the
active revision and fingerprint in a flagged error.
The app saves that metadata and makes one bounded recovery transfer. It does
not use an unflagged error as recovery metadata.

After a ChatESP device is selected, the app also sends the current time, UTC
offset, and a position rounded to 0.1 degree when the device connects and at
most once per hour while connected. Significant-location monitoring runs only
when the user already gave Always location access. With While Using the App access,
the app requests one location while it is active. The live location stays in
memory and is not saved in the preferences record. The hourly timer does not
request a live location while the selected device is disconnected.

While the selected ChatESP device is connected, the app can list, add, delete,
and clear its saved memories. The ChatESP device is the only source of truth.
The app clears the view on disconnect and does not save a memory mirror in
preferences, UserDefaults, Keychain, or another store. If the optional memory
characteristics are absent, settings still work and the app asks for a firmware
update. Facts are plaintext on the ChatESP device and go to the configured
chat model with each request.

The project has no personal team or signing setting. Build the generic unsigned
iOS target:

```sh
xcodebuild \
  -project ios/ChatESP.xcodeproj \
  -scheme ChatESP \
  -configuration Debug \
  -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Run the test bundle on an installed simulator. Replace the simulator name only
when Xcode does not have this runtime:

```sh
xcodebuild \
  -project ios/ChatESP.xcodeproj \
  -scheme ChatESP \
  -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  CODE_SIGNING_ALLOWED=NO \
  test
```

For a physical iPhone, keep the Apple Team ID in the ignored local signing
file. If Xcode added the Team ID to the tracked project, capture and remove it:

```sh
uv run --locked python tools/ios.py configure-signing
```

Build and install the latest Debug app on the one available physical iPhone:

```sh
uv run --locked python tools/ios.py install
```

Add `--launch` to open the app after installation. The command fails safely
when no iPhone or more than one iPhone is available. Use `--device` only to
select between multiple local devices. The tool does not print the Team ID,
device name, or device identifier.

The simulator build and tests do not test Bluetooth pairing. A physical iPhone
and integrated device firmware must pass these gates:

- The ChatESP device shows a new six-digit pairing code.
- iOS shows its system pairing prompt.
- The ChatESP device rejects a transfer before authenticated encrypted pairing.
- An accepted transfer returns a matching revision and fingerprint.
- A retry of the same transfer returns `unchanged` and causes no NVS write.
- A disconnect, timeout, bad packet, or wrong acknowledgement does not show
  success.
- A lost iOS revision record causes at most one recovery packet. Matching
  content returns `unchanged`. Changed content uses the next revision.
- A scan stops after 10 seconds. A connection attempt stops after 10 seconds.
- A frame with no write response fails the connection. Only a missing final
  acknowledgement can cause one complete-transfer retry.
- iOS stores any number of ChatESP device records and restores only the active
  device after a background restart. A callback from another device does not
  change the active transfer.
- Removing the active ChatESP device stops scan, reconnect, location, and BLE
  work. The app does not reconnect until the user selects a device again.
- Memory list pages use one stable revision and fingerprint. A conflict reloads
  the full list before another edit.
- A retried memory request does not cause a second write. A voice memory change
  causes one full refresh.
- Disconnect clears the memory view. Old firmware still accepts settings and
  shows the memory firmware-update message.
- Memory BLE work does not reset the idle timer or prevent normal sleep.
- The app restores secrets from Keychain after restart. A temporary Keychain
  read error cannot send an empty credential update.
- The app restores global values, device overrides, the active device, and
  per-device revisions from one preferences record.
- Each field edit is saved at once. Empty credentials can sync. Empty endpoint
  and model values use built-in defaults. An invalid nonempty value does not
  block later edits, and the app syncs when the effective values are valid.
- The model browser can search the OpenRouter catalog. Each list contains only
  models that declare the capabilities required for that model role. Each
  model shows its price. Speech voice searches show only voices published for
  the selected model.
- The installed app starts at the full native screen size and does not use a
  compatibility-size canvas.
- The app sends time and a rounded location after each ChatESP device wake and
  no more than once per hour during one connection.
- A denied location permission still sends time and uses the saved city
  fallback.
