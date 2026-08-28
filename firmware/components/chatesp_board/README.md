# Local Waveshare board package

This directory contains the Waveshare board package at commit
`241f74ab98aab76eb4cea92ca47d42395b5c57a3`.

ChatESP keeps this local copy for its display start and recovery changes. The
board adapter probes CST820 at `0x15` before FT3168 at `0x38` and caches the
result. It selects the V2 CO5300 with a 16-pixel gap or the original SH8601 with
no gap. An unknown revision uses the V2 display path and disables touch. The
SH8601 driver is fixed at version 2.0.0. Original-board operation is not
physically verified.

On a cold ESP start, the adapter pulses only TCA9554 EXIO0, EXIO1, and EXIO2
low for 20 ms. It preserves the PWR input, audio amplifier, SD state, and all
other expander bits. The adapter is the only owner of the expander and its
masked read-modify-write operations.

Both panels use a 40 MHz QSPI clock. They start at zero brightness. The app
draws two black frames before it sets the normal brightness. An in-session wake
replays the selected panel table without a controller reset. Runtime display
commands use one display-control task. The local single-point touch reader
treats each read failure as a release and does not log coordinates. GPIO13 is
reserved for the panel and must not be configured.

The upstream license is in `LICENSE`.
