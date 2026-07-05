All verification done. Here is the complete architecture document.

---

# Engine Architecture & Tech Stack (verified July 2026)

## Layer diagram

```
┌─ game/app (client exe) ─────────┐  ┌─ game/ded (headless exe) ─┐
│ game/client   game/server ◄─────┼──┼── game/server             │
│        └── game/shared ◄────────┼──┼──── game/shared           │
├─ engine ────────────────────────┴──┴───────────────────────────┤
│ scene/anim │ gpu+renderer │ audio │ physics │ net │ asset      │
│ core: platform(SDL3) · ecs(EnTT) · math(GLM) · jobs · log ·    │
│       serialization · assert                                   │
└────────────────────────────────────────────────────────────────┘
tools/cooker (offline: assets + shaders)      docs/ (MkDocs site)
```

Rule: arrows point down only. `game/shared` is the simulation and never includes client or server headers. `ded` never links `gpu`/`audio`. Enforced by CMake target boundaries, proven by the Linux headless CI job.

---

## 1. Language, build, dependencies, CI

**Language: C++20.** Fully solid on MSVC, AppleClang, and Clang in 2026; C++23 is still uneven on AppleClang. Use `tl::expected` (single header) as the stand-in for `std::expected`. *Alternatives:* C++17 (loses concepts/ranges/designated initializers — worse for a learner, not better), C++23 (toolchain roulette on macOS). **Revisit:** bump to C++23 when Xcode's default toolchain ships `std::expected`/`std::print` complete — mechanical change.

**Build: CMake + CMakePresets.** Not lovable, but every dependency below ships first-class CMake support, and your future users will expect it from a product engine. *Alternatives:* Meson (nicer, but you'd write wrapper glue for game deps), Bazel/xmake (ecosystem tax a solo dev shouldn't pay). **Revisit:** effectively never; CMake is the product-compatible answer.

**Dependencies: vcpkg, manifest mode** (`vcpkg.json` committed, builds reproducible via baseline pinning). SDL3, Jolt, EnTT, GLM, spdlog, Catch2, GameNetworkingSockets all have maintained ports. *Alternatives:* FetchContent (fine for header-only, miserable for Jolt/GNS-sized deps — recompiles, no binary caching), vendoring (you become the maintainer of six upstreams), Conan (works, smaller games ecosystem). **Policy:** vcpkg first; FetchContent only for single-header libs without ports; overlay port when a dep needs a patch. **Revisit:** a critical dep with no port and heavy patches → vendor that one dep, not the strategy.

