# Engine Docs

Documentation for the engine itself: what is being built, why, in what order,
and the decisions that lock its shape. These pages grow with the milestones —
Getting Started lands with M2, Multiplayer with M5, the modding API reference
with M6.

## Start here

- **[Game Pitch](pitch.md)** — the game the engine exists to ship. One page.
  Re-read at every milestone exit; anything that doesn't serve it gets cut.
- **[Roadmap](roadmap.md)** — the milestone ladder from toolchain to Early
  Access, kept live as a checklist.
- **[Architecture](architecture/index.md)** — the Architecture Decision
  Records. Twenty-two decisions, one page each, with the alternatives that
  lost and why.

## What the engine is

A C++20 engine for a moddable co-op colony-sim on Windows and macOS
(plus a headless Linux dedicated server): SDL3 platform and GPU API, EnTT
ECS, fixed 60 Hz simulation with client prediction, Jolt physics, Luau
sandboxed modding. MIT licensed, public repo, free forever — the game funds
the work.

The engine is the long-term product, earned through the game. Until the game
proves it, every engine feature must justify itself against the
[pitch](pitch.md).

## Status

Pre-M1. The toolchain, CI, and this docs site are live; first pixels are the
next milestone. See the [roadmap](roadmap.md) for what "done" means at each
step.
