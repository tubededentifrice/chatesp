# Hardware contract

## Target

The target is the Waveshare ESP32-S3-Touch-AMOLED-1.8.

- ESP32-S3, 16 MB flash, 8 MB PSRAM
- 368 by 448 QSPI AMOLED
- Original display and touch: SH8601 and FT3168
- V2 display and touch: CO5300 and CST820. The ESP-IDF touch component uses
  the compatible CST816S driver-family API for this controller.
- ES8311 codec with microphone input and speaker amplifier
- AXP2101 power management
- QMI8658 six-axis IMU
- PCF85063A real-time clock
- MicroSD over SDMMC
- Two physical buttons

The local `chatesp_board` component is based on the official
`waveshare/esp32_s3_touch_amoled_1_8` component at a reviewed source commit. It
starts the CO5300 at zero brightness. The app draws a black frame before it
raises the brightness. After panel-on, it sends a second complete frame and the
same brightness command. This bounded retry recovers a controller that accepts
the first commands but keeps the first frame black. The current package always
creates a CO5300 panel. It probes both touch types, but it does not select an
SH8601 display. The ChatESP board adapter must add the original-board SH8601
path before original-board support is complete.

The V2 adapter configures the LCD and CST820 reset GPIO values as not connected.
An in-session display wake does not reset either controller. Firmware replays
the bounded CO5300 initialization table while LVGL is locked, restores the
selected brightness, and sends one complete frame. A brightness or display-on
command acknowledgement alone is not wake proof.
Cold start draws and shows the reliable splash before it probes and registers
the CST820. A held cold start uses this active panel state. It does not replay
the initialization table before listening. A failed touch start disables touch
controls but does not block voice operation.

The LVGL draw and software-rotation buffers are DMA-capable and use internal
memory. The board allocates them during display start. Do not use a normal
buffer that makes the SPI driver allocate an equivalent temporary DMA buffer
for each flush. Use the reviewed 32-row buffer. A larger buffer leaves too
little internal memory for audio DMA and the Bluetooth controller. A PSRAM DMA
buffer makes this panel driver allocate a private internal transfer buffer and
can stop display refresh. Initialize I2S before Wi-Fi and BLE. An audio
allocation error must return to the app without a fatal check. The optional
timezone HTTPS worker uses its fixed 20 KiB stack in PSRAM. It does not take the
internal RAM that the active Wi-Fi and I2S paths need.

Probe the touch controller after reset release. Address `0x15` identifies V2.
Address `0x38` identifies the original board. Do not infer the display revision
from a failed display start.

## Button behavior

Physical testing shows that the bottom button is the AXP2101 PWR key. The same
signal is available as the active-high TCA9554 EXIO4 input. It is the user
action button:

- short press from idle: sleep;
- hold: record while held;
- release after recording: submit.

The app disables the AXP2101 automatic long-hold shutdown while the PWR button
is pressed. This lets a recording continue for more than six seconds. It
restores hardware long-hold shutdown when the button is released and at each
idle start. The top BOOT button is active-low GPIO0. After boot, a debounced
press from 30 through 700 ms changes between ChatESP and Clock. A shorter
electrical pulse or a BOOT-only longer press has no app action. The button is
not a sleep wake source. Its boot-strapping function stays available for
firmware recovery. Firmware uses the EXIO4 level only to detect a held key at
start.
Holding PWR and BOOT together for five seconds calls the ESP software-restart
path. The timer starts only when both debounced buttons are down. Releasing
either button cancels and resets the timer. A PWR-only hold keeps its recording
action. A BOOT-only long press stays unassigned. The recovery check runs in the
main button poll and does not need the display, agent, or network.
It rejects that level when the PMU start status contains a completed release or
short-press event. This is necessary because EXIO4 can stay active after a
battery-powered release. During operation, it uses AXP2101 PWRON edge events.
On the connected V2 board,
EXIO4 stayed active after key release during battery operation. Firmware rejects
an edge sample that also contains a USB power-source event.
The long-hold control is AXP2101 `PWROFF_EN` register `0x22`, bit 1. Firmware
also clears register `0x10`, bit 2 at start. An earlier firmware revision used
that unrelated bit and enabled the PMIC 16-second PWR shutdown path.
AXP2101 register `0x27`, bits 1:0, selects the PWR-on recognition time.
Firmware sets and reads back `00`, which is 128 ms. It preserves the IRQ and
power-off timing fields in the same register. The PMIC starts its switched
rails after this interval. It does not wait to classify the press as short or
long. If the user continues to hold PWR, firmware enters `LISTENING` at the
normal 350 ms recording threshold.

