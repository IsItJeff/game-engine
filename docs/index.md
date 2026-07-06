# Game Engine Docs

!!! warning "Pre-1.0 — everything here can and will break"
    This is a solo-built engine and game, in the open, years from stable.
    APIs change without notice, milestones slip, and the
    [roadmap](engine/roadmap.md) is the only promise. If that sounds fun,
    welcome.

A C++20 game engine and a moddable co-op colony-sim, built in public — with a
fact-checked handbook documenting everything learned along the way.

## Pick your door

<div class="grid cards" markdown>

-   **Learn engine development**

    ---

    The Handbook: a track-by-track curriculum — C++, architecture, rendering,
    physics, netcode, AI, modding — every page verified against primary
    sources before it ships.

    [:octicons-arrow-right-24: Start the Handbook](handbook/index.md)

-   **Use or mod the engine**

    ---

    Engine Docs: the game pitch, the milestone roadmap, and the architecture
    decision records that explain why the engine is shaped the way it is.

    [:octicons-arrow-right-24: Read the Engine Docs](engine/index.md)

</div>

## Quick start

Builds on Windows, macOS, and Linux. You need CMake ≥ 3.25, a C++20 compiler,
and [vcpkg](https://github.com/microsoft/vcpkg).

```sh
git clone https://github.com/IsItJeff/game-engine.git
cd game-engine
export VCPKG_ROOT=/path/to/vcpkg   # where you cloned vcpkg
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The `dev` preset is Debug with ASan/UBSan enabled — the default way to work
on this codebase. `release` exists when you want speed instead of sanitizers.
