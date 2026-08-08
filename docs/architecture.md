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
returns to idle with a short message that identifies the failed operation. The
UI does not show internal numeric error categories. Sleep stays available after
a failure.

One complete request has a 180-second monotonic limit. This limit includes
Wi-Fi connection, transcription, model calls, tool calls, and speech. Provider
retries use the remaining request time and wait 250 ms before one retry.

The bottom PWR-button duration selects the action. A press shorter than the
recording threshold requests sleep. A held press cancels active work or speech
and starts audio capture. Release ends the capture and submits it. A held wake
continues into audio capture. The exact threshold is a tested configuration
value. At start, the power module uses the IO-expander level to detect a held
PWR key. During operation, it uses debounced AXP2101 PWR-key edge events. It
rejects a PWR-key event when the same PMU sample contains a USB power-source
event. A USB power-source change does not start or submit a recording.

The button poll runs separately from cloud work. A button press cancels active
audio and HTTPS work. A separate bounded task shows BLE passkeys. The passkey
view always uses the normal ChatESP orientation, including when Clock is
active. It restores the Clock orientation when it closes. A voice button press
always hides the passkey view.

A debounced top GPIO0 press from 80 through 700 ms changes between ChatESP and
Clock. A shorter electrical pulse or a longer press has no app action. A
top-button press has no effect during soft sleep, and GPIO0 is not a production
wake source. A switch to Clock cancels active voice work and clears the
in-memory thread. If local time is not ready, Clock keeps the startup Wi-Fi
connection for at most 15 seconds while NTP and the timezone lookup finish. It
stops Wi-Fi when local time becomes available or the limit expires. The board
adapter owns the GPIO0 pin value.

Clock uses LVGL software rotation to turn the UI 90 degrees counterclockwise.
The 448-by-368 layout puts the USB port at the bottom. A large, anti-aliased
Lato face shows 24-hour local time with tabular digits. A rounded white path
follows an inset rounded rectangle. Its 60 bounded sections fill clockwise on
even minutes and drain clockwise on odd minutes. The path starts at 12
o'clock. The clock style is one validated value with background, time,
seconds, radius, inset, and path-width fields. This keeps later phone
customization separate from the drawing code. The same quick-control panel
stays available in the rotated layout.

The top edge of the touch display has a small control handle. A tap or a
48-pixel downward swipe from the top 32 pixels opens a black control panel.
The top touch target keeps control until release, so the swipe can continue
beyond the small handle without losing the gesture.
The panel position follows the finger during a pull. A short settle animation
closes it when the release position is below half deployment and opens it at or
above half deployment. The display and touch refresh period is 16 ms. The
internal draw buffer holds 32 rows.
The panel has one shared five-percent control component for brightness and
volume. Each existing 320-by-44-pixel layout box has a centered transparent
352-by-64-pixel touch target. Thus, the rail, fill, dot, labels, and spacing do
not move. The dot shows the current value; the user does not have to touch it.
A press or a drag at any position in the larger row sets the value. An upward
swipe, a tap outside the panel, or five seconds without touch closes it. The
panel is not available during start, recording, sleep, or BLE passkey display.
A PWR-button action keeps priority. Opening the panel or changing a control
resets the idle timer only when the runtime is idle.

The 32-row LVGL draw and rotation buffers use DMA-capable internal memory.
They are allocated once during display start. The QSPI driver does not allocate
a large temporary DMA buffer while Wi-Fi and BLE are active. This keeps a
failed late allocation from leaving LVGL in a permanent flush wait.

The runtime initializes the ES8311 and reserves both I2S DMA channels before it
starts Wi-Fi or BLE. The smaller display buffer leaves enough internal memory
for the audio DMA channels and the Bluetooth controller at the same time. An
I2S or codec allocation failure returns an error. It does not stop or restart
the firmware.

During recording, the listening view analyzes only the latest 256 transient
PCM samples. One fixed FFT groups all positive-frequency bins into 18
continuous bands from 125 Hz through 8 kHz. It supplies one logarithmic level
for each band.
Rounded vertical bars rise quickly and fall smoothly, and a short peak marker
shows recent energy. The analyzer uses fixed stack values, does not allocate,
and does not keep or log audio outside the existing recording buffer.