## Power behavior

Before sleep, stop audio, radio work, display updates, touch, and unused
peripherals. Development soft sleep draws a full black frame and keeps the
CO5300 initialized at its current brightness. The panel can accept a successful
brightness or display-on command after brightness zero while its pixels stay
black. Therefore, an in-session wake must not use this zero-to-nonzero path.
Production sets brightness to zero only after the sleep cancel window, then it
enables the AXP2101 internal output discharge and requests system-off. The PMIC
turns off all switched outputs except RTCLDO. Its data sheet specifies 40
microamps as typical battery-only system-off consumption, with only RTCLDO
active. This value does not include the board RTC, battery protection, battery
self-discharge, or board leakage. Measure complete board current on battery
hardware before a battery-life claim.

ChatESP requests AXP2101 system-off at or below 5 percent when the PMIC does
not report good VBUS or active charging. This limit applies to development and
production. Good VBUS or active charging prevents the low-battery request so
the device can start and charge. The active runtime reads the gauge at most
once every 30 seconds, after a VBUS event, or once after a PWR wake. If the
wake refresh is blocked while the button has priority, the runtime keeps it
pending and reads the PMIC after that priority ends. It does not read the gauge
after sleep starts. Production system-off also stops the processor, so no
firmware polling occurs in that state.

The production profile does not run the optional full-PSRAM start test. It uses
a speed-optimized bootloader that reports only warnings and failures.
Development keeps the PSRAM start test and normal boot reports so firmware work
still checks the fixed memory part. The UI sends the reliable two-frame splash
before it probes touch, builds hidden Clock, plot, and control views, or loads
saved memories. It defers the rounded Clock path buffer and calculation until
the first Clock entry. Thus, touch and Clock work do not delay the first visible
feedback. These start changes do not change system-off current or steady active
settings. Measure PWR-to-pixels time and complete interaction energy on the
board.

An active PWR press does not replay the panel initialization table before the
firmware knows its action. A short press draws the black sleep frame without an
intermediate panel restart. If the press reaches the recording threshold,
microphone capture starts first and the bounded panel recovery follows. A wake
from development soft sleep or USB-held production system-off still requests
panel recovery at once.

The 128 ms PWR-on setting is a reversible PMIC register change. It stays set
through normal AXP2101 system-off and does not enable a processor timer, poll,
or switched rail while off. Thus, it does not add system-off current. A complete
PMIC power loss can restore the factory PWR-on time until the next firmware
start writes the fast value again.

USB can keep the ESP32 powered after the PMIC accepts system-off. Production
must stay in its completed sleep state and repeat the request at a one-second
interval. It must not restore the display, radios, or normal runtime only
because USB kept power present. A bottom PWR press cancels this state. USB
removal lets the next request turn the main rails off. A PWR press causes a cold
production boot or removes the development black frame. The top BOOT button is
not a wake source. This keeps the same short-press and hold patterns.

The NimBLE shutdown completion wait has a one-second limit. A stalled shutdown
must not block a development PWR-button wake or a production system-off
request.

When the authenticated iPhone phone proxy is ready, a cloud request keeps BLE
active and does not start Wi-Fi. If that proxy is not ready after a two-second
limit, the existing Wi-Fi path starts. A normal ChatESP idle sleep stops both
radios. Phone distance cannot reconnect a device while it is in system-off or
development soft sleep. A bottom PWR press wakes it, and the saved bond lets the
app reconnect without a new pairing code.

