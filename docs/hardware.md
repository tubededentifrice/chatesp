# Hardware contract

## Target

The target is the Waveshare ESP32-S3-Touch-AMOLED-1.8.

- ESP32-S3, 16 MB flash, 8 MB PSRAM
- 368 by 448 QSPI AMOLED
- Original display and touch: SH8601 and FT3168
- V2 display and touch: CO5300 and CST820
- ES8311 codec with microphone input and speaker amplifier
- AXP2101 power management
- QMI8658 six-axis IMU
- PCF85063A real-time clock
- MicroSD over SDMMC
- Two physical buttons

Use the official `waveshare/esp32_s3_touch_amoled_1_8` component as the board
base. Pin it to a reviewed source commit. The current 2.0.3 display start always
creates a CO5300 panel. It probes both touch types, but it does not select an
SH8601 display. The ChatESP board adapter must add the original-board SH8601
path before original-board support is complete.

Probe the touch controller after reset release. Address `0x15` identifies V2.
Address `0x38` identifies the original board. Do not infer the display revision
from a failed display start.

## Button behavior

The top PWR button is the user action button. It connects to the AXP2101, not to
an ESP GPIO. Read the PMU press and release events through I2C while awake:

- short press from idle: sleep;
- hold: record while held;
- release after recording: submit.

Disable the PMU automatic long-hold shutdown during product use so a normal
recording does not turn off the board. Keep a tested recovery path.

The second BOOT button is GPIO0 and has no product behavior. A diagnostic build
can use it only when the diagnostic screen and documentation state this
clearly.

## Power behavior

Before sleep, stop audio, radio work, display updates, touch, and unused
peripherals. Turn the AMOLED off and request AXP2101 system-off. A PWR press then
causes a cold boot and a new thread. PMU system-off is the primary path because
PWR cannot be an ESP deep-sleep wake GPIO. Measure current on battery hardware
before making a battery-life claim.

## Physical acceptance gates

- Identify the connected board revision.
- Verify both buttons and the selected wake source.
- Verify AMOLED black level, rotation, touch mapping, and full-screen image.
- Record and replay speech through the ES8311 path without clipping.
- Verify Wi-Fi connection and TLS requests.
- Verify encrypted BLE provisioning and acknowledgement with a physical iPhone.
- Measure idle, recording, Wi-Fi, playback, and deep-sleep current.
- Run at least 100 talk cycles and 100 sleep/wake cycles without a leak, reset,
  stuck state, or unexpected NVS write.