After a recording ends, the runtime stops BLE before it starts the cloud
request. This releases the Bluetooth controller memory for TLS. Provisioning
starts again when the interaction ends and the runtime returns to idle. A phone
connection can close at the end of a recording. The app must treat this as a
normal disconnect and reconnect when the ChatESP device advertises again.
If the Bluetooth host does not stop, the runtime keeps its state intact and
does not start TLS work. BLE stop runs in a separate worker. The NimBLE
completion wait has a one-second limit. A timeout does not block sleep or a
PWR-button wake. A later stop can retry or collect the incomplete shutdown.

When no live app location is available, the runtime also stops BLE for one
short IP-context HTTPS lookup. This gives TLS the same controller-memory
headroom. The optional worker uses one fixed 20 KiB PSRAM stack because it does
not use DMA from the stack. Thus, its task can start without taking the internal
RAM that Wi-Fi and I2S need. The provider necessarily observes the public
source IP, but the request does not ask the provider to return it. The firmware
does not log or store the IP or the returned coarse location. BLE starts again
after the worker stack is reclaimed.

Control movement sends the latest brightness and volume values through the
same bounded callback. Volume uses safe atomic state at once, including during
speech. The runtime sends the latest brightness command. It retries the command
if a display refresh owns the display lock, and it does not move the visible
control back for this temporary condition. The LVGL callback does not write
flash, send a panel command, or wait for audio. The runtime saves the final
preference pair after release. It defers that write while voice work or a
PWR-button action has priority. It does not write NVS for each track position.

The iOS app is optional at runtime. When Wi-Fi credentials are available from
an ignored development configuration or a stored production record, the
station starts an asynchronous
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
Production ChatESP mode uses a 30-second idle timer. Development ChatESP mode
uses a five-minute idle timer so that a test result stays visible. Clock mode
resets that idle gate and stays on. A short PWR-button press can still request
sleep in either mode. Sleep stops the station.

Each turn first uses a short required-tool route. The route is direct answer,
web search, image search, restricted Python, memory management, or one
registered device tool.
Image search still needs a separate model-selected result ID. A relative
brightness or volume request gets device status before it changes the value.
After routing and tool work, the final model request has no tools.
The iOS companion can send its clock and current UTC offset when the ChatESP device
connects and at most once per hour while the connection stays active. The
firmware also starts a non-blocking SNTP sync with `time.cloudflare.com` after
Wi-Fi connects. It advances an accepted value with monotonic time while the
ChatESP device stays powered. The successful transcription response and the IP-context
response can also supply a standard HTTP `Date` header as a UTC fallback.
Each route and final-answer prompt gets the user's local minute, such as
`YYYY-MM-DD HH:MM UTC+04:00`. The prompt does not contain seconds, so requests
in the same minute use the same time value. A missing or invalid time stops the
turn instead of giving the model a false time.
Clock uses seconds from this same accepted time. An HTTP `Date` or NTP value
does not identify the user's timezone by itself. The app offset or the bounded
IP-location fallback supplies that offset. Before one source supplies both
parts, the face shows a time-unavailable pattern. The accepted value advances
with subtraction-based monotonic elapsed time and handles millisecond wrap.
Each model request also gets a fresh snapshot of all saved memory IDs and
facts. The prompt marks these facts as untrusted user-provided context. The
model must not follow instructions inside a fact. A successful memory tool
change is therefore visible to the final answer in the same turn. Saved facts
do not enter normal chat history.
OpenRouter sends that answer as an event stream. The UI receives bounded copies
of the complete answer so far. It limits display updates to keep the button and
network paths responsive. Tool-call data cannot enter the answer or speech
path. The model stream has a 128,000-byte response limit and a 16,384-byte
line limit. These limits include provider event framing. They do not increase
the answer, tool argument, or spoken-text limits. The answer starts with one
bounded internal language tag. The stream
parser removes this tag before the display, speech queue, and chat history. It
uses `af_heart` for an English Kokoro answer and `ff_siwis` for a French Kokoro
answer. A missing tag uses English for compatibility. An invalid language tag
stops the answer before it can reach the user. Only the final validated answer
enters chat history.

The answer stream forms a segment at a question mark, exclamation mark,
newline, or safe period. After 96 bytes, a comma, semicolon, or colon is also a
safe boundary. A 160-byte hard limit splits at a complete UTF-8 word. The
speech path accepts at most four segments and 640 bytes. It wipes each segment
after use.