Clock keeps the AMOLED and BLE on while external power is connected. If local
time is not ready, it keeps the startup Wi-Fi connection for at most 15
seconds. It stops Wi-Fi when local time becomes available or the limit expires.
Without external power, Clock requests sleep after five minutes. The timeout
starts again when external power is removed. The bottom PWR button keeps its
short-press sleep action. A bottom-button press first restores the portrait
ChatESP layout, and a held press then starts recording at the normal threshold.
ChatESP requests sleep after 30 seconds without input. Only a short top
BOOT-button press enters Clock.

The model can request device status, set display brightness from 5 through 100
percent, set playback volume from 0 through 100 percent, request power-off, and
request a software restart.
The user can also open a top touch panel and change brightness or volume in
five-percent steps. The panel follows a downward finger movement. A press or a
drag in the 352-by-64-pixel invisible row around either track sets its value.
The visible rail, fill, dot, labels, and spacing do not change. The dot shows
the current value, but it is not the required touch target. A release below
half panel deployment closes the panel. A release at or above half deployment
opens it. Brightness changes during a drag. Volume changes during active
playback. Firmware saves the final pair after release and does not write NVS
for each input position. The LVGL refresh period is 16 ms. Its internal draw
buffer is 368 by 32 pixels to reduce transfers during panel motion.
Brightness and volume persist across a restart when NVS is available. A model
power-off first completes a short spoken confirmation. Production then uses the
same system-off cleanup as the PWR button and inactivity timer. One bottom PWR
press starts the board again. Development firmware uses soft sleep so that USB
upload stays available; one bottom PWR press wakes it.

A model restart first completes a short spoken confirmation and then calls the
ESP software-restart path. It keeps saved settings, memories, and BLE bonds. A
new PWR-button action cancels the pending restart. The call does not depend on
the display task, so it can recover the runtime when the AMOLED stays black but
voice input and speech output still work.

The connected V2 board must pass these checks for this control change:

- each cold start, software reset, or watchdog reset first shows the black
  `CHAT ESP` and `STARTING` splash without a white frame;
- the splash changes to `READY` as soon as the runtime can accept input, with
  no added minimum delay;
- an in-session display wake shows the current state and not the boot splash;
- an in-session display wake after each soft sleep or USB-held production
  system-off restores pixels without a panel or touch-controller reset;
- a held bottom PWR press shows `LISTENING`, and release shows `TRANSCRIBING`;
- the `LISTENING` view shows responsive low-to-high frequency bars and does
  not show one total-volume bar;
- a held PWR press longer than six seconds does not stop the board during a
  recording;
- unplugging or reconnecting USB power does not start, stop, or submit a
  recording;
- with USB disconnected, a held PWR press starts recording and its release
  submits without a USB reconnection;
- a short PWR press from idle turns the screen off and requests system-off;
- a short PWR press from idle does not turn the panel on again between the
  first black frame and the completed sleep state;
- 30 seconds without input in ChatESP turns the screen off and requests the
  selected development or production sleep path;
- a stalled BLE shutdown does not block a PWR-button wake from development
  soft sleep or a production system-off request;
- a failed BLE connection with controller reason `0x3e` starts advertising
  again and the selected iPhone can connect on its next scan;
- a failed-connection callback during BLE shutdown does not start advertising;
  a transient advertising failure retries and then requests a complete host
  recovery if the retry limit ends;
- after Wi-Fi and BLE start, repeated full-screen refreshes do not report a
  private transmit-buffer allocation failure or block the LVGL task;
- a held PWR-button cold start replaces the splash with `LISTENING` at the
  normal hold threshold without a second panel initialization;
- the reliable cold-start splash appears before the touch-controller probe;
- from production system-off, each PWR press starts the power rails after the
  128 ms recognition interval, without a short-versus-hold result;
- after the fast PWR-on setting is applied, production system-off current is
  not higher than the prior measured value;
- a short battery-powered PWR cold start shows `CHAT ESP`, then `READY`, and
  does not show `LISTENING`;
- a production cold start with saved settings reaches
  `settings_apply_complete` and does not reset after `settings_apply_begin`;
- a held battery-powered PWR cold start uses saved production settings before
  it sends a cloud request;
