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

The local `chatesp_board` component is based on the official
`waveshare/esp32_s3_touch_amoled_1_8` component at a reviewed source commit. It
starts the CO5300 at zero brightness. The app draws a black frame before it
raises the brightness. The current package always creates a CO5300 panel. It
probes both touch types, but it does not select an SH8601 display. The ChatESP
board adapter must add the original-board SH8601 path before original-board
support is complete.

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
button is GPIO0. It has no normal app action and stays available for firmware
recovery.

## Power behavior

Before sleep, stop audio, radio work, display updates, touch, and unused
peripherals. Turn the AMOLED off and request AXP2101 system-off. A PWR press
then causes a cold boot and a new thread. Measure current on battery hardware
before making a battery-life claim.

The connected V2 board must pass these checks for this control change:

- a held bottom PWR press shows `LISTENING`, and release shows `TRANSCRIBING`;
- a held PWR press longer than six seconds does not stop the board during a
  recording;
- a short PWR press from idle turns the screen off and requests system-off;
- 30 seconds without input turns the screen off and requests system-off;
- a held PWR-button cold start enters `LISTENING` directly;
- the top BOOT button does not change application state;
- cold start does not show a white frame.

The current USB-power checks do not verify battery sleep current.

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