One TTS worker sends each complete segment to OpenRouter in FIFO order. One
playback task and codec session stay active for the full sequence. The playback
task uses one fixed 16 KiB PSRAM stack so limited internal RAM cannot prevent
speech from starting. PCM goes into one bounded 2.16 MB PSRAM ring. After a
9,600-byte sample, playback starts early only when ingress has safe headroom
above the 48,000-byte-per-second playback rate. A slow first segment buffers
completely. The next TTS request can fill the ring while prior PCM plays. A
response can retry only before PCM playback starts. A button press cancels
model, search, image, TTS, and playback work and erases transient buffers.

HTTPS uses four bounded lanes: OpenRouter control, OpenRouter audio, Brave
search, and optional image download. Each lane keeps one client handle for one
HTTPS origin. Settings changes, Wi-Fi disconnect, sleep, cancellation, or a
transport error closes the handle. Request headers are removed before the next
request, and no authorization header can move to another origin.

The runtime keeps a selected image ID until speech playback has started. Speech
therefore reserves its task stack and speaker resources before the optional
image worker can use internal memory. The image worker then downloads and
decodes on a lower-priority task while speech continues. An image failure does
not stop speech. The device publishes the image only after speech ends.

Restricted Python runs in the voice worker before the final answer. It uses one
fixed 256 KiB PSRAM heap, a 12 KiB stack limit, a 2,048-byte output limit, a
1,024-byte source limit, a one-second wall-clock limit, and a 250,000-hook
operation limit. Bytecode jumps, returns, garbage collection, and iterator
steps check the limits and button cancellation. The interpreter has no file,
network, hardware, persistence, native-code, `eval`, or `exec` access. It wipes
the source, output, plot data, and heap after use. The tool keeps its JSON result
within 4,096 bytes. It marks printed text as truncated if JSON escaping cannot
fit the complete bounded output. A failed optional Python heap allocation
removes this tool but does not disable the other voice tools.

The voice worker uses a bounded internal-RAM stack. Large request buffers stay
in PSRAM so flash operations remain safe and display DMA memory stays free.

Each accepted interaction resets the monotonic inactivity timer. A voice
interaction sets a Clock-return gate. After 30 seconds of idle follow-up time,
the runtime enters Clock, clears the PSRAM thread, and stops Wi-Fi. Clock keeps
BLE available for time context and does not enter automatic sleep. A manual
ChatESP session with no voice interaction uses the normal production or
development sleep timer. AXP2101 system-off clears all volatile state. A
PWR-button wake causes a cold boot and creates a new thread.

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
- `ui`: terminal layout, rotated Clock face, streamed text, Wi-Fi and battery
  footer, and state-specific motion. It also owns the bounded top control panel
  and touch gesture presentation.
- `provisioning`: versioned BLE packets, authenticated encrypted transfer,
  acknowledgement, and NVS persistence.
- `device preferences`: a small versioned brightness and volume record. It is
  separate from BLE settings and contains no secret data.
- `memories`: a bounded versioned record, model tools, prompt context, and an
  optional connected iOS BLE manager. The ChatESP device is the source of truth.
- `power`: inactivity, PWR-button input, peripheral shutdown, AXP2101
  system-off, and cold-boot reset.

## iOS settings model

The companion keeps one global configuration and any number of ChatESP device
records. A device record contains a display name, optional non-secret
overrides, and independent provisioning revision state. Keychain contains the
global credentials and optional credential overrides for each device. The app
combines these two inheritance layers only when it builds a settings packet.
It does not build or send a packet after a Keychain read error. It retries the
read when the app becomes active. Thus, temporary Keychain unavailability
cannot become an empty credential update.

Each UI edit writes only its local non-secret or Keychain store at once. An
empty credential does not block another edit or an automatic BLE transfer.
Empty endpoint and model values use built-in defaults in the effective packet.
An invalid nonempty value stays local, and the device-page status identifies
the field. The app sends each valid effective configuration automatically after
connection or a settings change. The firmware receives one validated, atomic
settings packet. It reports a runtime error when a cloud action needs missing
Wi-Fi or OpenRouter credentials. An empty Brave key disables search.
An explicit device change cancels an active settings transfer before the app
changes the selected peripheral.

The model browser gets the bounded public OpenRouter all-modality catalog. It
does not send the saved API key for this public request. It filters chat
models for text input, text output, and tool calling. It filters transcription
models for audio input and transcription output. It filters speech models for
text input, speech output, and a published voice list. Each model result shows
the catalog input and output price. English and French voice browsers show the
voices published for the selected speech model. Model and voice values support
global inheritance and device overrides. Catalog or network failure does not
remove the saved selections or block other settings edits.