- a short top-button press changes between portrait ChatESP and Clock;
- an electrical top-button pulse shorter than 30 ms does not change mode;
- a long top-button press and a top-button press during development soft sleep
  do not change application state;
- holding PWR and BOOT together for less than five seconds does not restart;
- holding PWR and BOOT together for five seconds causes one software restart;
- releasing either button resets the restart timer;
- the five-second restart button combination works when the AMOLED stays black
  but the main button poll still runs;
- Clock rotates 90 degrees counterclockwise, has the USB port at the bottom,
  and maps the touch control panel to that orientation;
- a BLE pairing code uses the portrait ChatESP orientation with the buttons on
  the right, a pairing request wakes the AMOLED, and Clock returns to its prior
  orientation when the code closes;
- Clock shows only large white 24-hour time and a one-pixel white seconds line
  on black;
- the seconds path uses the outermost valid pixels, follows the rounded screen
  shape, starts at 12 o'clock, changes by one display pixel at a time, fills on
  even minutes, and drains on odd minutes;
- Clock gets time from authenticated phone context or NTP, gets the UTC offset
  from the phone or the bounded IP fallback, and continues from monotonic time
  while the ChatESP device stays powered;
- Clock keeps Wi-Fi on for no more than 15 seconds when local time is not ready,
  and stops it as soon as local time becomes available;
- Clock stays on while USB is connected. After USB is removed, it requests
  sleep at five minutes. On battery, it stays on before this limit and requests
  sleep at the limit. A short bottom PWR press still requests sleep;
- a bottom PWR press in Clock shows ChatESP without visible delay, and a held
  press starts `LISTENING` at the normal threshold;
- 30 seconds after the final voice interaction, ChatESP sleeps, Wi-Fi stops,
  and the prior thread is not available;
- the footer shows the active Wi-Fi or secure BLE icon. Connected Wi-Fi shows
  one through three signal bars. The footer shows a valid battery percentage
  or a clear unavailable value. While external power is connected, its
  battery-level icon and percentage are green, and a white charge mark is
  centered over the battery icon. This state stays correct when the battery is
  full and charge current stops;
- after a PWR wake with USB still connected, the footer updates the charging
  state after button priority ends and before the next idle sleep;
- development firmware shows a small `DEV` marker at the footer center, and
  production firmware does not show the marker;
- at 6 percent on battery power, the device stays on. At 5 percent, it requests
  system-off in development and production. Good VBUS or active charging
  prevents this request;
- after sleep starts, the firmware does not read the battery gauge;
- model text grows on the display before the complete answer is available;
- a long transcript, answer, or error scrolls vertically under the finger,
  keeps release momentum, resists both ends, and shows its scroll indicator
  only during movement;
- the first text of a new view starts at the top, and a streamed answer update
  does not change the user's current scroll position;
- smart quotation marks, long dashes, bullets, and ellipses in model text have
  the correct visible glyphs;
- a provisioned 100% chat font size keeps the standard ChatESP layout;
- a provisioned 200% chat font size enlarges all ChatESP text and status
  glyphs, wraps the answer within the display, and keeps long text scrollable;
- changing the chat font size does not change the Clock face or its time font;
- on a held cold start, Wi-Fi setup starts only after 100 ms of valid audio and
  does not stop microphone capture;
- speech starts from the first complete sentence. One second TTS request has
  the complete remaining spoken answer. Request order is correct and the codec
  stays active;
- an image request reserves speech playback resources before JPEG work starts,
  and a JPEG allocation failure does not stop speech;
- a speech failure shows a clear operation and reason. It does not show an
  internal numeric error category;
- an English answer uses the selected English voice, a French answer uses the
  selected French voice, and the internal language tag is not visible or spoken;
- a fast PCM transfer starts after the 200 ms prebuffer. A slower initial
  transfer gets one 500 ms safety-buffer check. A transfer below the safe
  playback rate buffers completely so that audio stays clear;
- the iPhone proxy forwards declared-length PCM while the HTTPS body arrives.
  Playback does not wait for the complete phone response buffer;
- a new held press returns to `LISTENING` within 250 ms and stops model, TTS,
  playback, search, and image work.
