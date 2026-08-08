# Restricted MicroPython component

This component embeds MicroPython 1.28.0 from the repository submodule at the
commit in `.gitmodules`. MicroPython uses the MIT license. See
`firmware/third_party/micropython/LICENSE` for the complete license.

The ChatESP build includes the compiler, garbage collector, floating-point
math, bounded integer math, and the local `plot` module. It does not include a
filesystem, network access, hardware access, persistent bytecode, native code,
`eval`, `exec`, `sys`, `os`, `gc`, or the `micropython` module.

The application supplies a fixed heap, output buffer, cancellation callback,
monotonic clock, execution duration, operation budget, stack limit, and plot
buffer for each execution. The interpreter state is removed after each call.
