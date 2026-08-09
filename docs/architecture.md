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
startup path builds hidden runtime views only after this reliable splash
transfer. It loads saved memories after the splash is visible. The rounded
Clock path uses double-precision geometry and a dedicated point buffer.
Firmware allocates and builds them only on the first Clock entry, so this
unrelated work does not delay voice-runtime readiness.
Production also skips the optional full-PSRAM start test and uses a
speed-optimized bootloader. These changes do not change system-off or steady
active power policy. The runtime replaces the splash as soon as its task and
button queue can accept input. It does not use a minimum splash timer. An
in-session display wake shows the current interaction state instead.
Development soft sleep keeps the CO5300 initialized at its current brightness
and covers the screen with one full black frame. Wake removes this frame and
does not depend on a zero-to-nonzero brightness transition. Each deliberate
in-session wake also replays the bounded CO5300 initialization table, restores
the selected brightness, and sends one complete frame. It does not reset the
panel or CST820 touch controller. This recovers a panel that accepts commands
but keeps its output stage black.

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
value. AXP2101 register `0x27` uses its minimum 128-millisecond PWR-on
recognition time. Thus, a system-off press starts the power rails without a
short-versus-hold classification delay. This reversible setting does not keep
the processor or another switched rail on during system-off. At start, the
power module uses the IO-expander level to detect a held PWR key. A completed
AXP2101 release or short-press event overrides a latched IO-expander level.
Thus, a short battery wake does not become a voice hold.
During operation, it uses debounced AXP2101 PWR-key edge events. It
rejects a PWR-key event when the same PMU sample contains a USB power-source
event. A USB power-source change does not start or submit a recording.

The button poll runs separately from cloud work. A button press cancels active
audio and HTTPS work. A separate bounded task shows BLE passkeys. An incoming
pairing request wakes the AMOLED before it shows the code. The passkey view
always uses the normal ChatESP orientation, including when Clock is active. It
restores the Clock orientation when it closes. A voice button press always
hides the passkey view.

A debounced top GPIO0 press from 30 through 700 ms changes between ChatESP and
Clock. A shorter electrical pulse or a BOOT-only longer press has no app
action. A top-button press has no effect during soft sleep, and GPIO0 is not a
production wake source. This short BOOT-button press is the only action that
enters Clock. A separate recovery button combination tracks both buttons. When
the debounced PWR and BOOT states stay pressed together for five seconds, the
main button poll records the controlled event and calls the ESP software-restart
path. Releasing either button resets the timer. A PWR-only or BOOT-only hold
does not start this restart path. The check runs outside the voice and display
tasks, so it can recover a black or unresponsive display while the main button
poll still runs.
A switch to Clock cancels active voice work and clears the
in-memory thread. If local time is not ready, Clock keeps the startup Wi-Fi
connection for at most 15 seconds while NTP and the timezone lookup finish. It
stops Wi-Fi when local time becomes available or the limit expires. The board
adapter owns the GPIO0 pin value.

Clock uses LVGL software rotation to turn the UI 90 degrees counterclockwise.
The 448-by-368 layout puts the USB port at the bottom. A large, anti-aliased
Lato face shows 24-hour local time with tabular digits. A rounded white path
uses the outermost valid pixels on all four display edges. The path is a plain,
one-pixel line. Its endpoint advances by one distinct display pixel at a time
on even minutes. It removes one pixel at a time in the same direction on odd
minutes. A 16 ms timer keeps each pixel change independent from the one-second
time update. The path starts at 12 o'clock. The clock style is one validated
value with background, time, seconds, and radius fields. This keeps later phone
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

When an authenticated iPhone subscribes to the phone proxy, the runtime keeps
BLE active and keeps Wi-Fi off. It sends each bounded HTTPS request envelope to
the app with notifications. The app uses an ephemeral URL session and returns
bounded response data with write-without-response flow control. It uses
confirmed writes for the response boundaries. For bounded PCM with a declared
length, it forwards BLE data while the HTTPS body arrives. This path keeps the
saved bond active and makes the phone the device network path.

