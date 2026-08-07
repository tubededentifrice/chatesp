# ChatESP

ChatESP is an open-source, physical voice client for ChatGPT-compatible APIs.
It targets the Waveshare ESP32-S3-Touch-AMOLED-1.8 board and has a small iOS
companion for secure BLE configuration.

## Intended experience

1. Hold the bottom PWR button and speak.
2. Release it. The display shows the transcript.
3. The device gets a concise answer. Text appears as the model sends it. Speech
   starts after a short buffer on a fast link. A slow link buffers enough audio
   to keep playback clear.
4. Continue within 30 seconds to use the same thread.
5. Wait 30 seconds, or use a short PWR-button press, to sleep. The next wake
   starts a new thread.

The interface is black, high-contrast, and similar to a small terminal. The
bottom line shows Wi-Fi state and battery level. Status motion has a purpose:
recording level, network work, tool work, or speech.

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
Wi-Fi, HTTPS, streamed model text, streamed speech, search, BLE provisioning,
and sleep paths. The iOS companion can store settings in Keychain and send them
over encrypted BLE. A selected, bounded JPEG can appear full-screen after the
spoken answer.

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

Use production mode only for final power tests and release images. It enables
encrypted persistent settings and AXP2101 system-off after a sleep request.
You can build it without a device change:

```sh
uv run --locked python tools/pio.py run -e watch_prod
```

The first production start can write an HMAC key to an eFuse block. This
operation cannot be reversed. The repository wrapper blocks production upload
until the user gives explicit approval.

See [development mode](docs/development-mode.md) for the mode contract and the
recovery procedure.

Read device logs with the repository monitor. The generic serial monitor can
request the ESP32-S3 ROM loader when it opens this board:

```sh
uv run --locked python tools/watch_monitor.py --port LOCAL_PORT --duration 10
```

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
