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

Use the official managed component
`waveshare/esp32_s3_touch_amoled_1_8`. The BSP is the source of truth for pins
and supported board revisions.

## Button behavior

The top button is the user action button:

- short press from idle: sleep;
- hold: record while held;
- release after recording: submit.

The second button has no product behavior yet. A diagnostic build can use it
only when the diagnostic screen and documentation state this clearly.

## Power behavior

Before deep sleep, stop audio, radio work, display updates, touch, and unused
peripherals. Configure only a supported button wake source. Turn the AMOLED off
through the BSP or PMU path. Clear the in-memory thread. Measure sleep current
on battery hardware before making a battery-life claim.

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
