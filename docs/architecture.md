# Architecture

## Runtime flow

The firmware uses one bounded state machine:

`idle -> recording -> transcribing -> thinking -> tool work -> speaking -> idle`

Each state has a visible black-screen presentation and a timeout. An error
returns to idle with a short, useful message. Sleep stays available after a
failure.

One complete request has a 180-second monotonic limit. This limit includes
Wi-Fi connection, transcription, model calls, tool calls, and speech. Provider
retries use the remaining request time and wait 250 ms before one retry.

The bottom PWR-button duration selects the action. A press shorter than the
recording threshold requests sleep. A held press cancels active work or speech
and starts audio capture. Release ends the capture and submits it. A held wake
continues into audio capture. The exact threshold is a tested configuration
value.

The button poll runs separately from cloud work. A button press cancels active
audio and HTTPS work. A separate bounded task shows BLE passkeys, but a voice
button press always hides the passkey view.

When settings are available, the Wi-Fi station starts an asynchronous
connection as soon as the runtime starts. This does not block the button or
microphone path. A request waits for that connection or starts a bounded retry
if the early attempt did not finish. If the device starts with the PWR button
held, microphone capture starts first and Wi-Fi starts after release. This
keeps radio setup out of the first audio reads.

The station scans all 2.4 GHz channels and selects the strongest matching
access point. It rejects access points below -75 dBm and retries the strongest
candidate before it starts the existing 10-second background retry. It then
uses modem power saving. Voice work uses the active Wi-Fi power mode. It stays
connected only during the active session.
Production uses a 30-second idle timer. Development uses a five-minute idle
timer so that a test result stays visible. Sleep stops the station. This gives
later requests in the same session a fast path without keeping the radio active
after the display turns off.

OpenRouter sends model text as an event stream. The UI receives bounded copies
of the complete answer so far. It limits display updates to keep the button and
network paths responsive. Tool-call data does not go to the answer view. Only
the final validated answer enters chat history.

Speech PCM goes into a bounded PSRAM ring sized to the declared response or the
2.16 MB response cap. After a 9,600-byte sample, playback starts early only
when ingress has safe headroom above the 48,000-byte-per-second playback rate.
A slow response buffers completely to prevent codec underrun noise. HTTPS and
codec writes run on separate tasks. A button press cancels both tasks. An abort
stops playback and erases buffered audio. The provider does not retry after
playback starts.

The voice worker uses a bounded internal-RAM stack. Large request buffers stay
in PSRAM so flash operations remain safe and display DMA memory stays free.

Each accepted interaction resets the monotonic inactivity timer. Production
uses 30 seconds. Development uses five minutes. Chat messages stay in PSRAM
only. AXP2101 system-off clears them. A PWR-button wake causes a cold boot and
creates a new thread.

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
- `ui`: terminal layout, streamed text, Wi-Fi and battery footer, and
  state-specific motion.
- `provisioning`: versioned BLE packets, encrypted settings, acknowledgement,
  and NVS persistence.
- `power`: inactivity, PWR-button input, peripheral shutdown, AXP2101
  system-off, and cold-boot reset.

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
- `search_images(query)`: returns a small list with short result IDs.

To show an image, the model must first search and then select one result by its
current ID. The firmware does not accept a URL from the model. A new search
removes the old result set, and a selection can be used only once.

After it shows and speaks the text answer, the device can get the selected
Brave thumbnail from the trusted `imgs.search.brave.com` proxy. It does not
send a credential with this request and it does not follow a redirect. The
download has a 768 KiB limit and a 20-second provider limit inside the full
180-second interaction limit.

Version 1 accepts baseline sequential JPEG with dimensions up to 2,048 by
2,048. It rejects progressive JPEG, PNG, WebP, CMYK JPEG, and other unsupported
data. The ROM decoder scales and center-crops the image into one fixed 368 by
448 RGB565 PSRAM frame. It scales up a small image to cover the display. The
image stays on the full screen until the next button action or sleep. An image
error does not change a successful text or spoken answer.

Tools do not get credentials directly. Providers own credentials and HTTP
details.

## Secret lifecycle

Development secrets use an ignored local file. The iOS app stores secrets in
Keychain. BLE provisioning requires an authenticated encrypted link. The
firmware validates a full settings packet, writes only changed values, and
reports the applied revision and fingerprint. Device settings use encrypted
NVS. The production profile uses the ESP32-S3 HMAC NVS security provider. Its
first start can create the HMAC key in the configured eFuse key block. The
development profile keeps BLE settings in memory and does not make this eFuse
change. Logs show only redacted error categories.
