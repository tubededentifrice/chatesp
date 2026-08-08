# ChatESP

ChatESP is an open-source, physical voice client for ChatGPT-compatible APIs.
It targets the Waveshare ESP32-S3-Touch-AMOLED-1.8 board and has a small iOS
companion for optional secure BLE configuration.

## Intended experience

1. Use a short top-button press to change between ChatESP and Clock. Clock
   rotates the display 90 degrees counterclockwise, so the USB port is at the
   bottom. It shows a large white local time and a rounded seconds path. It
   stays on.
2. Hold the bottom PWR button and speak. From Clock, the press first returns
   the display to ChatESP. Release the button to submit the recording.
3. The device selects a direct, web, image, device-control, calculation, or
   memory route. It then gets a concise answer with tools disabled. Text appears as
   the model sends it. Complete
   sentences go to speech at once. One playback session joins up to four
   speech segments in order. Each model request includes the user's current
   local date and time at minute precision and an approximate location. The
   app can supply this context. Without the app, the ChatESP device uses NTP and one
   fast, bounded IP-location lookup.
4. Pull down the small top handle to change brightness or volume on the
   device. The panel follows the finger. When released, it closes below half
   deployment and opens at or above half deployment. Tap or drag the large
   invisible touch row around either control track to set its value. The
   visible dot shows the value, but it is not the required touch target. The
   same controls are also available by voice.
5. Continue within 30 seconds to use the same thread. After 30 seconds without
   another interaction, the device clears the thread, stops Wi-Fi, and returns
   to Clock.
6. A short PWR-button press, without a recording, requests sleep. Clock does
   not sleep from inactivity. In ChatESP mode, the normal production or
   development idle timer still requests sleep.

The interface is black, high-contrast, and similar to a small terminal. The
bottom line shows Wi-Fi state and battery level. Status motion has a purpose:
recording level, network work, tool work, or speech.

Each full power-on or reset shows `CHAT ESP` and `STARTING` as soon as the
display is ready. The splash stays on only while the voice runtime starts. An
in-session display wake does not show the splash.

## Cloud defaults

- Chat: `~deepseek/deepseek-v4-flash-latest` through OpenRouter.
- Speech recognition: `openai/whisper-large-v3-turbo` through OpenRouter.
- Speech synthesis: `hexgrad/kokoro-82m` through OpenRouter. It uses
  `af_heart` for English and `ff_siwis` for French. It returns streaming
  24 kHz, 16-bit, mono PCM.
- Search: Brave Web Search and Brave Image Search through a provider adapter.

One OpenRouter key can supply chat, transcription, and speech. The provider
interfaces remain replaceable. For example, a later search adapter can use
ScrapingDog without a change to the conversation state machine.

## Repository status

The project is in active development. The firmware implements the black
terminal interface, bottom-PWR hold-to-talk, bounded microphone capture,
Wi-Fi, persistent HTTPS sessions, streamed model text, sentence speech,
parallel image download, search, BLE provisioning,
device status and controls, touch quick controls, a travel-clock mode, and
sleep paths. The iOS app is not a runtime requirement. A development image can
use its ignored local credentials without pairing. A production image keeps
the last provisioned credentials when the app is not connected. Without Wi-Fi
or a service key, local Clock and device controls stay available, but cloud
voice features report the missing capability. Brightness
and volume changes use a small persistent device-preference record. Up to ten
short user-requested memories persist in plaintext NVS and enter each model
request as untrusted context. A full list causes the model to compact the old
facts before it saves the pending fact. The iOS companion supports any number
of ChatESP devices. It saves global settings, per-device overrides, and each
edit as soon as it changes. Secrets stay in Keychain. A searchable model
browser filters OpenRouter models for the required chat, transcription, or
speech capabilities. The app sends one complete effective configuration over
encrypted BLE. A selected, bounded JPEG can appear full-screen after the
spoken answer. A restricted MicroPython tool can do short calculations and
show a bounded line plot on the full screen.