- device status reports the current brightness and volume and reports a battery
  value or a clear unavailable state;
- brightness commands apply at 5 and 100 percent and reject values outside the
  range;
- volume commands mute at 0 percent, apply at 100 percent, and reject values
  outside the range;
- a tap on the top handle and a downward top-edge swipe open the control panel;
- the downward swipe stays captured after it moves below the top touch target;
- the panel follows the finger during a downward pull without a visible jump;
- the handle stays at the panel bottom and follows each drag and settle motion
  without a visible jump from its closed position;
- releasing the panel below half deployment closes it, and releasing it at or
  above half deployment opens it;
- a swipe that starts below the top edge or moves mainly sideways does not open
  the control panel;
- the panel closes after an upward swipe, a tap outside, five seconds without
  touch, a recording start, a passkey display, or sleep;
- the panel opens above a full-screen image and does not cover a BLE passkey;
- a press at any position on either track sets the related value, and each
  knob stays fully visible during a drag;
- a press in the invisible margin around either track starts its drag without
  a change to the visible control layout;
- brightness changes during a drag and does not return to the old value after
  a temporary display-lock conflict;
- active speech volume changes without a restart, and one release causes at
  most one preference write;
- a PWR-button action keeps priority while the control panel is open;
- changed brightness and volume values return after a reset;
- an explicit model power-off gives one short confirmation and then sleeps;
- a farewell, a hypothetical statement, or an uncertain transcript does not
  schedule power-off;
- a new PWR-button action cancels a pending model power-off;
- an explicit model restart gives one short confirmation and then starts the
  normal boot splash;
- a farewell, a hypothetical statement, or an uncertain transcript does not
  schedule a restart;
- a new PWR-button action cancels a pending model restart;
- a model restart keeps settings, memories, and BLE bonds;
- development model power-off enters soft sleep and a PWR press wakes it;
- production model power-off requests AXP2101 system-off and a PWR press causes
  a cold start.
- when production requests sleep with USB connected, the screen and radios
  stay off, a bottom PWR press wakes the app, and USB removal causes system-off
  in no more than one retry interval;
- production system-off discharges the switched PMIC outputs and does not leave
  the display, touch, codec, amplifier, radio, or ESP32 rail powered;

The full-screen image path must pass these checks on the V2 AMOLED:

- a selected baseline JPEG fills the screen with a centered cover crop;
- an invalid first thumbnail falls through to a later trusted current result,
  and all candidates share one 20-second limit;
- a valid JPEG media type with case changes or parameters is accepted;
- after all bounded candidates fail, the text answer stays visible with a
  clear image-unavailable notice;
- an image can still appear when speech fails before playback starts;
- a successful image search that has no second model selection shows the first
  current result instead of only claiming that an image is visible;
- red, green, and blue test areas have the correct color and byte order;
- wide and tall images have a centered crop with the correct rotation;
- a new PWR-button press removes the image and starts `LISTENING` without a
  visible delay;
- a BLE passkey stays visible above an image;
- a short press and the inactivity timer remove the image before sleep;
- an unsupported or large image leaves the text answer available;
- repeated image requests do not cause a reset or a PSRAM leak.

The restricted Python path must pass these checks on the V2 AMOLED:

- a short calculation returns the correct printed value and spoken answer;
- an infinite loop stops within the bounded execution interval and the next
  voice request works;
- a large allocation stops at the fixed Python heap limit without a reset;
- a held PWR press cancels Python work and starts `LISTENING` without a long
  delay;
- `plot.line` shows 2 through 128 bounded entries on a black full-screen chart
  after speech ends, and a `None` y value makes a visible line gap;
- a plot of `1/x` from -1 through 1 completes in one Python tool call and does
  not draw a line through the undefined value at zero;
- the plot title and axis ranges are readable with the correct board rotation;
- a new PWR action and sleep remove the plot;
- repeated calculations, limit failures, and plots do not cause a reset or a
  PSRAM leak.

The current USB-power checks do not verify battery sleep current.

## Permanent-write policy

