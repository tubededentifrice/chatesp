# Hardware measurement receipts

Use one receipt for each resource or power choice. A source value or a firmware
build is not a physical measurement. A receipt closes a gate only when all
fields contain measured evidence from the named board.

Record the full 40-character firmware commit, the board revision, the profile,
the power source, the test load, all compared values, the selected value, and
each open physical gate. Keep the receipt free of device IDs, serial ports,
credentials, request text, audio, URLs, and precise location.

## Open receipts

| Receipt | Board revision | Firmware commit | Profile | Power source | Test load | Compared values | Current selected value | Open physical gate |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Panel clock | V2 first; original after its driver gates pass | Record at test time | `watch_dev` and `watch_prod` | USB and battery | 500 full-screen transfers and 100 sleep/wake cycles | 40 and 80 MHz; use 60 MHz only if 80 MHz fails | 40 MHz | Corruption, stuck flush, transfer, touch, audio, reset, latency, and energy checks are open |
| Display buffer | V2 first; original after its driver gates pass | Record at test time | Both profiles | USB and battery | Images, Clock motion, Wi-Fi, BLE, capture, and playback | 32 rows and each measured candidate | 32 rows in internal DMA memory | Stable DMA memory, latency, and 100-cycle checks are open |
| Task stacks | V2 | Record at test time | Both profiles | USB and battery | 100 voice cycles, 100 sleep/wake cycles, images, plots, radio changes, and TLS | Reviewed values in `task_config.hpp` and each measured candidate | Reviewed task table, including an explicit 7 KiB LVGL stack | Watermarks and no-underrun margins are open |
| Wi-Fi buffers | V2 | Record at test time | Both profiles | USB and battery | Scan, connect, TLS, image, transcription, and speech | Static RX 6, dynamic RX 16, dynamic TX 16, BA window 6, and each measured candidate | 6/16/16/6 | Throughput, TLS success, internal-memory stability, and energy checks are open |
| BLE thresholds | V2 and original | Record at test time | Both profiles | USB and battery | Provisioning, proxy traffic, BLE-to-Wi-Fi-to-BLE changes, and repeated reconnects | 30 KiB largest internal block and 48 KiB free, plus each measured candidate | 30 KiB largest block and 48 KiB free | Controller restart, bond restoration, proxy, and memory-stability checks are open |
| Sleep current | V2 and original | Record at test time | `watch_dev` and `watch_prod` | Battery, with USB absent unless the case states otherwise | Completed development soft sleep and completed AXP2101 production system-off | Development soft sleep and production system-off measured separately | No measured current is selected | Meter voltage, stable current, rail discharge, wake, and USB/charging cases are open |

Do not change a selected value until its receipt shows that the candidate passes
the stated load without a one-way memory loss, PCM underrun, display fault,
touch fault, radio fault, or reset. Add the measured median, worst value, and
energy result when they apply.
