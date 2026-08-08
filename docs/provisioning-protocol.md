# Provisioning protocol

This document defines ChatESP BLE provisioning protocol version 2. All integer
values use network byte order. All text uses UTF-8. A length is a byte count.

## BLE service

The device advertises the local name `ChatESP Setup` and this primary service:

| Item | UUID | Property |
| --- | --- | --- |
| Provisioning service | `7B2E1000-6F3C-4B8A-9D71-4C4553500001` | Primary service |
| Control | `7B2E1001-6F3C-4B8A-9D71-4C4553500001` | Write with response |
| Data | `7B2E1002-6F3C-4B8A-9D71-4C4553500001` | Write with response |
| Acknowledgement | `7B2E1003-6F3C-4B8A-9D71-4C4553500001` | Indicate |
| Device context | `7B2E1004-6F3C-4B8A-9D71-4C4553500001` | Write with response |

The device must use LE Secure Connections bonding with man-in-the-middle
protection. It shows a new six-digit passkey on its display. iOS shows the
system pairing prompt. The firmware must check that the connection is
encrypted, authenticated, bonded, and uses LE Secure Connections before it
accepts a control, data, or device-context write. It returns
`authentication_required` when the stack permits an application reply. It must
not parse or keep settings or context from an insecure link.

The iOS app cannot use a public Core Bluetooth API to inspect these link flags.
The device GATT permissions and the firmware check are authoritative. An iOS
write can cause the system pairing prompt.

## Transfer

One transfer contains one complete settings packet. The maximum packet size is
1,024 bytes. iOS sends one control frame and then ordered data frames. It waits
for each write response before it sends the next frame.

The 16-byte control frame has this layout:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESB` |
| 4 | 1 | Protocol version, `2` |
| 5 | 1 | Operation: `1` begin, `2` cancel |
| 6 | 2 | Flags, zero |
| 8 | 4 | Random transfer ID |
| 12 | 2 | Complete packet length, 48 through 1,024 |
| 14 | 2 | Requested data bytes per frame, 1 through 180 |

A begin frame replaces an incomplete transfer on the same connection. A
cancel frame must have zero packet length and zero requested data length. A
disconnect deletes the incomplete packet. The device keeps at most one
transfer and 1,024 packet bytes in memory.

Each data frame has a 14-byte header followed by 1 through 180 packet bytes:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESD` |
| 4 | 1 | Protocol version, `2` |
| 5 | 1 | Flags, zero |
| 6 | 4 | Transfer ID from the begin frame |
| 10 | 2 | Offset in the complete packet |
| 12 | 2 | Data length |
| 14 | Data length | Packet bytes |

The first offset is zero. Each next offset is the sum of the prior offset and
data length. The frame length must equal 14 plus data length. Data length must
not exceed the requested data bytes from the begin frame. The final offset must
equal the packet length from the begin frame. The device rejects a wrong
transfer ID, gap, overlap, empty frame, excess data, bad flag, or bad length.

The iOS app uses 180 data bytes per frame, a 10-second acknowledgement timeout,
and at most two complete transfer attempts. A timeout or disconnect is not a
success. The app keeps the same revision and fingerprint for a retry.

## Settings packet

