# Architecture

## Runtime flow

The firmware uses one bounded state machine:

`idle -> recording -> transcribing -> thinking -> tool work -> speaking -> idle`

Each `app_main` start shows a full-boot splash with `CHAT ESP` and `STARTING`.
Power-button setup and the initial button sample occur first. The display then
flushes the splash while the AMOLED is off and raises the brightness only after
the complete black frame is ready. It sends a second complete frame and repeats
the selected brightness command after panel-on. This recovers a CO5300 that
accepts the first commands but does not show the first pixel transfer. The
runtime replaces the splash as soon as its task and button queue can accept
input. It does not use a minimum splash timer. An in-session display wake shows
the current interaction state instead.

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
held, microphone capture starts first. After 100 ms of audio arrives, a
low-priority worker starts Wi-Fi while capture continues. The capture task does
not run Wi-Fi setup.

The station scans all 2.4 GHz channels and selects the strongest matching
access point. It rejects access points below -75 dBm and retries the strongest
candidate before it starts the existing 10-second background retry. It then
uses modem power saving. Voice work uses the active Wi-Fi power mode. It stays
connected only during the active session.
Production uses a 30-second idle timer. Development uses a five-minute idle
timer so that a test result stays visible. Sleep stops the station. This gives
later requests in the same session a fast path without keeping the radio active
after the display turns off.

Each turn first uses a short required-tool route. The route is direct answer,
web search, image search, or one registered device tool. Image search still
needs a separate model-selected result ID. A relative brightness or volume
request gets device status before it changes the value. After routing and tool
work, the final model request has no tools.
OpenRouter sends that answer as an event stream. The UI receives bounded copies
of the complete answer so far. It limits display updates to keep the button and
network paths responsive. Tool-call data cannot enter the answer or speech
path. Only the final validated answer enters chat history.

The answer stream forms a segment at a question mark, exclamation mark,
newline, or safe period. After 96 bytes, a comma, semicolon, or colon is also a
safe boundary. A 160-byte hard limit splits at a complete UTF-8 word. The
speech path accepts at most four segments and 640 bytes. It wipes each segment
after use.

One TTS worker sends each complete segment to OpenRouter in FIFO order. One
playback task and codec session stay active for the full sequence. PCM goes
into one bounded 2.16 MB PSRAM ring. After a 9,600-byte sample, playback starts
early only when ingress has safe headroom above the 48,000-byte-per-second
playback rate. A slow first segment buffers completely. The next TTS request
can fill the ring while prior PCM plays. A response can retry only before PCM
playback starts. A button press cancels model, search, image, TTS, and playback
work and erases transient buffers.

HTTPS uses four bounded lanes: OpenRouter control, OpenRouter audio, Brave
search, and optional image download. Each lane keeps one client handle for one
HTTPS origin. Settings changes, Wi-Fi disconnect, sleep, cancellation, or a
transport error closes the handle. Request headers are removed before the next
request, and no authorization header can move to another origin.

The optional image worker starts after the model selects a current image ID.
It downloads and decodes on a lower-priority task while final model text and
speech continue. The device publishes the image only after speech ends.

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
- `provisioning`: versioned BLE packets, authenticated encrypted transfer,
  acknowledgement, and NVS persistence.
- `device preferences`: a small versioned brightness and volume record. It is
  separate from BLE settings and contains no secret data.
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

Version 1 has six tools:

- `search_web(query)`: returns a small list of titles, URLs, and snippets.
- `search_images(query)`: returns a small list with short result IDs.
- `get_device_status()`: returns brightness, volume, battery availability,
  preference-persistence state, and the current power-off mode.
- `set_brightness(percent)`: applies and stores a value from 5 through 100.
- `set_volume(percent)`: applies and stores a value from 0 through 100.
- `power_off()`: schedules power-off only after an explicit current request.

The device-control prompt does not infer power-off from a greeting, a farewell,
a hypothetical request, or an uncertain transcript. The final answer gives one
short confirmation before the runtime starts the normal sleep cleanup. A new
PWR-button action cancels a pending model power-off. Development mode enters
recoverable soft sleep. Production mode requests AXP2101 system-off. A bottom
PWR-button press wakes either mode; production wake is a cold start.

Brightness starts at 65 percent and volume starts at 70 percent when no valid
record exists. The record has a fixed eight-byte, version-1 format and strict
range checks. A changed value applies for the current session if NVS storage
fails. The tool result then reports that the value is temporary. Device-control
values contain no credentials or user text.

To show an image, the model must first search and then select one result by its
current ID. The firmware does not accept a URL from the model. A new search
removes the old result set, and a selection can be used only once.

After model selection, the device can get the selected Brave thumbnail from
the trusted `imgs.search.brave.com` proxy. It does not send a credential with
this request and it does not follow a redirect. The
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
reports the applied revision and fingerprint. Production stores the packet and
BLE bonds in plaintext NVS. This keeps settings after a restart, but a person
with physical flash access can read the credentials. Development keeps BLE
settings and bonds in memory. ChatESP never enables eFuse-backed NVS
encryption, flash encryption, secure boot, or another irreversible device
write. Each device build has an explicit zero policy flag and compile-time
checks. Logs show only redacted error categories. Turn timing records contain
phase durations and bounded counters only. They do not contain text, audio,
URLs, credentials, stable identifiers, or precise location.
