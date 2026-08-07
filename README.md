# ChatESP

ChatESP is an open-source, physical voice client for ChatGPT-compatible APIs.
It targets the Waveshare ESP32-S3-Touch-AMOLED-1.8 board and has a small iOS
companion for secure BLE configuration.

## Intended experience

1. Hold the bottom PWR button and speak.
2. Release it. The display shows the transcript.
3. The device gets a concise answer, shows it, and speaks it.
4. Continue within 30 seconds to use the same thread.
5. Wait 30 seconds, or use a short PWR-button press, to sleep. The next wake
   starts a new thread.

The interface is black, high-contrast, and similar to a small terminal. Status
motion has a purpose: recording level, network work, tool work, or speech.

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

The project is in active initial development. The connected V2 board now runs
the black terminal interface. The bottom PWR button starts the hold-to-talk
state sequence. A short press or 30 seconds of inactivity turns off the AMOLED
and requests AXP2101 system-off. A PWR-button wake starts a new thread. A held
wake starts the recording state directly. The top BOOT button has no app action.
Native tests cover the interaction state machine and button filter. Voice,
cloud, BLE, iOS, and battery-current measurements are not complete. Do not use
this status as a claim that the full device workflow works.

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

Use development mode during normal firmware work. It keeps the device awake
when the 30-second sleep timer expires. This keeps USB flashing available:

```sh
uv run --locked python tools/pio.py run -e watch_dev -t upload
```

Use production mode only for final power tests and release images. It enables
AXP2101 system-off after a sleep request:

```sh
uv run --locked python tools/pio.py run -e watch_prod -t upload
```

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
