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

The LVGL draw and software-rotation buffers are DMA-capable and use internal
memory. The board allocates them during display start. Do not use a normal
48-row buffer that makes the SPI driver allocate an equivalent temporary DMA
buffer for each flush. After Wi-Fi and BLE start, that temporary allocation can
fail and leave LVGL waiting for a transfer completion that cannot occur.

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
restores hardware long-hold shutdown when the button is released. The top BOOT
button is active-low GPIO0. After boot, a debounced press of 700 ms or less
changes between ChatESP and Clock. A longer press has no app action. The button
is not a sleep wake source. Its boot-strapping function stays available for
firmware recovery. Firmware uses the EXIO4 level only to detect a held key at
start.
During operation, it uses AXP2101 PWRON edge events. On the connected V2 board,
EXIO4 stayed active after key release during battery operation. Firmware rejects
an edge sample that also contains a USB power-source event.

## Power behavior

Before sleep, stop audio, radio work, display updates, touch, and unused
peripherals. Turn the AMOLED off and request AXP2101 system-off. A PWR press
then causes a cold boot and a new thread. Measure current on battery hardware
before making a battery-life claim.

Clock keeps the AMOLED and BLE on and stops Wi-Fi when it becomes active. It
does not request automatic sleep. The bottom PWR button keeps its short-press
sleep action. A bottom-button press first restores the portrait ChatESP layout,
and a held press then starts recording at the normal threshold. After 30
seconds without a follow-up interaction, the runtime returns to Clock and
clears the thread.

The model can request device status, set display brightness from 5 through 100
percent, set playback volume from 0 through 100 percent, and request power-off.
The user can also open a top touch panel and change brightness or volume in
five-percent steps. The panel follows a downward finger movement. A press or a
drag in the 352-by-64-pixel invisible row around either track sets its value.
The visible rail, fill, dot, labels, and spacing do not change. The dot shows
the current value, but it is not the required touch target. Brightness changes
during a drag. Volume changes during active playback. Firmware saves the final
pair after release and does not write NVS for each input position. The LVGL
refresh period is 16 ms. Its internal draw buffer is 368 by 48 pixels to reduce
transfers during panel motion.
Brightness and volume persist across a restart when NVS is available. A model
power-off first completes a short spoken confirmation. Production then uses the
same system-off cleanup as the PWR button and inactivity timer. One bottom PWR
press starts the board again. Development firmware uses soft sleep so that USB
upload stays available; one bottom PWR press wakes it.

The connected V2 board must pass these checks for this control change:

- each cold start, software reset, or watchdog reset first shows the black
  `CHAT ESP` and `STARTING` splash without a white frame;
- the splash changes to `READY` as soon as the runtime can accept input, with
  no added minimum delay;
- an in-session display wake shows the current state and not the boot splash;
- a held bottom PWR press shows `LISTENING`, and release shows `TRANSCRIBING`;
- a held PWR press longer than six seconds does not stop the board during a
  recording;
- unplugging or reconnecting USB power does not start, stop, or submit a
  recording;
- with USB disconnected, a held PWR press starts recording and its release
  submits without a USB reconnection;
- a short PWR press from idle turns the screen off and requests system-off;
- 30 seconds without input in a manual production ChatESP session turns the
  screen off and requests system-off;
- after Wi-Fi and BLE start, repeated full-screen refreshes do not report a
  private transmit-buffer allocation failure or block the LVGL task;
- a held PWR-button cold start replaces the splash with `LISTENING` at the
  normal hold threshold;
- a short top-button press changes between portrait ChatESP and Clock;
- a long top-button press and a top-button press during development soft sleep
  do not change application state;
- Clock rotates 90 degrees counterclockwise, has the USB port at the bottom,
  and maps the touch control panel to that orientation;
- a BLE pairing code uses the portrait ChatESP orientation with the buttons on
  the right, and Clock returns to its prior orientation when the code closes;
- Clock shows only large white 24-hour time and a white seconds path on black;
- the seconds path follows the rounded screen shape, starts at 12 o'clock,
  fills on even minutes, and drains on odd minutes;
- Clock shows an unavailable-time face until an authenticated phone context
  supplies epoch time and the user's UTC offset;
- Clock stays on for more than each ChatESP idle timeout, while a short bottom
  PWR press still requests sleep;
- a bottom PWR press in Clock shows ChatESP without visible delay, and a held
  press starts `LISTENING` at the normal threshold;
- 30 seconds after the final voice interaction, Clock returns, Wi-Fi stops,
  and the prior thread is not available;
- the footer shows Wi-Fi connection state and a valid battery percentage, or a
  clear unavailable value;
- model text grows on the display before the complete answer is available;
- on a held cold start, Wi-Fi setup starts only after 100 ms of valid audio and
  does not stop microphone capture;
- speech starts from the first complete sentence while later answer text and
  TTS segments continue. Segment order is correct and the codec stays active;
- an English answer uses the `af_heart` Kokoro voice, a French answer uses
  `ff_siwis`, and the internal language tag is not visible or spoken;
- a fast PCM transfer starts after the 200 ms prebuffer. A slow first segment
  buffers before playback so that audio stays clear;
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
- development model power-off enters soft sleep and a PWR press wakes it;
- production model power-off requests AXP2101 system-off and a PWR press causes
  a cold start.

The full-screen image path must pass these checks on the V2 AMOLED:

- a selected baseline JPEG fills the screen with a centered cover crop;
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
- `plot.line` shows 2 through 128 finite points on a black full-screen chart
  after speech ends;
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
environment, and build-flag overrides for watch builds. It must reject direct
first-party eFuse write APIs. There is no user request, environment variable,
or approval flag that can bypass this rule.

Production BLE settings and BLE bonds use normal plaintext NVS. This is less
secure than eFuse-backed encrypted NVS, but it is reversible. An attacker with
physical flash access can read Wi-Fi and provider credentials. BLE transfer
still requires an authenticated encrypted connection, and iOS secrets stay in
Keychain.
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
- Verify both buttons, short and long top-button presses, sleeping-state top
  input, and the selected wake source.
- Verify AMOLED black level, both mode rotations, rounded Clock rendering,
  rotated touch mapping, full-screen image, and full-screen Python plot.
- Record and replay speech through the ES8311 path without clipping.
- Verify Wi-Fi connection and TLS requests.
- Verify that Wi-Fi starts at boot and a held PWR button stays responsive while
  the station connects.
- Verify encrypted BLE provisioning and acknowledgement with a physical iPhone.
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
- Measure ChatESP idle, continuous Clock, recording, Wi-Fi, playback, and
  deep-sleep current.
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
