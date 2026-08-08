# ChatESP iOS companion

The companion app saves API keys and Wi-Fi details in Keychain. It saves the
endpoint, model choices, optional city-level location, and provisioning
revision in one versioned preferences record. It sends one atomic settings
packet over the authenticated encrypted BLE provisioning service. After a
watch is selected, the app also sends the current time, UTC offset, and a
position rounded to 0.1 degree when the watch connects and at most once per
hour while connected. Significant-location monitoring avoids continuous GPS
use. The live location stays in memory and is not saved in the preferences
record.

While the selected watch is connected, the app can list, add, delete, and clear
its saved memories. The watch is the only source of truth. The app clears the
view on disconnect and does not save a memory mirror in preferences,
UserDefaults, Keychain, or another store. If the optional memory
characteristics are absent, settings still work and the app asks for a firmware
update. Facts are plaintext on the watch and go to the configured chat model
with each request.

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

The simulator build and tests do not test Bluetooth pairing. A physical iPhone
and integrated device firmware must pass these gates:

- The watch shows a new six-digit pairing code.
- iOS shows its system pairing prompt.
- The watch rejects a transfer before authenticated encrypted pairing.
- An accepted transfer returns a matching revision and fingerprint.
- A retry of the same transfer returns `unchanged` and causes no NVS write.
- A disconnect, timeout, bad packet, or wrong acknowledgement does not show
  success.
- Memory list pages use one stable revision and fingerprint. A conflict reloads
  the full list before another edit.
- A retried memory request does not cause a second write. A voice memory change
  causes one full refresh.
- Disconnect clears the memory view. Old firmware still accepts settings and
  shows the memory firmware-update message.
- Memory BLE work does not reset the idle timer or prevent normal sleep.
- The app restores secrets from Keychain after restart.
- The app restores non-secret choices and the revision from one preferences
  record.
- The app sends time and a rounded location after each watch wake and no more
  than once per hour during one connection.
- A denied location permission still sends time and uses the saved city
  fallback.