The button-wake path starts BLE before the hold becomes a recording. If a
complete saved bond is present, the runtime keeps BLE active for the full
recording. It gives the phone a two-second reconnect limit after the button is
released. Thus, a long recording does not consume the phone reconnect limit.
If the secure phone proxy is still not ready, the runtime stops BLE, protects
its restart block, and starts Wi-Fi. This releases the remaining Bluetooth
memory for TLS. Provisioning starts again when the interaction ends and the
runtime returns to idle. Before the controller restarts, the runtime releases
completed TLS sessions and temporarily shuts down Wi-Fi. It starts BLE only
when internal memory has one 30 KiB block and at least 48 KiB free. ChatESP
idle keeps Wi-Fi off. This avoids the controller assertion that occurs if the
vendor stack cannot allocate its internal block. Wi-Fi shutdown deinitializes
the driver and releases its DMA buffers. Stopping only the radio does not
release enough contiguous memory. The app treats this fallback disconnect as
normal and reconnects when the device advertises again.

The app scans for the selected device for 30 seconds at a time. Consecutive
scan windows have a one-second bounded gap. Thus, a device wake does not fall
inside a long reconnect backoff while the app is active.

The ESP-IDF automatic connection reattempt is disabled. On the ESP32-S3 it can
retry a failed peripheral connection with an invalid advertising parameter and
leave the device awake but not advertising. ChatESP receives the failed link
event and starts its complete advertising record again.

A failed-connection callback can arrive while the host is stopping. The stop
state blocks this callback from starting advertising. Outside shutdown, a
failed advertising start uses five host-task retries at 100-millisecond
intervals. If they all fail, the runtime performs one complete BLE host restart
from its idle state. This prevents a transient host memory or procedure state
from requiring another PWR cycle.

Development sleep keeps the CO5300 initialized and covers the UI with one full
black frame. It does not send brightness zero. A successful CO5300 command does
not prove that pixels are visible after a zero-brightness interval. Production
uses brightness zero only when its cancel window has ended and system-off is
the next state. The system-off request enables the AXP2101 internal discharge
path for each switched DCDC and LDO output. Thus, stored charge or an IO path
has less time to keep a peripheral partly powered after the PMIC turns the rail
off.

After BLE stops for a cloud request, the runtime reserves the controller restart
block before TLS starts. The request can use the rest of the released Bluetooth
memory, but it cannot split this block. After the runtime closes TLS and fully
deinitializes Wi-Fi, it releases the block immediately before BLE starts.

General allocations larger than 512 bytes prefer PSRAM. Hardware paths that
need internal or DMA-capable memory request it explicitly. This policy keeps
small control objects fast but reduces TLS and response pressure on internal
memory.

If the runtime cannot reserve the BLE restart block before a cloud request, it
records a diagnostic event and performs a controlled software restart. The
restart keeps settings and the BLE bond. This recovery prevents a live device
from remaining awake without BLE or entering the controller with unsafe memory.
If the Bluetooth host does not stop, the runtime keeps its state intact and
does not start TLS work. BLE stop runs in a separate worker. The NimBLE
completion wait has a one-second limit. A timeout does not block sleep or a
PWR-button wake. A later stop can retry or collect the incomplete shutdown.

The runtime does not stop active BLE for the optional IP-context HTTPS lookup.
A phone can be connecting before the firmware receives a GATT event, and that
lookup must not interrupt pairing or bond restoration. The optional lookup can
run only during another operation that already stopped BLE. Its worker uses one
fixed 20 KiB PSRAM stack because it does not use DMA from the stack. The
provider necessarily observes the public source IP, but the request does not
ask the provider to return it. The firmware does not log or store the IP or the
returned coarse location.

Control movement sends the latest brightness and volume values through the
same bounded callback. Volume uses safe atomic state at once, including during
speech. The runtime sends the latest brightness command. It retries the command
if a display refresh owns the display lock, and it does not move the visible
control back for this temporary condition. The LVGL callback does not write
flash, send a panel command, or wait for audio. The runtime saves the final
preference pair after release. It defers that write while voice work or a
PWR-button action has priority. It does not write NVS for each track position.

The iOS app is optional at runtime. ChatESP idle keeps BLE available and Wi-Fi
off. After 100 ms of a valid recording arrives, a low-priority worker selects
the secure phone proxy when it is ready. Otherwise, it stops BLE, protects the
restart block, and starts Wi-Fi while capture continues. The capture task does
not run radio setup. This order prevents idle Wi-Fi allocations from splitting
memory that a later BLE restart needs.

Production reads and applies the last valid NVS settings record before it can
process a startup button command. A held cold wake therefore has its saved
service key, model choices, and Wi-Fi values before recording starts. The
voice-runtime task applies this record as its first operation. This keeps the
complete settings record off the smaller main startup stack. The 500-millisecond
idle check applies later BLE settings changes.

