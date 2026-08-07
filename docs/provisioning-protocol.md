# Provisioning protocol

This document is the authoritative location for BLE service UUIDs, packet
encoding, field limits, revision rules, fingerprints, acknowledgements, and
errors.

The protocol is not yet implemented. The firmware and iOS task must define the
first complete version here before it adds code. Firmware and Swift tests must
use shared golden packet vectors.

## Required version 1 properties

- Require an authenticated encrypted BLE link.
- Validate the version, exact packet length, each field, the revision, and the
  fingerprint before any persistent change.
- Apply all settings as one transaction. Do not make a partial update.
- Write only changed values. A repeated packet with the same content must not
  cause another NVS write.
- Report success only after durable storage. The application acknowledgement
  must include the version, applied revision, and matching fingerprint.
- Reject stale revisions and one revision with different content.
- Bound each string, the full packet, retries, and acknowledgement time.
- Store API and Wi-Fi secrets in iOS Keychain and encrypted device NVS. Store
  non-secret iOS choices in a versioned preferences record.
- Never put packet values or secrets in logs.

The implementation task must replace this design outline with the exact binary
layout, UUIDs, algorithms, defaults, error values, and compatibility rules.
