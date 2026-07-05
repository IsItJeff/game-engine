# Architecture

The engine's shape is locked by 22 Architecture Decision Records — one
decision per page, each with the context, the alternatives that lost, and the
consequences accepted. They exist so future work argues with a written record
instead of re-litigating from memory.

A decision changes only by superseding it with a new ADR that links back to
the old one; existing ADRs are never edited into agreement with the present.

| ADR | Decision |
|---|---|
| [0001](adr-0001-game-first-strategy.md) | Game-first strategy — the engine is earned through the game |
| [0002](adr-0002-fixed-60hz-tick.md) | Fixed 60 Hz simulation tick; render interpolates |
| [0003](adr-0003-single-player-is-a-listen-server.md) | Single-player is a listen server over loopback |
| [0004](adr-0004-one-command-funnel.md) | One command funnel for every sim mutation |
| [0005](adr-0005-predicted-movement-is-cpp.md) | Predicted movement is C++; mods never script the predicted path |
| [0006](adr-0006-first-party-as-a-mod-ratchet.md) | First-party gameplay as a mod — a ratchet, not a day-one rule |
| [0007](adr-0007-cpp20-cmake-vcpkg.md) | C++20, CMake presets, vcpkg manifest with pinned baseline |
| [0008](adr-0008-sdl3-platform.md) | SDL3 as the platform layer |
| [0009](adr-0009-sdl-gpu-renderer.md) | SDL3 GPU API as the sole v1 renderer; HLSL via SDL_shadercross |
| [0010](adr-0010-entt-ecs.md) | EnTT for the ECS, quarantined behind engine/ecs |
| [0011](adr-0011-jolt-charactervirtual.md) | Jolt physics; CharacterVirtual kinematic controller |
| [0012](adr-0012-ozz-animation.md) | ozz-animation; glTF-only asset interchange |
| [0013](adr-0013-json-authored-bitstream-wire.md) | JSON for authored data; engine bitstream for wire and saves |
| [0014](adr-0014-gns-transport.md) | GameNetworkingSockets behind a six-function transport seam |
| [0015](adr-0015-luau-modding.md) | Luau as the sandboxed modding runtime |
| [0016](adr-0016-behavior-trees.md) | Behavior trees for NPC AI |
| [0017](adr-0017-errors-expected-boundaries.md) | Errors: tl::expected; exceptions contained at boundaries |
| [0018](adr-0018-testing-three-lanes.md) | Testing: three lanes, determinism harness backbone |
| [0019](adr-0019-solid-seams-dod-core.md) | SOLID at the seams, data-oriented design at the core |
| [0020](adr-0020-mit-license-public-repo.md) | MIT license, public repository |
| [0021](adr-0021-writes-under-prefpath.md) | All writes under SDL_GetPrefPath |
| [0022](adr-0022-imgui-slice-ui.md) | Dear ImGui for the slice UI; real UI stack decided at R1 |

Start with [ADR-0001](adr-0001-game-first-strategy.md) — it frames every
decision after it.