**Warnings/sanitizers:** MSVC `/W4 /permissive-`, Clang `-Wall -Wextra -Wconversion -Wshadow`. Warnings-as-errors in **CI only** — locally it kills learning momentum. ASan+UBSan on the Linux and macOS Debug CI jobs and available as a local CMake preset; MSVC ASan job on Windows weekly (it's slow). As someone new to C++, sanitizers are your seatbelt — run the ASan preset whenever a bug feels "spooky."

**CI: GitHub Actions**, 3-job matrix from day one: `windows-latest` (MSVC), `macos-latest` (AppleClang, arm64), `ubuntu-latest` (Clang + sanitizers + **headless `game_ded` build** — this job existing is what keeps the layering honest). sccache for build caching; unit tests + a determinism replay test (below) on every push.

---

## 2. Platform layer

**Pick: SDL3** (stable since 3.2.0, Jan 2025; now 3.4.x, battle-tested in Steam/CS2/Dota) for windowing, input, gamepads, events, filesystem. It is the single most institutionally durable dependency in games — exactly what a product engine should sit on. *Alternatives:* GLFW (windowing/input only — you'd assemble gamepad, IME, filesystem yourself), raw Win32+Cocoa (months of learning that isn't engine-making), sokol_app (fine, but you'd be off SDL_GPU's happy path). **Revisit:** no realistic trigger on desktop.

Audio device layer: skipped — see §4, miniaudio owns its own backends.

---

## 3. Rendering — the highest-stakes call

**Pick: SDL3 GPU API as the one and only v1 renderer.**

Why, against each criterion:

- **macOS deprecation risk:** SDL_GPU runs **native Metal** on macOS — no GL (frozen at 4.1, deprecated since 2018), no MoltenVK translation layer to debug through. On Windows it's D3D12/Vulkan.
- **Solo + new-to-C++:** it's a modern explicit API (command buffers, render passes, pipelines, no global state) at roughly a fifth of Vulkan's concept count — no descriptor-set layouts, no manual sync hazards, no swapchain recreation dance. This is the difference between a renderer in weeks vs. quarters.
- **Product longevity:** Valve-backed, ABI-stable, shipped in the Steam runtime. Of every option evaluated, it has the best bus-factor and the strongest "will still be maintained in 2032" story. Third parties building on your engine inherit that stability.
- **Debugging:** because it lowers to native APIs, you get first-class tools per OS: RenderDoc/PIX on Windows, Xcode GPU frame capture on macOS.
- **Shader toolchain:** author **HLSL**, offline-compile in the cooker via **SDL_shadercross** CLI → DXIL + SPIR-V + MSL. Shadercross is technically still "preview" (3.0.0-preview2, Apr 2026) but widely used; the risk is confined to a build-time tool, not your runtime. One shader language, three backends, zero runtime compilation.
- **Honest cons:** targets a broad-hardware feature set — no bindless, no ray tracing, limited to what all backends express. A third-person action-adventure slice does not hit these walls.

**Alternatives:**

| Option | Pro | Why not |
|---|---|---|
| Vulkan + MoltenVK | Full control, max features | 3–6 month learning tax; MoltenVK portability-subset quirks on the platform you develop on; solo-killer |
| WebGPU native (Dawn/wgpu) | Clean API, converging `webgpu.h` | Chromium-scale build (Dawn), native-side API still shifting, browser-shaped constraints (buffer mapping, binding model). Strong #2 |
| bgfx | Mature, many backends | Older bind-ful model, bespoke shader dialect, single-maintainer bus factor under a *product* |
| sokol_gfx | Tiny, pleasant | GL-era binding model; header-lib philosophy strains at product-engine scale |
| OpenGL 4.1 | Simplest to learn | Dead branch on macOS; disqualifying for a product with a multi-year horizon |

**RHI abstraction: not now.** Wrap SDL_GPU in a thin engine-owned `gpu::` module (handle types, a Renderer that owns passes) so raw SDL types don't leak into game code — that's a namespace, not an abstraction layer. Write a real RHI only when a concrete second backend exists to design against. **Revisit triggers:** (a) console port signed, (b) a feature you need that SDL_GPU can't express (bindless-scale scenes, RT), (c) SDL_GPU development visibly stalls. The thin wrapper keeps the blast radius to `engine/gpu/`.

---

## 4. Math, ECS, physics, audio, assets, data

**Math: GLM.** The lingua franca; header-only; GLSL-like so shader and CPU code read alike. Fix conventions once in a single header (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, right-handed, +Y up) and document it — convention drift is the classic 3am math bug. *Alternatives:* DirectXMath (Windows-flavored), RTM (faster SIMD, worse ergonomics for a learner), hand-rolled (no — you'd be debugging quaternions instead of shipping). **Revisit:** profiler shows math hot → RTM in that hot system only.

**ECS: EnTT.** Header-only, enormous adoption (Minecraft), sparse-set design. The replication argument: sparse sets give you **per-component pools** — replication walks a pool, checks dirty flags, serializes quantized deltas; entity IDs are stable integer handles that map 1:1 to network IDs via a table. That's the cleanest possible substrate for snapshot/delta netcode. New-to-C++ caveat, stated honestly: EnTT internals are template-heavy and its compile errors are hostile — but the *usage* API (`registry.emplace<T>`, `view<A,B>().each(...)`) is small; keep EnTT includes behind `engine/ecs/`. *Alternatives:* flecs (archetype ECS with first-class entity relationships — genuinely better for the eventual RPG/RTS query patterns, but a C-core API and a much larger concept surface today), hand-rolled sparse set (great learning exercise, wrong for a product others depend on). **Revisit:** relationship-style queries (inventories, hierarchies, RTS groups) dominate the frame → re-evaluate flecs before genre-module work starts; until then parent/child is one component.

**Physics: Jolt.** Shipped in Horizon Forbidden West and Death Stranding 2, actively maintained, CMake-native, deterministic-capable (`JPH_CROSS_PLATFORM_DETERMINISTIC`). *Alternatives:* PhysX (heavier integration, overkill), Bullet (maintenance mode; don't found a product on it).

**Character controller — the prediction-critical decision:** use Jolt's **`CharacterVirtual`**, and understand *why*: it is not a body in the simulation; it's a kinematic object you step explicitly (`ExtendedUpdate(dt, …)`) against the collision world. Character movement therefore *is* a pure function of (character state, input, static world) that you can call N times in one frame — exactly what client-side prediction/reconciliation requires. Policy: **predicted entities use CharacterVirtual + your movement code; dynamic props are server-authoritative Jolt rigid bodies, replicated not predicted.** Do not attempt full-world rollback physics in v1. **Revisit:** design demands predicted interaction with dynamics (throwing objects in co-op) → rewind just those bodies using Jolt's determinism, as a scoped experiment.

**Audio: miniaudio.** One header: device backends (WASAPI/CoreAudio), mixing, and 3D spatialization via its high-level engine API. *Alternatives:* FMOD/Wwise (better tooling, but their licensing infects *your users* — wrong for an engine-as-product core), OpenAL Soft (LGPL, dated), SDL3 audio + hand-rolled mixer (more code for less). **Revisit:** designers need authoring tooling → offer FMOD as an optional module, keep miniaudio the default.

**Assets & cooking:** **glTF 2.0 is the only model/scene interchange** (parse with `cgltf` — tiny, battle-tested; *alternative* assimp is a huge dependency with uneven output). **Cook step from day one:** a `cooker` CLI transforms `assets_src/` → packed engine-native binaries + a manifest of content hashes. Textures: author PNG/EXR → cook to **KTX2** (BC7/BC5, UASTC for portability). Audio: WAV for SFX (PCM), Ogg Vorbis for music/long files. Shaders: HLSL → shadercross → all three backends, same cooker. The runtime never parses glTF/PNG — faster loads, and cooked packs become the mod-distribution surface later. **Revisit:** iteration pain → add a dev-only hot-reload path that shells out to the cooker, not runtime source-format parsing.

**Data formats:** hand-authored = **JSON with comments** (nlohmann/json, `ignore_comments=true`): universal tooling, modder-friendly, and your web/phone companion services speak it natively. Wire + saves = an **engine-owned bitstream serializer** with a single templated `Serialize(Stream&, T&)` per type that works for both read and write (yojimbo-style) — one function, no read/write drift, quantization built in; saves are the same component serializers plus a version header and per-type migration. *Alternatives:* TOML (nicer to hand-edit, worse ecosystem/tooling fit), FlatBuffers/Protobuf for wire (schema codegen buys little when you need per-field quantization anyway and it can't do delta-against-baseline as naturally). **Revisit:** save-migration burden grows → add a reflection layer over the same serializers.

---

## 5. Simulation loop

**Fixed timestep, 60 Hz simulation.** Accumulator pattern ("Fix Your Timestep"): render at native rate, step sim at exactly 1/60, **interpolate rendering between the last two sim states**, clamp accumulated dt (max ~0.25 s) to prevent the death spiral. Fixed timestep is non-negotiable given prediction: re-simulating ticks requires ticks to be a defined unit. 60 Hz because it's an action game (30 Hz shows in melee feel); snapshot *send* rate is separate (§6). *Alternatives:* variable dt (disqualified by netcode and physics stability), 30 Hz sim (revisit-in-reverse: drop to 30 only if the ded server can't hold 60 with full lobbies).

**Input-as-data:** device events → **action mapping** (data-driven bindings file: physical input → named action) → one POD per tick: `InputCommand { tick, move(quantized vec2), yaw/pitch, buttons bitfield }`. This exact struct is what feeds local simulation, gets sent to the server, and sits in the prediction history buffer — input is data, never callbacks into gameplay. Camera look applies at render rate for latency; its result is sampled into the next tick's command.

---

## 6. Client/server & netcode

**Architecture: single-codebase Quake/Source model — the server is always there.**

- `game/shared` — components + simulation systems (ticks the world; pure w.r.t. (state, inputs)).
- `game/server` — authority: accepts InputCommands, ticks shared sim, emits snapshots.
- `game/client` — prediction (re-runs shared movement from last ack'd state through pending inputs), interpolation of remote entities, rendering, UI.
- **Single-player = listen server minus network:** client exe always embeds the server; SP and hosting co-op are the same code path over a `LoopbackTransport` (in-memory queues). **There is no "offline mode" branch anywhere in gameplay code.**
- `game/ded` = server lib + shared lib + headless stub. Built on Linux CI every push.

**Transport: GameNetworkingSockets (GNS).** The product-lens argument is decisive: GNS's API *is* the Steamworks networking API. Ship on Steam later and the swap to Steam Sockets — which brings **SDR relays and NAT traversal for player-hosted games solved by Valve's infrastructure, free** — is nearly a link-target change. Meanwhile open-source GNS gives reliable/unreliable channels, fragmentation, AES-GCM encryption, and connection stats today. Honest cons: heaviest build of the candidates (OpenSSL/protobuf; use the vcpkg port), and the vcpkg port disables standalone P2P/ICE — acceptable because plain UDP covers dev + dedicated servers, and the player-hosted NAT answer is Steam SDR anyway. *Alternatives:* ENet (delightfully simple; no encryption, no NAT story, no Steam path), yojimbo (solid, explicitly dedicated-server-only, no NAT punch), hand-rolled UDP (you would spend a year rebuilding GNS badly). **Revisit:** non-Steam player-hosted P2P becomes a hard requirement → build GNS's ICE support or stand up your own relay.

**Isolation interface** (so the transport never leaks): ~6 functions — `Connect/Listen`, `Send(conn, channel, bytes, Reliable|Unreliable)`, `Poll() → events{connected, disconnected, message}`, `Stats(conn) → {rtt, loss}`. Three impls: Loopback, GNS, (later) SteamSockets. Everything above it sees only this.

**Replication:** server-authoritative snapshot + delta compression against last-acked, client predicts *only its own character*, remote entities interpolate ~100 ms behind. Snapshot send rate 20–30 Hz (decoupled from 60 Hz sim). Interest management deferred until the world is big enough to need it.

---

## 7. Errors, logging, testing, profiling

**Error policy:** compile *with* exceptions on (fighting `-fno-exceptions` against nlohmann/std is not worth it) but **engine APIs don't throw**: `tl::expected<T, Error>` for fallible operations (asset load, net, init); exceptions permitted only inside subsystem boundaries (e.g., JSON parse) and caught at that boundary, converted to `expected`. Programmer errors → `ENGINE_ASSERT` (logs, breaks in debugger, fatal in dev builds, compiled out in ship). Truly unrecoverable (device lost, OOM) → log + fatal. This gives a learner one simple rule: *callers handle `expected`, nobody writes `try` in gameplay code.*

**Logging: spdlog** — dominant, fast, bundles fmt, per-sink levels (console + rotating file), compile-time level stripping for ship builds. *Alternative:* quill (faster, smaller community). Add a category system (`log::net`, `log::gpu`) day one; it's a table of loggers.

**Testing: Catch2 v3** for unit tests (serialization round-trips, math conventions, ECS glue). *Alternative:* doctest (faster compiles, sleepier maintenance), GoogleTest (fine, heavier ceremony). The test that matters most for *this* engine and costs little: **determinism replay** — record an input stream, run the shared sim headless, hash final state, compare across runs and across OSes in CI. It guards the entire prediction/netcode contract and doubles as a physics-determinism canary.

**Profiling: Tracy** — embed the client (a macro per scope), connect the viewer live; frame timeline, plots, locks, memory. Best-in-class and free. GPU side: RenderDoc + PIX (Windows), Xcode GPU capture (macOS). *Alternative:* Superluminal (excellent, paid, Windows-only).

---

## 8. Repository layout

```
/engine/                 # STATIC LIBS ONLY — no game knowledge
  core/                  # platform(SDL3), log, assert, jobs, containers, bitstream
  math/                  # GLM config + conventions header
  ecs/                   # EnTT integration, net-ID mapping, dirty tracking
  gpu/                   # SDL_GPU wrapper + renderer/passes
  physics/  audio/  asset/  net/        # net/ = ITransport + snapshot/delta
/game/
  shared/                # components, InputCommand, simulation systems
  server/                # authority (links: shared, engine sans gpu/audio)
  client/                # prediction, rendering, UI (links: shared, all engine)
  app/                   # game exe = client + embedded server
  ded/                   # dedicated exe = server + headless stub
/tools/cooker/           # asset + shader (shadercross) cooking CLI
/assets_src/             # authored sources (glTF, PNG, WAV, HLSL, JSON defs)
/tests/                  # Catch2 + determinism replay
/docs/                   # MkDocs Material site (handbook + engine docs)
/cmake/  vcpkg.json  CMakePresets.json
```

Layering is enforced mechanically, not by discipline: each directory is a CMake target with PRIVATE includes; `game/shared` cannot see client/server headers because they aren't on its include path; `ded` proving it links without `engine/gpu`/`engine/audio` is a CI job, not a promise.

One deferred decision, named so it isn't forgotten: the **sandboxed mod runtime** (hostile-mod requirement points at WASM/wasmtime rather than Lua) is deliberately out of the v1 slice; the JSON-defs + cooked-pack pipeline above is the v1 extensibility surface and nothing in this stack forecloses the WASM choice.

---

## Sources

- [SDL releases (3.4.x, July 2026)](https://github.com/libsdl-org/SDL/releases) · [SDL3 GPU API docs](https://wiki.libsdl.org/SDL3/CategoryGPU) · [SDL 3 stable release](https://www.phoronix.com/news/SDL3-Official-Release)
- [SDL_shadercross (3.0.0-preview2)](https://github.com/libsdl-org/SDL_shadercross) · [Introducing SDL_shadercross](https://moonside.games/posts/introducing-sdl-shadercross/)
- [Jolt Physics](https://github.com/jrouwe/JoltPhysics) · [Jolt release notes (CharacterVirtual determinism)](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html)
- [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) · [GNS building/vcpkg notes](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/BUILDING.md)
- [EnTT](https://github.com/skypjack/entt/wiki/Similar-projects) · [flecs FAQ](https://www.flecs.dev/flecs/md_docs_2FAQ.html) · [ECS benchmarks](https://github.com/abeimler/ecs_benchmark)
- [Dawn (WebGPU native)](https://github.com/google/dawn) · [WebGPU browser/native status 2026](https://www.webgpu.com/news/webgpu-hits-critical-mass-all-major-browsers/)