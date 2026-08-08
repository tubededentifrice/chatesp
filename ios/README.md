# ChatESP iOS companion

The companion app saves API keys and Wi-Fi details in Keychain. It saves the
endpoint, model choices, optional city-level location, and provisioning
revision in one versioned preferences record. The record also contains the
non-secret Core Bluetooth identifier of the selected watch. The app restores
this selection after iOS restarts it for Bluetooth work. Scan, connect,
reconnect, frame, and confirmation operations have fixed time limits. Use
**Stop and Forget Watch** to stop this work and remove the saved selection.

The app sends one atomic settings packet over the authenticated encrypted BLE
provisioning service. If the app lost its local revision record, current
firmware can return the active revision and fingerprint in a flagged error.
The app saves that metadata and makes one bounded recovery transfer. It does
not use an unflagged error as recovery metadata.

After a watch is selected, the app also sends the current time, UTC offset,
and a position rounded to 0.1 degree when the watch connects and at most once
per hour while connected. Significant-location monitoring runs only when the
user already gave Always location access. With While Using the App access,
the app requests one location while it is active. The live location stays in
memory and is not saved in the preferences record.

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
- A lost iOS revision record causes at most one recovery packet. Matching
  content returns `unchanged`. Changed content uses the next revision.
- A scan stops after 10 seconds. A connection attempt stops after 10 seconds.
- A frame with no write response fails the connection. Only a missing final
  acknowledgement can cause one complete-transfer retry.
- iOS restores only the saved watch after a background restart. A callback
  from another watch does not change the active transfer.
- **Stop and Forget Watch** stops scan, reconnect, location, and BLE work. The
  app does not reconnect until the user selects a watch again.
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
- The installed app starts at the full native screen size and does not use a
  compatibility-size canvas.
- The app sends time and a rounded location after each watch wake and no more
  than once per hour during one connection.
- A denied location permission still sends time and uses the saved city
  fallback.
