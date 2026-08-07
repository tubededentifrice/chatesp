# Architecture

## Runtime flow

The firmware uses one bounded state machine:

`idle -> recording -> transcribing -> thinking -> tool work -> speaking -> idle`

Each state has a visible black-screen presentation and a timeout. An error
returns to idle with a short, useful message. Sleep stays available after a
failure.

The top-button duration selects the action. A press shorter than the recording
threshold requests sleep. A held press starts audio capture. Release ends the
capture and submits it. The exact threshold is a tested configuration value.

Each accepted interaction resets a 30-second monotonic inactivity timer. Chat
messages stay in PSRAM only. PMU system-off clears the messages. A cold-boot
wake creates a new thread.

## Modules

- `board`: official BSP setup, board revision, buttons, display, touch, codec,
  power, and wake source.
- `audio`: bounded PCM capture and playback. Raw request audio is transient.
- `transport`: HTTPS requests, TLS validation, limits, cancellation, and JSON.
- `providers`: OpenRouter chat, transcription, and speech plus replaceable
  search providers.
- `tools`: a small registry, JSON-schema arguments, result limits, and a fixed
  maximum tool-round count.
- `conversation`: system prompt, short in-memory history, tool loop, and thread
  lifetime.
- `ui`: terminal layout and state-specific motion.
- `provisioning`: versioned BLE packets, encrypted settings, acknowledgement,
  and NVS persistence.
- `power`: inactivity, cancel, peripheral shutdown, deep sleep, and wake reset.

## Model contract

The system prompt tells the model to:

- answer for speech, with a natural tone;
- give the answer first;
- use one to three short sentences by default;
- avoid Markdown unless it materially helps the display;
- use a tool only when current or visual information is necessary;
- never expose tool protocol or hidden reasoning.

The firmware also sets a short output limit. The display can scroll, but the
normal answer must fit a spoken interaction.

## Tools

Version 1 has two tools:

- `search_web(query)`: returns a small list of titles, URLs, and snippets.
- `search_images(query)`: returns a small list of image URLs and thumbnails.

The model selects a result. The device can download one bounded image, decode
it, and show it full-screen. Tools do not get credentials directly. Providers
own credentials and HTTP details.

## Secret lifecycle

Development secrets use an ignored local file. The iOS app stores secrets in
Keychain. BLE provisioning requires an authenticated encrypted link. The
firmware validates a full settings packet, writes only changed values, and
reports the applied revision and fingerprint. Device settings use encrypted
NVS. Logs show only redacted error categories.
