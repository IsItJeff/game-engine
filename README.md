# game-engine

A **moddable co-op embodied colony-sim** — you live inside the town you run, assigning tasks
to NPCs you know by name, building production chains, defending against factions, in a
procedurally growing world where death is permanent and everything is moddable — plus the
C++ engine it runs on. The game is the product; the engine is the long-term product, earned
through the game.

The single source of truth for every decision here is
[docs/design/master-plan.md](docs/design/master-plan.md).

## Status: walking skeleton

There's a running foundation you can build and play with. Run the client (below) and you get
a window with a controllable dot, drifting entities, and a live debug panel — driven through
the whole engine pipeline: input → a **Command** → a **transport** → a **Server** that owns
the **World** → a fixed-60 Hz ECS simulation → interpolated rendering. It's the load-bearing
architecture in miniature: no networking-over-the-wire, no 3D, no game yet — those are named
roadmap milestones.

Start here: **[docs/engine/skeleton/index.md](docs/engine/skeleton/index.md)** explains what
exists and why, with a page per subsystem and a recipe for extending it. The full plan and
milestone order live in [docs/design/master-plan.md](docs/design/master-plan.md) and
[ROADMAP.md](ROADMAP.md).

## Building

Requirements:

- CMake ≥ 3.28 and Ninja
- A [vcpkg](https://github.com/microsoft/vcpkg) checkout with `VCPKG_ROOT` set, at (or
  containing) the pinned baseline `dfcb04008a5a9e038a4ae0d1af57909e30d21359`
- A C++20 compiler (AppleClang on macOS, MSVC on Windows, Clang on Linux)

```sh
cmake --preset dev        # configure: Debug + ASan/UBSan
cmake --build --preset dev
ctest --preset dev        # run the test suite
./build/dev/game/app/game # run the game
```

Other presets: `debug` (no sanitizers), `release`, `ci` / `ci-msvc` (warnings-as-errors,
used by CI).

## Where things are

| Path | What |
|---|---|
| [docs/design/master-plan.md](docs/design/master-plan.md) | The approved master plan — single source of truth |
| `docs/` | MkDocs site: learning handbook + engine docs + roadmap |
| [ROADMAP.md](ROADMAP.md) | Living milestone checklist (M0 → M10) |
| [TESTING.md](TESTING.md) | The three-lane testing policy |
| [code-design-rules.md](code-design-rules.md) | The 12-rule design charter and its enforcement |
| `engine/` | Engine static libraries — no game knowledge |
| `game/` | The game: shared sim, client, server, app executable |
| `tests/` | Catch2 test suites, wired into CTest |

## Ground rules

- **Engine = static libs, game = executable.** Layering is enforced by CMake targets and an
  include-graph check; sim code never includes gpu/platform.
- **Fixed 60 Hz simulation tick**; render interpolates.
- **Single-player = client + embedded server over loopback.** One sim code path for SP,
  listen server, and dedicated server.
- **One command funnel**: every sim mutation from every source (input, network, companion,
  console, mods) enters as a validated, PlayerId-tagged command.
- **Mods are Luau-only — no filesystem, network, or FFI.** This is a written security
  invariant, not an implementation detail. Native C++ plugins will never exist.
- **STOP rule**: a milestone is done when the demo is recorded, exit criteria are met, and
  remaining flaws are *filed* — not fixed.

## License

Code is [MIT](LICENSE) — the engine is free forever. **Game content will live in a clearly
marked proprietary directory later** (planned: game asset/content dirs); everything in this
repository today is MIT.
