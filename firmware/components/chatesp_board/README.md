# Local Waveshare board package

This directory contains the Waveshare board package at commit
`241f74ab98aab76eb4cea92ca47d42395b5c57a3`.

ChatESP keeps this local copy for its display start and recovery changes. The
CO5300 starts at zero brightness. The app draws its first black frame before it
sets the normal brightness. This change prevents a white frame during startup.
An in-session wake replays the panel initialization table without resetting
the panel or touch controller. This repairs a controller that accepts wake
commands but keeps its pixels black.

The upstream license is in `LICENSE`.
