# Provisioning protocol

This document defines ChatESP BLE provisioning protocol version 3. All integer
values use network byte order. All text uses UTF-8. A length is a byte count.
The companion and this protocol are optional at runtime. The ChatESP device uses an
ignored local configuration in development or the last valid stored record in
production. Local Clock and controls do not require a provisioning record.
Cloud voice needs Wi-Fi and service credentials from one of these sources.

## BLE service

The device advertises the local name `ChatESP` and this primary service:

| Item | UUID | Property |
| --- | --- | --- |
| Provisioning service | `7B2E1000-6F3C-4B8A-9D71-4C4553500001` | Primary service |
| Control | `7B2E1001-6F3C-4B8A-9D71-4C4553500001` | Write with response |
| Data | `7B2E1002-6F3C-4B8A-9D71-4C4553500001` | Write with response |
| Acknowledgement | `7B2E1003-6F3C-4B8A-9D71-4C4553500001` | Indicate |
| Device context | `7B2E1004-6F3C-4B8A-9D71-4C4553500001` | Write with response |
| Memory command | `7B2E1005-6F3C-4B8A-9D71-4C4553500001` | Write with response |
| Memory response | `7B2E1006-6F3C-4B8A-9D71-4C4553500001` | Indicate |

The device must use LE Secure Connections bonding with man-in-the-middle
protection. It shows a new six-digit passkey on its display. iOS shows the
system pairing prompt. The firmware must check that the connection is
encrypted, authenticated, bonded, and uses LE Secure Connections before it
accepts a control, data, device-context, or memory write. It returns
`authentication_required` when the stack permits an application reply. It must
not parse or keep settings or context from an insecure link.

The iOS app cannot use a public Core Bluetooth API to inspect these link flags.
The device GATT permissions and the firmware check are authoritative. An iOS
write can cause the system pairing prompt.

## Transfer

The ChatESP device can stop BLE after a voice recording to release internal memory for
the cloud TLS request. This closes an active phone connection. The ChatESP device starts
advertising again when it returns to idle. The iOS app must treat this as a
normal, recoverable disconnect. A bounded reconnect retry scans for the saved
Core Bluetooth identifier before it starts a new connection. It must not
report a completed settings write unless it received the application
acknowledgement before the disconnect.

A development build keeps its bounded BLE bond store in RAM. It must retain
that store when it stops and restarts BLE during the same boot. A cold start
clears the development bond store. A production build keeps bonds in plaintext
NVS so the phone can reconnect after a cold start.

One transfer contains one complete settings packet. The maximum packet size is
1,024 bytes. iOS sends one control frame and then ordered data frames. It waits
for each write response before it sends the next frame.

The 16-byte control frame has this layout:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESB` |
| 4 | 1 | Protocol version, `3` |
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
| 4 | 1 | Protocol version, `3` |
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
and at most two complete transport attempts for each packet. A timeout or
disconnect is not a success. The app keeps the same revision and fingerprint
for a transport retry. One flagged revision-recovery response can start one
additional packet as specified below. Thus, one Save action sends at most two
packets and makes at most four complete transfer attempts. The app must not
start a second recovery for the same Save action.

## Settings packet

The packet has a 48-byte header:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESP` |
| 4 | 1 | Protocol version, `3` |
| 5 | 1 | Packet type, `1` for settings |
| 6 | 1 | Flags, zero |
| 7 | 1 | Field count, `11` |
| 8 | 4 | Revision, 1 through `0xffffffff` |
| 12 | 2 | TLV payload length |
| 14 | 2 | Complete packet length |
| 16 | 32 | Content fingerprint |

The payload contains all 11 fields once, in increasing field-ID order. Each
field has a one-byte ID, a two-byte length, and its text bytes. Unknown,
missing, repeated, or out-of-order fields are errors.

| ID | Setting | Limit and validation | Storage |
| ---: | --- | --- | --- |
| 1 | Chat endpoint | 12 through 192 visible ASCII bytes; HTTPS URL with a dotted host; no user information, query, or fragment | iOS preferences and plaintext device NVS |
| 2 | OpenRouter key | Empty, or 8 through 256 visible ASCII bytes | iOS Keychain and plaintext device NVS |
| 3 | Brave key | Empty, or 1 through 128 visible ASCII bytes | iOS Keychain and plaintext device NVS |
| 4 | Wi-Fi SSID | Empty, or 1 through 32 valid UTF-8 bytes | iOS Keychain and plaintext device NVS |
| 5 | Wi-Fi password | Empty, or 8 through 63 valid UTF-8 bytes | iOS Keychain and plaintext device NVS |
| 6 | Chat model | 1 through 96 ASCII letters, digits, `.`, `_`, `-`, `/`, `:`, or `~` | iOS preferences and plaintext device NVS |
| 7 | Transcription model | Same model rule | iOS preferences and plaintext device NVS |
| 8 | Speech model | Same model rule | iOS preferences and plaintext device NVS |
| 9 | Approximate location | Empty, or up to 96 valid UTF-8 bytes without control characters; use a city and country, not coordinates or a street address | iOS preferences and plaintext device NVS |
| 10 | English speech voice | 1 through 96 ASCII letters, digits, `.`, `_`, `-`, or `:` | iOS preferences and plaintext device NVS |
| 11 | French speech voice | Same voice rule | iOS preferences and plaintext device NVS |

