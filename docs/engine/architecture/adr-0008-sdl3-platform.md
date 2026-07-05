# ADR-0008: SDL3 is the platform layer

**Status:** accepted (2026-07).

SDL3 provides the window, input, gamepad, and filesystem abstraction for Windows and macOS. It is the one dependency the engine sits directly on top of, and it also supplies the GPU API (ADR-0009) and the pref-path write policy (ADR-0021).

Why: SDL is the most durable dependency in games — decades of maintenance, Valve-backed, first-class on every target we ship, and SDL3 is the actively developed generation with the GPU API included. For a solo developer, platform-layer breadth (gamepad hotplug, IME, high-DPI, filesystem paths) is exactly the code that is miserable to write and maintain per-OS.

Rejected: GLFW (window/input only — no gamepad depth, filesystem, or GPU API; we'd re-assemble SDL from parts); platform-native code per OS (2× the work for zero product value); SDL2 (superseded; SDL3's GPU API is the renderer decision's foundation).
