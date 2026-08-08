# ChatESP iOS companion

The companion app supports any number of ChatESP devices. It saves one set of
global settings. Each device inherits the global values and can override any
setting. The app saves each edit at once. It does not require a complete valid
form before it saves another field.

The app saves API keys and Wi-Fi details in Keychain. It saves all non-secret
global values, device records, overrides, active-device selection, and
per-device provisioning revisions in one versioned preferences record. The
app can migrate the prior single-device record. It restores the active
ChatESP device after iOS restarts it for Bluetooth work. Scan, connect,
reconnect, frame, and confirmation operations have fixed time limits. Remove a
device from its settings page to stop its BLE work and delete its overrides.

The Models section explains each model role. Its searchable OpenRouter browser
shows only compatible models. Chat and tool models require text input, text
output, and tool calling. Transcription models require audio input and
transcription output. Speech models require text input, speech output, and the
English and French voices that the firmware uses. The catalog request has
fixed time and response-size limits. An invalid partial key does not block the
public model catalog.

The app sends one atomic, complete settings packet over the authenticated and
encrypted BLE provisioning service. If the app lost its local revision record, current
firmware can return the active revision and fingerprint in a flagged error.
The app saves that metadata and makes one bounded recovery transfer. It does
not use an unflagged error as recovery metadata.

After a ChatESP device is selected, the app also sends the current time, UTC
offset, and a position rounded to 0.1 degree when the device connects and at
most once per hour while connected. Significant-location monitoring runs only
when the user already gave Always location access. With While Using the App access,
the app requests one location while it is active. The live location stays in
memory and is not saved in the preferences record.

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
- The app restores secrets from Keychain after restart.
- The app restores global values, device overrides, the active device, and
  per-device revisions from one preferences record.
- Each field edit is saved at once, including when other required fields are
  empty or invalid. Provisioning stays unavailable until the effective device
  settings are complete and valid.
- The model browser can search the OpenRouter catalog. Each list contains only
  models that declare the capabilities required for that model role.
- The installed app starts at the full native screen size and does not use a
  compatibility-size canvas.
- The app sends time and a rounded location after each ChatESP device wake and
  no more than once per hour during one connection.
- A denied location permission still sends time and uses the saved city
  fallback.