An empty Brave key disables search. Empty OpenRouter and Wi-Fi credentials are
valid stored states. Cloud voice reports a runtime error until Wi-Fi and the
OpenRouter key are available. Empty endpoint, model, and voice edits in the iOS
app use the documented built-in defaults in the transmitted packet. UTF-8 must use the
shortest form and must not contain a null, surrogate, or value above `U+10FFFF`.

The content fingerprint is:

```text
SHA-256("CESP-CONTENT-V3" || version || packet_type || field_count || TLV_payload)
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
application acknowledgement. It saves the pending state only after it builds a
complete packet within the 1,024-byte limit.

If the iOS preferences record is lost, the first new packet can have an old or
conflicting revision. After the firmware fully validates that packet on an
authenticated encrypted link, a `stale_revision` or `revision_conflict`
acknowledgement supplies the active durable revision and fingerprint. The
active-version flag identifies this metadata. iOS saves the active metadata,
clears its pending metadata, and makes at most one recovery packet. That packet
has the same two-attempt transport bound. If the current content has the active
fingerprint, it uses the active revision and expects `unchanged`. If the content
is different, it uses the active revision plus one. It stops with
`revisionExhausted` when different content follows revision `0xffffffff`.

The recovery response contains only a revision and a SHA-256 fingerprint. It
does not contain settings. Firmware must not set the active-version flag for an
insecure link, an invalid packet envelope, another status, or absent durable
settings. iOS must reject a flagged zero revision. A response without the flag
is not recovery metadata. This rule prevents a response from old firmware from
being used as active metadata.

The iOS app keeps one non-secret device record for each added Core Bluetooth
identifier. Each new record has the default user-visible name `ChatESP`,
non-secret overrides, and its own acknowledged and pending revision state. The
same versioned preferences record contains the global non-secret values, all
device records, and the active-device identifier. A prior single-device record
migrates to this form. Credentials stay in Keychain. Keychain contains the
global secret values and optional secret overrides for each device.

The app saves each field edit to its local store at once. It does not validate
the complete settings packet before it saves another field. The app combines
the current global values and device overrides, applies endpoint, model, and voice
defaults to empty values, and starts an automatic transfer when each nonempty
value is valid. An invalid nonempty value stays local. The device-page status
identifies it. The app sends saved changes after the selected device connects.

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

The iOS app sends live context when the ChatESP device connects. It sends it again no
more than once per hour while the same connection stays active. The app uses
significant-location monitoring and requests one current location when it
connects. It rounds latitude and longitude to 0.1 degree before transfer. If
location permission is not available, the location is empty and the firmware
uses field 9 as the fallback. The firmware keeps live context in RAM. A context
sync does not write NVS and does not reset the ChatESP device idle timer.

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
location let it format the user's local date and time for model requests. The
Clock face also uses the epoch, seconds, and accepted app offset. Without live
app context, SNTP supplies UTC. One bounded IP-location request supplies a
coarse city, region, country code, and current UTC offset. A valid HTTP `Date`
response can also refresh UTC. A UTC-only source does not remove the last
accepted offset.

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

## Memory protocol version 1

The ChatESP device stores at most ten ordered facts. Each fact has a stable nonzero
32-bit ID and 1 through 128 valid UTF-8 bytes without control characters. The
ChatESP device is the source of truth. iOS shows the list only while the selected ChatESP device
is connected and does not save a mirror.

The 54-byte memory command header can have up to 128 fact bytes:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CEMC` |
| 4 | 1 | Memory protocol version, `1` |
| 5 | 1 | Operation |
| 6 | 2 | Flags, zero |
| 8 | 4 | Nonzero request ID |
| 12 | 4 | Expected revision |
| 16 | 32 | Expected content fingerprint |
| 48 | 4 | Memory ID or list cursor |
| 52 | 2 | Fact byte length |
| 54 | Length | Fact bytes |

Operations are `1` list page, `2` add, `3` delete, and `4` clear. An initial
list command has revision zero, a zero fingerprint, cursor zero, and no fact.
Each later page uses the revision and fingerprint from the first response. Its
cursor is the prior fact ID. The ChatESP device returns the first fact with a larger ID.
Add has a zero memory ID and one fact. Delete has one nonzero ID and no fact.
Clear has a zero ID and no fact.