The packet has a 48-byte header:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESP` |
| 4 | 1 | Protocol version, `2` |
| 5 | 1 | Packet type, `1` for settings |
| 6 | 1 | Flags, zero |
| 7 | 1 | Field count, `9` |
| 8 | 4 | Revision, 1 through `0xffffffff` |
| 12 | 2 | TLV payload length |
| 14 | 2 | Complete packet length |
| 16 | 32 | Content fingerprint |

The payload contains all nine fields once, in increasing field-ID order. Each
field has a one-byte ID, a two-byte length, and its text bytes. Unknown,
missing, repeated, or out-of-order fields are errors.

| ID | Setting | Limit and validation | Storage |
| ---: | --- | --- | --- |
| 1 | Chat endpoint | 12 through 192 visible ASCII bytes; HTTPS URL with a dotted host; no user information, query, or fragment | iOS preferences and plaintext device NVS |
| 2 | OpenRouter key | 8 through 256 visible ASCII bytes | iOS Keychain and plaintext device NVS |
| 3 | Brave key | Empty, or 1 through 128 visible ASCII bytes | iOS Keychain and plaintext device NVS |
| 4 | Wi-Fi SSID | 1 through 32 valid UTF-8 bytes | iOS Keychain and plaintext device NVS |
| 5 | Wi-Fi password | 8 through 63 valid UTF-8 bytes | iOS Keychain and plaintext device NVS |
| 6 | Chat model | 1 through 96 ASCII letters, digits, `.`, `_`, `-`, `/`, or `:` | iOS preferences and plaintext device NVS |
| 7 | Transcription model | Same model rule | iOS preferences and plaintext device NVS |
| 8 | Speech model | Same model rule | iOS preferences and plaintext device NVS |
| 9 | Approximate location | Empty, or up to 96 valid UTF-8 bytes without control characters; use a city and country, not coordinates or a street address | iOS preferences and plaintext device NVS |

An empty Brave key disables search. Empty values are not valid for other
fields. UTF-8 must use the shortest form and must not contain a null, surrogate,
or value above `U+10FFFF`.

The content fingerprint is:

```text
SHA-256("CESP-CONTENT-V2" || version || packet_type || field_count || TLV_payload)
```

The fingerprint does not include the revision. Both implementations compare
all 32 bytes. The firmware uses a constant-time comparison.

## Revision and storage rules

The first revision is 1. A settings change uses the last acknowledged revision
plus one. Revision zero is stale.

- A lower revision is `stale_revision`.
- The current revision with the current fingerprint is `unchanged`.
- The current revision with a different fingerprint is `revision_conflict`.
- A higher revision with a valid packet can be applied.

The iOS app saves a pending revision and fingerprint before transfer. It keeps
them for a retry. It marks them as acknowledged only after a successful
application acknowledgement.

The firmware validates the complete packet before a persistent write. It then
writes the changed settings and metadata to plaintext NVS as one logical
transaction. It must verify durable storage before it sends `applied`. A repeat
with the same revision and fingerprint sends `unchanged` and causes no NVS
write. A validation or storage error must not make a partial setting set active.
The production profile does not use the ESP32-S3 HMAC NVS security provider.
It must not enable NVS encryption because HMAC key setup can burn an eFuse.
This means a person with physical flash access can read stored credentials.
The same person can read the optional approximate location. The setting must
not contain precise coordinates or a street address.
The development profile does not claim durable provisioning.

## Live device context

The iOS app sends live context when the watch connects. It sends it again no
more than once per hour while the same connection stays active. The app uses
significant-location monitoring and requests one current location when it
connects. It rounds latitude and longitude to 0.1 degree before transfer. If
location permission is not available, the location is empty and the firmware
uses field 9 as the fallback. The firmware keeps live context in RAM. A context
sync does not write NVS and does not reset the watch idle timer.

The context packet is 49 through 145 bytes:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESC` |
| 4 | 1 | Context version, `1` |
| 5 | 1 | Flags, zero |
| 6 | 8 | Unix epoch seconds from the iPhone |
| 14 | 2 | Signed UTC offset in minutes, -840 through 840 |
| 16 | 32 | Context fingerprint |
| 48 | 1 | Approximate-location byte length, 0 through 96 |
| 49 | Length | Valid UTF-8 location without control characters |

The context fingerprint is:

```text
SHA-256("CESP-CONTEXT-V1" || version || epoch_seconds || utc_offset_minutes || location_length || location)
```

All numeric fingerprint inputs use network byte order. The firmware accepts
epoch values from 2020-01-01 through 9999-12-31. The UTC offset and rounded
location let it format the user's local date and time without seconds. A valid
HTTP `Date` response can refresh the UTC clock, but it does not remove the last
accepted app offset.