Automated tests cover pure state, protocol, privacy, and bounded-buffer paths.
The V2 development device has passed black-screen startup without a white
frame, bottom-button hold-to-talk, streamed answer text, clear streamed speech,
button preemption, strong-access-point selection, and modem power saving.
Full-screen image color and crop, Clock rotation and rounded-corner rendering,
top-button mode switching, physical iPhone provisioning, Clock current draw,
production system-off, memory persistence and compaction, secure BLE memory
management, MicroPython limits and plot display, and long cycle tests are still
acceptance gates. Do not use this status as a claim that these open physical
checks passed.

## Development

Requirements:

- `uv` 0.11.32 or newer
- Xcode 26 or newer for the iOS app
- The connected Waveshare board for physical acceptance tests

Set up the locked tools:

```sh
python3 tools/check_dependency_age.py
uv sync --locked
```

Run repository checks:

```sh
uv run --locked python -m unittest discover -s tests -p 'test_*.py'
uv run --locked python tools/check_secrets.py
uv run --locked python tools/pio.py test -e native
uv run --locked python tools/pio.py run -e watch_dev
```

Use development mode during normal firmware work. ChatESP mode uses a
five-minute automatic idle timer and keeps USB flashing available. Clock mode
stays on. A short PWR-button press still turns off the display at once:

```sh
uv run --locked python tools/pio.py run -e watch_dev -t upload
```

For a black screen, or after each display or power change, use the device
doctor with the explicit local ChatESP device port. It uploads the current development
image and checks the version, board revision, two-step display wake, and voice
runtime start:

```sh
uv run --locked python tools/device_doctor.py --port LOCAL_PORT
```

The command cannot inspect emitted pixels. Confirm that `CHAT ESP` or `READY`
is visible when the command reports that its automatic checks passed.

Use production mode only for final power tests and release images. It enables
persistent settings, persistent BLE bonds, and AXP2101 system-off after a sleep
request. Settings, bonds, and saved memories use plaintext NVS. A person with
physical flash access can read the stored credentials and saved facts.
You can build it without a device change:

```sh
uv run --locked python tools/pio.py run -e watch_prod
```

Production does not enable encrypted NVS, flash encryption, or secure boot.
These features can burn eFuses. Each device profile sets
`CHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0`. The build wrapper, CMake,
reviewed SDK settings, and source checks each fail closed if the lock is
missing, changed, duplicated, or replaced. ChatESP device builds also reject project or
build-flag overrides. There is no user-approval or environment-variable bypass.

See [development mode](docs/development-mode.md) for the mode contract and the
recovery procedure.

Read device logs with the repository monitor. Opening the ESP32-S3 native USB
serial port can reset the chip. Use the monitor only when a reset is acceptable.
The repository monitor clears the close-time hangup flag. Before it closes, it
resets the chip with the boot line inactive. This prevents a normal monitor
close from leaving the device in the ROM loader. The generic serial monitor can
still request the ROM loader:

```sh
uv run --locked python tools/watch_monitor.py --port LOCAL_PORT --duration 10
```

Add `--latency-report` to calculate p50 and p90 values from the privacy-safe
`LATENCY` records. These records contain durations only. They do not contain
audio, chat text, URLs, credentials, or device identifiers.

Run all PlatformIO commands through `tools/pio.py`. The wrapper checks the
dependency cooldown first. For the ChatESP device build, it also creates the ESP-IDF
Python environment from a hash-locked requirements file.

For a user-requested task branch or a repeatable tooling failure, use the
procedures in [agent tooling](docs/agent-tooling.md).

Never put credentials in tracked files. Local development values belong in
`.secrets/device.env`, which Git ignores. The iOS app will store secrets in
Keychain and provision the device through encrypted BLE.

## Hardware source

The board support and hardware facts come from the
[official Waveshare repository](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8)
and [product documentation](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8).

## License

MIT. See `LICENSE`. Third-party components keep their own licenses.
