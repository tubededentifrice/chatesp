# ChatESP

ChatESP is an open-source, physical voice client for ChatGPT-compatible APIs.
It targets the Waveshare ESP32-S3-Touch-AMOLED-1.8 board and has a small iOS
companion for secure BLE configuration.

## Intended experience

1. Hold the top button and speak.
2. Release it. The display shows the transcript.
3. The device gets a concise answer, shows it, and speaks it.
4. Continue within 30 seconds to use the same thread.
5. Wait 30 seconds, or use a short top-button press, to sleep. The next wake
   starts a new thread.

The interface is black, high-contrast, and similar to a small terminal. Status
motion has a purpose: recording level, network work, tool work, or speech.

## Cloud defaults

- Chat: `deepseek/deepseek-v4-flash` through OpenRouter.
- Speech recognition: `openai/whisper-large-v3-turbo` through OpenRouter.
- Speech synthesis: `google/gemini-3.1-flash-tts-preview` through OpenRouter.
  It returns 24 kHz, 16-bit, mono PCM and gives natural multilingual speech.
- Search: Brave Web Search and Brave Image Search through a provider adapter.

One OpenRouter key can supply chat, transcription, and speech. The provider
interfaces remain replaceable. For example, a later search adapter can use
ScrapingDog without a change to the conversation state machine.

## Repository status

The project is in active initial development. The connected V2 board now runs
the black terminal interface and enters the visible sleep state after 30 seconds.
Native tests cover the interaction state machine. The physical button, voice,
cloud, BLE, iOS, and real low-power paths are not complete. Do not use this
status as a claim that the full device workflow works.

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
uv run --locked python tools/pio.py run -e watch
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