The memory response has a 56-byte header and can have one fact:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CEMR` |
| 4 | 1 | Memory protocol version, `1` |
| 5 | 1 | Status |
| 6 | 1 | Operation |
| 7 | 1 | Bit 0 has fact, bit 1 has more, bit 2 is change event |
| 8 | 4 | Request ID |
| 12 | 4 | Current revision |
| 16 | 32 | Current content fingerprint |
| 48 | 4 | Returned or affected memory ID |
| 52 | 2 | Fact byte length |
| 54 | 1 | Total fact count |
| 55 | 1 | Reserved, zero |
| 56 | Length | Optional fact bytes |

Status values are `0x00` `applied`, `0x01` `unchanged`, `0x02` `full`, `0x03`
`not_found`, `0x04` `revision_conflict`, `0x10` `invalid_field`, `0x11`
`storage_failure`, `0x12` `authentication_required`, `0x13` `busy`, and `0x14`
`unsupported_version`. A GATT write response is not success. iOS waits up to
10 seconds and makes at most two attempts. A retry uses the exact same request
ID and bytes. The ChatESP device returns its cached response without another write.

The content fingerprint is:

```text
SHA-256("CHATESP-MEMORY-V1" || version || count || ordered entries)
entry = memory_id || fact_length || fact
```

All integers in the fingerprint use network byte order. A mutation requires an
exact current revision and fingerprint. iOS reloads the full list after a
conflict and after a successful mutation. A list conflict restarts at the first
page.

A voice tool change sends one coalesced change indication. It uses operation
zero, request ID zero, the change-event flag, and no fact. iOS then reloads the
complete list. Memory BLE work does not reset the idle timer and does not block
sleep.

Memory facts persist as one version-1 record with revision, next ID, ordered
entries, content fingerprint, and a full-record SHA-256 integrity value. The
firmware writes an inactive NVS slot, commits it, reads and validates the full
record, and then changes the active slot. A failed write keeps the old active
record. Development and production use separate plaintext NVS namespaces.
Facts and memory frames must not enter logs.

## Application acknowledgement

The device sends one 44-byte indication after it processes a complete packet.
A GATT write response is not provisioning success.

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `CESA` |
| 4 | 1 | Protocol version, `3` |
| 5 | 1 | Status |
| 6 | 2 | Flags; bit 0 means that error metadata is the active durable version |
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
error other than a flagged revision error after a valid packet header, the
device copies the packet revision and fingerprint. Otherwise, it sends zero for
both values. For a flagged `stale_revision` or `revision_conflict`, the device
copies the active durable revision and fingerprint and sets bit 0. All other
flag bits are zero. The acknowledgement must not contain field data or error
text.

## Compatibility

The firmware accepts version 1 and version 2 packets and frames so that old
stored settings remain valid. Version 1 has fields 1 through 8 and does not
have an approximate-location field. Version 2 has fields 1 through 9. They use
the `CESP-CONTENT-V1` and `CESP-CONTENT-V2` fingerprint domains. The firmware
uses `af_heart` and `ff_siwis` when either old version has no voice fields. The
iOS app sends version 3. All versions reject unknown frame versions,
operations, flags, packet types, fields, and acknowledgement sizes.

Version 2 firmware from before active-version recovery sends revision errors
with zero flags. The current iOS app reports the error and does not use that
metadata for recovery.

The memory characteristics are optional for compatibility. An old app can use
the first five characteristics with new firmware. The current app needs
version 3 firmware to send voice choices.

## Golden vector

Firmware and Swift tests use this non-secret vector:

- revision: `7`
- endpoint: `https://openrouter.ai/api/v1`
- OpenRouter key: `OPENROUTER_TOKEN_PLACEHOLDER`
- Brave key: `BRAVE_TOKEN_PLACEHOLDER`
- Wi-Fi SSID: `Test Network`
- Wi-Fi password: `PASSWORD_PLACEHOLDER`
- chat model: `~deepseek/deepseek-v4-flash-latest`
- transcription model: `openai/whisper-large-v3-turbo`
- speech model: `google/gemini-3.1-flash-tts-preview`
- approximate location: `Dubai, United Arab Emirates`
- English speech voice: `Zephyr`
- French speech voice: `Puck`
- complete packet length: `327`
- fingerprint: `1960e12e83ee87f7595f353d6c65b186c1348b0e136d76dd1dd02e9286b0b0e6`

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

The memory fingerprint cross-language vector is:

- entries: ID `2`, `User likes tea.`; ID `7`, `Use short answers.`
- fingerprint: `9b9a7dc2d6f8b263694fd1b4b02d675daa47cb56bb9d84b7219ee93309440d1d`