The production image uses 80 MHz QIO flash, a 240 MHz CPU, a
speed-optimized bootloader, and no optional full-PSRAM start test. It keeps
application-image validation and the startup low-battery gate. These checks
protect recovery and the battery. The firmware reads the AXP2101 PWR-on setting
on each start. If necessary, it programs and reads back the 128-millisecond
value. The setting stays through normal system-off. A complete PMIC power loss
can restore its factory PWR-on time for the first start after power returns.
That start applies the fast value again.

The station scans all 2.4 GHz channels and selects the strongest matching
access point. It rejects access points below -75 dBm and retries the strongest
candidate before it starts the existing 10-second background retry. It then
uses modem power saving. Voice work uses the active Wi-Fi power mode. It stays
connected only during the active session. The footer shows the active secure
BLE link instead of Wi-Fi off. Connected Wi-Fi uses three RSSI bands. The exact
RSSI does not enter a log path.
ChatESP mode uses a 30-second idle timer in development and production. Clock
mode resets that idle gate and stays on. A short PWR-button press can still
request sleep in either mode. Sleep stops BLE and Wi-Fi. The app reports the
device as asleep or unavailable while it runs bounded reconnect attempts.
Battery-powered production normally stops when the AXP2101 accepts system-off.
USB can keep the ESP32 powered after that request. In this condition,
production stays in the completed sleep state and repeats the system-off
request once per second. A bottom PWR press cancels this state and wakes the
app. If USB is removed, the next request turns the main rails off. Production
does not restore the full runtime only because USB held the power rail on.
While the display is active, the runtime reads the battery at most once every
30 seconds or after a power-source event. At or below 5 percent, it requests
system-off only when VBUS is not good and the PMU does not report active
charging. This protection uses system-off in development
and production. The runtime does not read the battery after sleep starts.

Each turn first uses a short required-tool route. The route is direct answer,
web search, image search, restricted Python, memory management, or one
registered device tool.
Image search still needs a separate model-selected result ID. A relative
brightness or volume request gets device status before it changes the value.
Web search and restricted Python each stop tool routing after one call. Thus,
the model cannot repeat a completed call and reach the tool-step limit.
After routing and tool work, the final model request has no tools.
The iOS companion can send its clock and current UTC offset when the ChatESP device
connects and at most once per hour while the connection stays active. The
firmware also starts a non-blocking SNTP sync with `time.cloudflare.com` after
Wi-Fi connects. It advances an accepted value with monotonic time while the
ChatESP device stays powered. The successful transcription response and the IP-context
response can also supply a standard HTTP `Date` header as a UTC fallback.
Each route and final-answer prompt gets the user's local weekday and minute,
such as `YYYY-MM-DD HH:MM UTC+04:00 (Saturday)`. The prompt does not contain
seconds, so requests in the same minute use the same time value. A missing or
invalid time or a weekday that does not match the date stops the turn instead
of giving the model false time context.
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

The 18-pixel answer font contains Latin Extended characters and the supported
Unicode General Punctuation glyphs. Common model punctuation, such as smart
quotation marks, long dashes, bullets, and ellipses, appears as text instead of
a missing-glyph box.

The answer stream starts the first speech request at a question mark,
exclamation mark, newline, or safe period. A 160-byte limit splits a long first
sentence at a complete UTF-8 word. The stream then keeps all remaining spoken
text until the final answer is valid. It sends that text in one second request.
The speech path accepts at most two requests and 640 bytes. It wipes each text
buffer after use.

One TTS worker sends the first request, then the complete remainder. One
playback task and codec session stay active for the full sequence. The playback
task uses one fixed 16 KiB PSRAM stack so limited internal RAM cannot prevent
speech from starting. PCM goes into one bounded 2.16 MB PSRAM ring. A fast
transfer can start after a 9,600-byte sample. A slower initial transfer gets a
second rate check with 24,000 buffered bytes. It starts when ingress has safe
headroom above the 48,000-byte-per-second playback rate. A transfer that is
still too slow buffers completely. The second TTS request can fill the ring
while prior PCM plays. A request can retry while none of its own PCM has
reached playback. A button press cancels model, search, image, TTS, and
playback work and erases transient buffers.

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