## Model contract

The system prompt tells the model to:

- answer for speech, with a natural tone;
- give the answer first;
- use one to three short sentences by default;
- reply in the language of the current user question;
- ask one short clarifying question when needed;
- avoid Markdown unless it materially helps the display;
- identify an English or French answer with one internal language tag;
- use a tool only when current or visual information is necessary;
- never expose tool protocol or hidden reasoning.

The route and answer system messages also contain the approximate user location
and the current user-local date and time at minute precision. After a ChatESP device is
selected, the companion sends a location rounded to 0.1 degree when the ChatESP device
connects and at most once per hour while connected. Without live app context,
the firmware makes one HTTPS request to `ipwho.is`. It requests only city,
region, country code, and UTC offset, and has a three-second total limit. A
worker-start failure stops this optional lookup for the current settings
session and restores BLE without a retry loop. A saved city and country is the
last fallback. Each live or IP value stays in RAM and is not written to flash.
It must not contain a precise position or a street address. The changing time
suffix follows the stable instruction text.

The firmware also sets a short output limit. The display can scroll, but the
normal answer must fit a spoken interaction.

## Tools

Version 1 has eleven tools:

- `search_web(query)`: returns a small list of titles, URLs, and snippets.
- `search_images(query)`: returns a small list with short result IDs.
- `get_device_status()`: returns brightness, volume, battery availability,
  preference-persistence state, and the current power-off mode.
- `set_brightness(percent)`: applies and stores a value from 5 through 100.
- `set_volume(percent)`: applies and stores a value from 0 through 100.
- `power_off()`: schedules power-off only after an explicit current request.
- `run_python(code)`: runs bounded MicroPython for short calculations. Printed
  text enters the tool result. `plot.line(x, y, title)` can select one line
  plot with 2 through 128 finite points and a title of at most 48 bytes.
- `remember_memory(fact)`: saves one concise fact only after an explicit user
  request. An exact duplicate returns `unchanged` without a write.
- `forget_memory(id)`: removes one fact by the ID in the current prompt.
- `clear_memories()`: removes all facts only after an explicit user request.
- `compact_memories(memories, include_pending)`: combines, shortens, or omits
  source facts. It runs automatically after a full `remember_memory` result,
  or when the user asks for compaction. The tool description states the
  ten-fact limit. With `include_pending` true, the model can return at most
  nine compacted entries so the pending fact has one free slot.

The memory store accepts at most ten facts and 128 UTF-8 bytes per fact. It
rejects empty text, control characters, invalid UTF-8, and revision exhaustion.
It does not reject a fact because of its content. The plaintext-storage and
model-sharing warnings apply to all facts. A full add keeps the new fact only
in a per-turn RAM buffer. One
successful compaction stores the rewritten facts and the pending fact in one
commit. A failure, cancellation, sleep, or turn end clears the buffer and keeps
the old durable record. Single-source unchanged facts keep their IDs. Changed
or combined facts get new IDs. Omitted source IDs are deleted.

The two firmware memory limits are adjacent constants. Record buffers, tool
schemas, and routing-prompt text derive from them. The iOS protocol mirror also
uses two adjacent constants, and its validation and user message derive from
those values.

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

A successful Python plot appears after the spoken answer. It uses a black
full-screen chart with a white line and bounded axis ranges. The plot stays
until the next button action or sleep. A plot has priority if one turn also
selects an image. A Python error or limit keeps the text answer available and
does not show partial plot data.

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
write. Each device build has an explicit zero policy flag. The PlatformIO
wrapper rejects unreviewed environments, project replacements, build-flag
overrides, duplicated policy flags, unsafe SDK settings, and direct first-party
eFuse write APIs. CMake requires a separate `FORBID` lock before configuration,
and source checks reject unsafe compiled settings. No user request, environment
variable, or approval flag can bypass this policy. Logs show only redacted
error categories. Turn timing records contain phase durations and bounded
counters only. They do not contain text, audio, URLs, credentials, stable
identifiers, or precise location.
Saved memories use a separate plaintext NVS namespace in development and
production. A two-slot commit verifies the complete inactive record before it
changes the active selector. Facts are never logged. A person with physical
flash access can read them. The iOS app does not save a mirror in preferences,
UserDefaults, Keychain, or another store.
