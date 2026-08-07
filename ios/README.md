# ChatESP iOS companion

The companion app saves API keys and Wi-Fi details in Keychain. It saves the
endpoint, model choices, and provisioning revision in one versioned preferences
record. It sends one atomic settings packet over the authenticated encrypted
BLE provisioning service.

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
- The app restores secrets from Keychain after restart.
- The app restores non-secret choices and the revision from one preferences
  record.
