# ChatESP

ChatESP is an open-source, physical voice client for ChatGPT-compatible APIs.
It targets the Waveshare ESP32-S3-Touch-AMOLED-1.8 board and has a small iOS
companion for secure BLE configuration.

## Intended experience

1. Hold the bottom PWR button and speak.
2. Release it. The display shows the transcript.
3. The device selects a direct, web, or image route. It then gets a concise
   answer with tools disabled. Text appears as the model sends it. Complete
   sentences go to speech at once. One playback session joins up to four
   speech segments in order. Each model request includes the user's current
   local date and time at minute precision and an approximate location from the
   companion.
   The app refreshes this context when the watch connects and at most once per
   hour while connected.
4. Ask for device status, brightness, volume, or power-off when needed.
5. Continue within 30 seconds to use the same thread.
6. Wait 30 seconds, or use a short PWR-button press, to sleep. The next wake
   starts a new thread.

The interface is black, high-contrast, and similar to a small terminal. The
bottom line shows Wi-Fi state and battery level. Status motion has a purpose:
recording level, network work, tool work, or speech.

Each full power-on or reset shows `CHAT ESP` and `STARTING` as soon as the
display is ready. The splash stays on only while the voice runtime starts. An
in-session display wake does not show the splash.

## Cloud defaults

- Chat: `deepseek/deepseek-v4-flash` through OpenRouter.
- Speech recognition: `openai/whisper-large-v3-turbo` through OpenRouter.
- Speech synthesis: `hexgrad/kokoro-82m` with the `af_heart` voice through
  OpenRouter. It returns streaming 24 kHz, 16-bit, mono PCM.
- Search: Brave Web Search and Brave Image Search through a provider adapter.

One OpenRouter key can supply chat, transcription, and speech. The provider
interfaces remain replaceable. For example, a later search adapter can use
ScrapingDog without a change to the conversation state machine.

## Repository status

The project is in active development. The firmware implements the black
terminal interface, bottom-PWR hold-to-talk, bounded microphone capture,
Wi-Fi, persistent HTTPS sessions, streamed model text, sentence speech,
parallel image download, search, BLE provisioning,
device status and controls, and sleep paths. Brightness and volume changes use
a small persistent device-preference record. The iOS companion can store
settings in Keychain and send them over encrypted BLE. A selected, bounded JPEG
can appear full-screen after the spoken answer.

Automated tests cover pure state, protocol, privacy, and bounded-buffer paths.
The V2 development device has passed black-screen startup without a white
frame, bottom-button hold-to-talk, streamed answer text, clear streamed speech,
button preemption, strong-access-point selection, and modem power saving.
Full-screen image color and crop, physical iPhone provisioning, battery current,
production system-off, and long cycle tests are still acceptance gates. Do not
use this status as a claim that these open physical checks passed.

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

Use development mode during normal firmware work. It uses a five-minute
automatic idle timer and keeps USB flashing available. A short PWR-button press
still turns off the display at once:

```sh
uv run --locked python tools/pio.py run -e watch_dev -t upload
```

For a black screen, or after each display or power change, use the device
doctor with the explicit local watch port. It uploads the current development
image and checks the version, board revision, two-step display wake, and voice
runtime start:

```sh
uv run --locked python tools/device_doctor.py --port LOCAL_PORT
```

The command cannot inspect emitted pixels. Confirm that `CHAT ESP` or `READY`
is visible when the command reports that its automatic checks passed.

Use production mode only for final power tests and release images. It enables
persistent settings, persistent BLE bonds, and AXP2101 system-off after a sleep
request. Settings and bonds use plaintext NVS. A person with physical flash
access can read the stored credentials.
You can build it without a device change:

```sh
uv run --locked python tools/pio.py run -e watch_prod
```

Production does not enable encrypted NVS, flash encryption, or secure boot.
These features can burn eFuses. Each device profile sets
`CHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0`, and source checks reject a build
that can make one of these permanent changes. There is no approval bypass.

See [development mode](docs/development-mode.md) for the mode contract and the
recovery procedure.

Read device logs with the repository monitor. Opening the ESP32-S3 native USB
serial port can reset the chip. Use the monitor only when a reset is acceptable.
The generic serial monitor can also request the ROM loader:

```sh
uv run --locked python tools/watch_monitor.py --port LOCAL_PORT --duration 10
```

Add `--latency-report` to calculate p50 and p90 values from the privacy-safe
`LATENCY` records. These records contain durations only. They do not contain
audio, chat text, URLs, credentials, or device identifiers.

Run all PlatformIO commands through `tools/pio.py`. The wrapper checks the
dependency cooldown first. For the watch build, it also creates the ESP-IDF
Python environment from a hash-locked requirements file.

Never put credentials in tracked files. Local development values belong in
`.secrets/device.env`, which Git ignores. The iOS app will store secrets in
Keychain and provision the device through encrypted BLE.

## Hardware source

The board support and hardware facts come from the
[official Waveshare repository](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8)
and [product documentation](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8).

## License

MIT. See `LICENSE`. Third-party components keep their own licenses.