The device sends a 48-byte context indication on the acknowledgement
characteristic. A GATT write response is not context-sync success.

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESR` |
| 4 | 1 | Context version, `1` |
| 5 | 1 | Status |
| 6 | 2 | Flags, zero |
| 8 | 8 | Accepted or rejected epoch seconds |
| 16 | 32 | Context fingerprint |

Context status uses `applied`, `authentication_required`,
`unsupported_version`, `malformed_packet`, `invalid_field`, and `busy` from the
application acknowledgement status table. For `applied`, iOS requires an exact
epoch and fingerprint match.

## Application acknowledgement

The device sends one 44-byte indication after it processes a complete packet.
A GATT write response is not provisioning success.

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESA` |
| 4 | 1 | Protocol version, `2` |
| 5 | 1 | Status |
| 6 | 2 | Flags, zero |
| 8 | 4 | Applied or rejected revision |
| 12 | 32 | Content fingerprint |

Status values are:

| Value | Name | Meaning |
| ---: | --- | --- |
| `0x00` | `applied` | Durable settings are active |
| `0x01` | `unchanged` | The same durable settings were already active |
| `0x10` | `authentication_required` | The link did not meet the security contract |
| `0x11` | `unsupported_version` | A frame or packet version is not supported |
| `0x12` | `malformed_transfer` | A control or data frame is invalid |
| `0x13` | `malformed_packet` | The packet envelope, field order, or fingerprint is invalid |
| `0x14` | `invalid_field` | A settings value is invalid |
| `0x15` | `stale_revision` | The revision is lower than the active revision |
| `0x16` | `revision_conflict` | The active revision has different content |
| `0x17` | `storage_failure` | Durable storage did not complete |
| `0x18` | `busy` | Another accepted settings transaction is active |

For `applied` and `unchanged`, revision and fingerprint must match the packet.
iOS rejects a success status with a different revision or fingerprint. For an
error after a valid packet header, the device copies its revision and
fingerprint. Otherwise, it sends zero for both values. The acknowledgement must
not contain field data or error text.

## Compatibility

The firmware accepts version 1 packets and frames so that stored version 1
settings remain valid. Version 1 has fields 1 through 8, uses the
`CESP-CONTENT-V1` fingerprint domain, and gets a version 1 acknowledgement. It
does not have an approximate-location field. The iOS app sends version 2.
Both versions reject unknown frame versions, operations, flags, packet types,
fields, and acknowledgement sizes.

## Golden vector

Firmware and Swift tests use this non-secret vector:

- revision: `7`
- endpoint: `https://openrouter.ai/api/v1`
- OpenRouter key: `OPENROUTER_TOKEN_PLACEHOLDER`
- Brave key: `BRAVE_TOKEN_PLACEHOLDER`
- Wi-Fi SSID: `Test Network`
- Wi-Fi password: `PASSWORD_PLACEHOLDER`
- chat model: `deepseek/deepseek-v4-flash`
- transcription model: `openai/whisper-large-v3-turbo`
- speech model: `google/gemini-3.1-flash-tts-preview`
- approximate location: `Dubai, United Arab Emirates`
- complete packet length: `303`
- fingerprint: `09fe4fdf6757295ba4960dccbf729df3a2efe5a3acfe2eca61a5335594d27ba0`

Tests also cover insecure links, truncated and excess packets, bad magic,
unknown versions, flags, lengths, fingerprints, UTF-8, field limits, field
order, repeated fields, missing fields, stale revisions, conflicts, and
unchanged retries.

The live-context cross-language vector is:

- epoch seconds: `1786147200`
- UTC offset minutes: `240`
- approximate location: `latitude 25.2, longitude 55.3`
- complete packet length: `78`
- fingerprint: `d3a73c211aa2d932725513317cabdccd55494cd56614c9232c175acce58ccdb8`