ChatESP must never burn an eFuse or enable a feature that can burn one on first
start. This rule also applies to production firmware. NVS encryption, flash
encryption, and secure boot stay disabled. Each device profile must set
`CHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0`. Source checks must stop a build if
this flag is absent, duplicated, or nonzero, or if an irreversible ESP-IDF
feature is on. CMake must also require
`CHATESP_PERMANENT_WRITE_POLICY=FORBID`. The build wrapper must reject project,
environment, and build-flag overrides for ChatESP device builds. It must reject direct
first-party eFuse write APIs. There is no user request, environment variable,
or approval flag that can bypass this rule.

Production BLE settings and all BLE bonds use normal plaintext NVS. This is
less secure than eFuse-backed encrypted NVS, but it is reversible. An attacker
with physical flash access can read Wi-Fi and provider credentials. BLE
transfer still requires an authenticated encrypted connection, and iOS secrets
stay in Keychain.
Saved memories also use plaintext NVS. Development and production use separate
memory namespaces. Each update writes and reads back one complete inactive
record before it changes the active slot. A person with physical flash access
can read the facts. The firmware must not log facts, memory tool arguments, or
BLE memory frames.

Run `tools/device_doctor.py` with the explicit local port after each display or
power change. Its serial checks can verify the image, V2 probe, command order,
and runtime start. They cannot verify emitted pixels. A person must confirm the
visible splash or ready view.

## Physical acceptance gates

- Identify the connected board revision.
- Verify both buttons, short and long top-button presses, the five-second
  restart button combination, sleeping-state top input, and the selected wake
  source.
- Verify AMOLED black level, both mode rotations, rounded Clock rendering,
  rotated touch mapping, full-screen image, and full-screen Python plot.
- Record and replay speech through the ES8311 path without clipping.
- Verify Wi-Fi connection and TLS requests.
- Verify that Wi-Fi starts at boot and a held PWR button stays responsive while
  the station connects.
- Verify encrypted BLE provisioning and acknowledgement with a physical iPhone.
- Verify that automatic memory loading and a settings transfer can start on the
  same connection, that the settings acknowledgement arrives, and that the
  device does not reset while it applies the new radio settings.
- Verify cold-start memory persistence, full-list automatic compaction,
  user-requested compaction, and rollback after an injected NVS failure.
- Verify secure BLE rejection, one-fact paging, revision conflicts, retry
  deduplication, voice-change refresh, and sleep while BLE memory work exists.
- Verify that a wake connection sends current iPhone time, UTC offset, and a
  location rounded to 0.1 degree to the model context.
- Verify that Clock matches the iPhone local time before and after a timezone
  change and remains correct across a monotonic millisecond wrap simulation.
- Verify that a denied location permission uses the saved city fallback and
  does not block a request.
- Verify that one continuous connection does not sync more than once per hour.
- Verify model device controls at each brightness and volume limit, after a
  reset, and with NVS write failure injection.
- Verify model power-off confirmation, cancellation, production current, and
  bottom-PWR wake.
- Measure production system-off current with USB disconnected. Record the
  battery voltage, stable current after rail discharge, and wake result. Do not
  use the AXP2101 40-microamp data-sheet value as a board measurement.
- Measure ChatESP idle, externally powered Clock, battery-powered Clock,
  recording, Wi-Fi, playback, and deep-sleep current.
- Run at least 100 talk cycles and 100 sleep/wake cycles without a leak, reset,
  stuck state, or unexpected NVS write.
- Run 100 memory writes across sleep and wake without a lost old record,
  duplicate retry write, reset, or stuck indication.
- Compare the same 20 direct, 10 web, 10 image, and 10 calculation prompts
  before and after the change on 2.4 GHz Wi-Fi at -65 dBm or better. Test warm
  and held-cold starts.
- Require at least 30 percent lower p50 release-to-first-audio time for direct
  questions. Warm direct p50 must be at most 4 seconds and p90 at most 7
  seconds. Held-cold p90 must be at most 10 seconds. Web p90 must be at most 12
  seconds.
- Confirm that the prompt set has no PCM underrun or segment-order error.
- Measure energy for the complete interaction. Do not infer energy improvement
  from peak current.