Each accepted interaction resets the monotonic inactivity timer. After 30
seconds of idle ChatESP time, the runtime requests sleep, clears the PSRAM
thread, and stops Wi-Fi. Clock keeps BLE available for time context and does
not enter automatic sleep. Only a short BOOT-button press enters Clock.
AXP2101 system-off clears all volatile state. A PWR-button wake causes a cold
boot and creates a new thread.

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
- `ui`: terminal layout, rotated Clock face, streamed text, radio and battery
  footer, and state-specific motion. The radio footer shows the active Wi-Fi or
  secure BLE link and three Wi-Fi signal levels. The battery footer becomes
  green and adds a charge mark while battery current flows in the charge
  direction. Development builds show a small `DEV` marker at the footer center.
  Production builds omit this marker. The UI also owns the bounded top control
  panel and touch gesture presentation.
- `provisioning`: versioned BLE packets, authenticated encrypted transfer,
  acknowledgement, and NVS persistence.
- `device preferences`: a small versioned brightness and volume record. It is
  separate from BLE settings and contains no secret data.
- `memories`: a bounded versioned record, model tools, prompt context, and an
  optional connected iOS BLE manager. The ChatESP device is the source of truth.
- `power`: inactivity, PWR-button input, peripheral shutdown, AXP2101
  system-off, and cold-boot reset.
- `simulator`: desktop adapters around the portable app, provisioning, and BLE
  cores. It models deterministic product input, pairing state, phone retries,
  link faults, storage faults, and bounded malformed-frame tests.

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
the field. The app sends each changed valid effective configuration
automatically. After a confirmed packet, it does not send the same fingerprint
again for 10 minutes. An unchanged reconnect gives the phone proxy two seconds
before an eligible refresh starts. The app retries a failed automatic transfer
after 30 seconds while the link stays ready. These intervals use a monotonic
clock. The firmware receives one
validated, atomic settings packet. It reports a runtime error when a cloud
action needs missing Wi-Fi or OpenRouter credentials. An empty Brave key
disables search.
The settings, device-context, and memory responses use one serialized ATT
indication path. A memory response cannot discard a settings acknowledgement.
The runtime keeps the current radio state until the phone confirms a successful
settings indication. It then applies the complete settings record.
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

Version 1 has twelve tools:

- `search_web(query)`: returns a small list of titles, URLs, and snippets.
- `search_images(query)`: returns a small list with short result IDs.
- `get_device_status()`: returns brightness, volume, battery availability,
  preference-persistence state, and the current power-off mode.
- `set_brightness(percent)`: applies and stores a value from 5 through 100.
- `set_volume(percent)`: applies and stores a value from 0 through 100.
- `power_off()`: schedules power-off only after an explicit current request.
- `restart_device()`: schedules a software restart only after an explicit
  current request.
- `run_python(code)`: runs bounded MicroPython for short calculations. Printed
  text enters the tool result. `plot.line(x, y, title)` can select one line
  plot with 2 through 128 entries and a title of at most 48 bytes. Each x value
  and each defined y value must be finite. A `None` y value makes a gap for an
  undefined function value, such as zero in a plot of `1/x`.
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

The restart prompt accepts only a clear request to restart the current device.
The final answer gives one short spoken confirmation. The runtime then records
the controlled action and calls the ESP software-restart path. The action does
not erase settings, memories, or BLE bonds. A new PWR-button action cancels a
restart that is still pending. The restart path does not depend on a working
display task, so voice control can recover a device whose AMOLED stays black.

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

Each visible failure names the operation and the reason. The messages
distinguish service connection phases, service time limits, account or key
problems, service request and response sizes, device data limits, and model
tool-step limits. The screen does not tell the user to retry when a retry cannot
correct the problem.

Tools do not get credentials directly. Providers own credentials and HTTP
details.

## Secret lifecycle

Development secrets use an ignored local file. The iOS app stores secrets in
Keychain. BLE provisioning requires an authenticated encrypted link. The
firmware validates a full settings packet, writes only changed values, and
reports the applied revision and fingerprint. Production stores the packet and
BLE bonds in plaintext NVS. This keeps settings after a restart, but a person
with physical flash access can read the credentials. Development keeps BLE
settings in memory and bonds in plaintext NVS so firmware uploads keep the
phone pairing. Phone proxy request headers and bodies use the same authenticated
encrypted BLE link. The app sends them only to the HTTPS destination in the
bounded request envelope. It does not save them. ChatESP never enables
eFuse-backed NVS
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
