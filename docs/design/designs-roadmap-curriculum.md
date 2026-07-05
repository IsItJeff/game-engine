# Roadmap & Curriculum — Engine-as-Product, Solo, Windows+macOS

**Locked stack assumptions (all verified alive as of July 2026):** [SDL3 + SDL_GPU](https://wiki.libsdl.org/SDL3/CategoryGPU) (shipped Jan 2025, console-proven via FNA), [EnTT](https://github.com/skypjack/entt) (used in Minecraft), [Jolt Physics](https://github.com/jrouwe/joltphysics) (Horizon Forbidden West, Death Stranding 2; active 2026), [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) (v1.5 shipped April 2026 — Valve woke it up after 4 years), [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) (MIT, active 2026), [Luau](https://luau.org/sandbox/) (sandboxed-by-design; adopted by Alan Wake 2, Warframe, Farming Sim 25 — this is your hostile-mod answer), [Recast/Detour](https://github.com/recastnavigation/recastnavigation) (industry standard, active).

---

## 1. Milestone Roadmap

**One deliberate reorder of your skeleton:** dumb client/server replication moves *before* physics/animation (M3), and prediction moves *after* the character exists (M5), with scripting *after* netcode. Reasons: (a) retrofitting a client/server split into a single-process engine is the single most expensive retrofit in this plan; (b) you can't predict a character controller you haven't built; (c) the Luau API must expose a sim that already has authority rules, or every mod API you publish bakes in client-side assumptions you can never remove (it's a *product* — API mistakes are forever).

**Retrofit-expensive decisions, and the milestone that locks them in:** fixed timestep + sim/render separation (M2), headless server target + server authority (M3), input-as-serializable-commands (M3), stable network-safe entity IDs (M3), asset/save format versioning fields from file one (M2/M8), scripting only at the sim boundary (M6).

| # | Milestone | Ends with (RUNS) | 🎥 |
|---|---|---|---|
| **M0** | **C++ onboarding + skeleton.** CMake project, CI on Windows+macOS, sanitizers, clang-format, warnings-as-errors. Write a tiny console game (e.g. roguelike-lite) to exercise RAII/containers/ownership. | Green CI on both OSes; console game runs. | |
| **M1** | **First pixels & first packets.** SDL3 window → SDL_GPU triangle → textured cube → 3D camera + loaded mesh. Separately: GNS client/server chat over UDP. Keep them separate programs. | Spinning textured cube; two terminals chatting over GNS. | 🎥 |
| **M2** | **Engine core.** Fixed-timestep loop with interpolated rendering; EnTT world; glTF loading (cgltf); JSON scene files with a `version` field; Dear ImGui debug overlay; asset hot-reload. | Fly-cam around a glTF scene; edit scene file, see it reload. | |
| **M3** | **Client/server split — before it's expensive.** Headless server build target (this is also your future dedicated server AND companion-service host). Input becomes serialized commands. Server owns sim; client renders snapshots. Dumb full-state replication, no prediction. Single-player = local loopback server (one code path forever). | Two clients connect; each sees the other's cube move. | 🎥 |
| **M4** | **Character in a world.** Jolt integration on the server sim; `CharacterVirtual` controller; third-person camera; ozz skeletal animation (idle/run/jump blend) client-side. | Controllable animated character on real terrain; feels good. | 🎥 |
| **M5** | **Real netcode.** Snapshot interpolation for remote entities; client-side prediction + reconciliation for *your own character only*; lag/loss simulator; deploy headless server to a cheap VPS. | Co-op movement over the internet with 150ms simulated latency, no rubber-banding. | 🎥 |
| **M6** | **Scripting & modding.** Embed Luau (sandbox per [luau.org/sandbox](https://luau.org/sandbox/)); expose the sim API; module manifest with declared permissions; script hot-reload; NPC-preset data format. Acceptance test: implement one gameplay feature *entirely* as a module, using only documented APIs. | A "mod" adds a pickup-and-throw mechanic without touching C++. | 🎥 |
| **M7** | **AI & NPCs.** Recast/Detour navmesh baked from scene geometry; NPC behavior authored in Luau (utility-scoring — simpler than BTs to expose to modders); basic melee combat loop, health, damage. | NPCs path, chase, and fight two co-op players. | 🎥 |
| **M8** | **Vertical slice.** 20–40 min of content: one biome, one dungeon, 3 enemy types, 1 boss, combat polish, save/load (versioned from day one), audio (SDL3 or miniaudio), menus/HUD. Mostly *content and feel*, not engine code — resist engine work here. | Full slice, start to boss kill, solo and co-op. | 🎥 record both |
| **M9** | **Steam + shipping.** Steamworks SDK; listen-server via Steam Datagram Relay + friend invites (GNS's API mirrors [ISteamNetworkingSockets](https://partner.steamgames.com/doc/api/ISteamnetworkingSockets) — this is why GNS was the right transport); crash reporting; macOS signing/notarization; Steam playtest with strangers. | Friend installs from Steam, joins your game via invite. | 🎥 |
| **M10** | **Engine product v1.** API-stability pass on the Luau surface (deprecation policy written down); template module repo; docs site complete for the mod-author persona; license chosen; one external human builds a module using only the docs. | A stranger's module runs in your engine. | 🎥 |
| **M11** | **Companion services.** Headless server exposes an authenticated WebSocket/HTTP state API (cheap — M3 made the server the single source of truth); web live map + inventory page; phone-browser friendly. | Move an item from your phone; see it in-game. | 🎥 |

M10 and M11 can swap or interleave; nothing in M11 blocks M10.

---

## 2. The Five Project-Killers

**K1. Renderer scope creep (kills at M1–M2 and forever after).** Kills because rendering has infinite visible progress and infinite depth; solo engines become renderers with no game. *Sub-steps:* triangle → textured mesh → camera/transforms → forward lighting (blinn-phong, a few lights) → cascaded shadow maps for one sun → skinned meshes → one post pass (tonemap). **That list is the entire v1 renderer. Hard budget.** *Fallback:* if SDL_GPU fights you, [bgfx](https://github.com/bkaradzic/bgfx) (mature, multi-backend). Pre-authorized art direction fallback: stylized/flat-shaded look — hides a simple renderer, ages better than bad PBR.

**K2. Skeletal animation (kills at M4).** Kills because it's math-dense (skinning matrices, bind poses), import pipelines are hostile, and a broken result is a horrifying mess of stretched limbs with no error message. *Sub-steps:* render a rigged mesh in bind pose → sample one clip with ozz → GPU skinning → 2-clip blend → blend tree (idle/walk/run by speed) → root motion *deferred to post-slice*. *Pre-authorized fallbacks:* ozz-animation **is** the plan (don't hand-roll); pipeline fallback: Blender → glTF → ozz tools, one path only, refuse FBX; content fallback: Mixamo clips retargeted in Blender — zero hand animation for the slice.

**K3. Client-side prediction & reconciliation (kills at M5).** Kills because it's the first system where bugs are *emergent and timing-dependent* — everything works on localhost, then explodes at 150ms; solo devs burn out re-architecting. *Sub-steps:* lag simulator FIRST (non-negotiable) → tick-numbered snapshots → interpolation of remote entities → client re-simulates own inputs from last acked state → reconcile → smooth misprediction correction. Predict **only** the local character; NPCs and physics props are interpolated, never predicted. *Pre-authorized fallback:* co-op tolerates latency PvP doesn't — ship the slice with interpolation-only + input delay (~100ms). Playable co-op beats abandoned prediction.

**K4. Character controller feel (kills at M4, silently).** Kills because it's unbounded subjective tuning; "it feels wrong" has no stack trace, and a bad-feeling character poisons motivation for all later work. *Sub-steps:* capsule + gravity via Jolt `CharacterVirtual` → slopes/steps/ground-snap (Jolt's samples solve these — copy them) → jump with coyote time + input buffering → camera (spring-arm, collision) → acceleration curves. Timebox tuning to sessions with a recorded before/after clip each. *Fallback:* Jolt's shipped `CharacterVirtual` sample config as ground truth; deviate one parameter at a time.

**K5. Engine astronautics: custom ECS / custom formats / editor-before-game (kills anywhere).** Kills because "engine as product" tempts you to build generality nobody asked for; rewriting the core resets the project. *Pre-authorization list — never hand-roll:* ECS = EnTT, physics = Jolt, transport = GNS, animation = ozz, navmesh = Recast, script VM = Luau, mesh format = glTF, config = JSON. **No editor GUI before M8**; text scene files + hot reload + ImGui inspectors are the editor. The product differentiator is the *module system*, not any of these wheels.

---

## 3. C++ Onboarding (M0, ordered)

Minimum before engine code — for an experienced dev this is weeks, not months:

1. **Compilation model**: translation units, headers vs source, include guards, the linker, ODR. This is the part with no analogue in your other languages; most "C++ is confusing" pain lives here.
2. **Value semantics & `const`**: copies by default, references, pass-by-`const&`.
3. **RAII + Rule of Zero**: destructors as the resource mechanism; you almost never write one.
4. **Ownership & smart pointers**: `unique_ptr` (default), `shared_ptr` (rare), raw pointer = non-owning view. Never raw `new`/`delete`.
5. **Core containers**: `vector` (the answer 90% of the time), `unordered_map`, `string`/`string_view`, `span`, `optional`.
6. **Move semantics** — usage-level only (what `std::move` does; that returns move automatically). Skip writing move constructors.
7. **Lambdas + range-for + `auto`.**
8. **CMake, minimum viable**: one exe, one lib, `FetchContent` for deps, toolchain on both OSes.

**Resources:** [learncpp.com](https://www.learncpp.com/) (free, current through C++20/23 — do chapters selectively, not linearly; you know what an if-statement is), *A Tour of C++* (Stroustrup, 3rd ed) as the fast survey for experienced devs, CppCon **"Back to Basics"** YouTube playlists (RAII, move semantics, smart pointers — one talk per topic above), [Compiler Explorer](https://godbolt.org) for instant experiments, [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) as a skimmed reference, not a read.

**Explicitly DEFER:** template authoring beyond `vector<T>` usage, concepts/SFINAE, custom allocators (until a profiler demands it), coroutines, C++20 modules (tooling still not worth it), multithreading (engine stays render-thread + everything-else until post-slice), exception-design debates (pick: exceptions off in the sim, error codes/`expected`; move on), operator overloading, CRTP and other pattern zoo entries.

**Day-one habits (cheapest on day one, agonizing later):** `-Wall -Wextra -Werror` from the first commit; ASan+UBSan on in all debug builds and CI; clang-format with any config, enforced pre-commit; clang-tidy core checks; CI on Windows *and* macOS from M0 (cross-platform rot is exponential); no raw owning pointers, ever.

---

## 4. Per-Track Syllabi (consume at the milestone shown)

**Rendering (M1–M2, then stop reading rendering content until post-slice):**
- [LearnOpenGL](https://learnopengl.com/) (Joey de Vries, free) — read for *concepts* (pipeline, transforms, lighting, shadow maps, and its skeletal-animation guest chapter) even though you'll write SDL_GPU; it's the best-ordered graphics pedagogy that exists. M1.
- [SDL_GPU docs/examples](https://wiki.libsdl.org/SDL3/CategoryGPU) + the community SDL_gpu examples repos — your actual API. M1.
- *Foundations of Game Engine Development Vol 1: Mathematics* (Lengyel) — the math reference. Alternatively Freya Holmér's "Math for Game Devs" YouTube series — uniquely good visual intuition for vectors/quaternions. M1, revisit at M4.

**Engine architecture (M2, ongoing):**
- [*Game Programming Patterns*](https://gameprogrammingpatterns.com/) (Nystrom, free online) — read *Game Loop*, *Update Method*, *Component*, *Data Locality* before M2. Short, perfectly pitched.
- [*Game Engine Architecture*, 4th ed.](https://www.routledge.com/Game-Engine-Architecture-Two-Volume-Set/Gregory/p/book/9781041162599) (Gregory, published April 2026, now two volumes, C++23-current) — the reference shelf. Vol I at M2, Vol II chapters at M4. Read per-system as you build, never cover-to-cover.
- [EnTT wiki crash courses](https://github.com/skypjack/entt/wiki) — the ECS you're actually using. M2.
- [TheCherno's Game Engine series](https://www.youtube.com/@TheCherno) (YouTube) — uniquely good for *watching engine decisions get made in C++*; the early ~40 episodes cover exactly M1–M2 territory. Note: the channel/Hazel's future looked uncertain in early 2026, but the existing videos are the value. M1–M2.
- Bonus video: "The Poor Man's Netcode"/Overwatch GDC talks below double as architecture education.

**Netcode (skim at M3, deep at M5):**
- [Gabriel Gambetta, *Fast-Paced Multiplayer*](https://www.gabrielgambetta.com/client-server-game-architecture.html) (4 parts + [live demo with code](https://www.gabrielgambetta.com/client-side-prediction-live-demo.html)) — the gentlest correct explanation of prediction/reconciliation/interpolation. Read at M3, implement against at M5.
- [Gaffer On Games](https://gafferongames.com/) (Glenn Fiedler) — *Fix Your Timestep* is required at **M2**; the UDP/snapshot/networked-physics series at M5. The deepest free material on this topic.
- Valve dev-wiki *Source Multiplayer Networking* — the classic production write-up (lag compensation, interp). M5.
- GDC: *"Overwatch Gameplay Architecture and Netcode"* (Tim Ford, free on YouTube/GDC Vault) — uniquely good for ECS+netcode *together*, i.e., your exact architecture. M5.
- [GNS docs](https://github.com/ValveSoftware/GameNetworkingSockets) + [awesome list](https://github.com/gafferongames/GameNetworkingResources). M1/M3.

**Physics & character controller (M4):**
- [Jolt samples + docs](https://jrouwe.github.io/JoltPhysics/), especially the [CharacterVirtual](https://jrouwe.github.io/JoltPhysics/class_character_virtual.html) sample — copy first, understand second.
- GDC *"Math for Game Programmers: Physics"* talks (Squirrel Eiserloh et al., free) — intuition without building a physics engine.
- GMTK *"Why Does Celeste Feel So Good?"* + Celeste's freely published player-controller source — uniquely good for *feel* parameters (coyote time, buffering) even though 2D; the ideas transfer wholesale. M4.

**Skeletal animation (M4):**
- [ozz-animation docs/samples](https://guillaumeblanc.github.io/ozz-animation/) — runtime + glTF import toolchain; the samples are the tutorial.
- LearnOpenGL *Skeletal Animation* chapter — the theory (bind pose, skinning matrices) in one sitting.
- Gregory 4th ed. Vol II animation chapters — when you need blend trees done properly.

**AI / navmesh (M7):**
- [Recast/Detour](https://github.com/recastnavigation/recastnavigation) — use RecastDemo to understand bake parameters before integrating.
- [Game AI Pro](http://www.gameaipro.com/) chapters (free online) — utility AI and architecture chapters; uniquely good practitioner depth for free.
- GDC *"Building a Better Centaur: AI at Massive Scale"* / utility-AI talks (Dave Mark) — why utility scoring beats behavior trees for moddable NPCs. M6–M7.

**Luau / scripting & modding (M6):**
- [luau.org](https://luau.org/) — language, C API delta from Lua, and the [sandboxing guide](https://luau.org/sandbox/), which is literally a how-to for your hostile-mod requirement.
- *Programming in Lua* (PIL, 1st ed free online) — semantics still apply to Luau.
- Study one shipped moddable-game API for surface design: Factorio's or Roblox's docs — uniquely good examples of *versioned, permissioned* mod APIs, which is your product's core.

---

## 5. Scope Governance & Motivation Engineering

**The STOP rule.** A milestone is done when: (1) the checkpoint demo is *recorded*, (2) exit criteria written at milestone *start* are met, (3) known flaws are filed as issues, not fixed. "I'll just clean this up first" is the failure mode — the issue tracker is where perfectionism goes to wait. A system is done when the demo shows it working, not when it's general.

**Demo-video ratchet.** Every 🎥 milestone produces a <2-minute recorded video, posted publicly (devlog thread/YouTube/Discord). Two functions: motivation (visible progress trail — reread it on bad weeks) and *regression testing by video* — each demo must show the previous demo's feature still working. If M5's video can't show M4's character animating, you broke something; the ratchet never moves backward.

**Track-switching rule.** When stuck on one problem for 2+ sessions with no new information, you must switch to a pre-declared secondary track (each milestone names one, e.g., M5-stuck → M6's Luau spike, or docs). Announce the switch in your devlog — that makes it a plan, not a defeat. The universal unstick track is docs-writing: always available, always product progress, and explaining the stuck system usually unsticks it.

**Renderer rabbit-hole protocol.** The K1 budget list is the whole v1 renderer. Any graphics feature beyond it requires a written note: "this blocks the vertical slice because ___", left to sit for 48 hours. If the sentence is still true in 48h, schedule it; it almost never is. Post-slice, deliberately budget one "renderer treat" per milestone — scheduled sugar prevents binges.

**Docs cadence (continuous, not phase-9).** Set up MkDocs Material in M0 (it's a 30-minute job) with the two sections: *Handbook* and *Engine Docs*. Then: every milestone's exit criteria include **2–4 handbook pages written from that milestone's learning notes** — you're learning netcode at M5 anyway; the notes *are* the handbook draft, and milestone-sized notes naturally produce the short, one-concept pages your accessibility spec demands. Engine reference docs start at M6, because the Luau API is the documented product surface — docs for a moving C++ internal API before then are waste. Verification pass on handbook pages happens once per milestone, batched. M10 is docs *completion and polish*, not docs writing.

**Honest cons of this whole plan:** M3-before-M4 means your first months produce cubes, not characters — it front-loads the least glamorous work; the ratchet only pays off if you actually accept the 100ms-input-delay fallback should K3 bite; and the engine-as-product goal stays *dormant* until M6 by design — resist building for imaginary users before you have a game proving the engine works.

Sources: [SDL3 GPU](https://wiki.libsdl.org/SDL3/CategoryGPU) · [SDL3 release](https://www.phoronix.com/news/SDL3-Official-Release) · [Jolt](https://github.com/jrouwe/joltphysics) · [GNS 1.5](https://www.phoronix.com/news/GameNetworkingSockets-1.5) · [EnTT](https://github.com/skypjack/entt) · [ozz](https://github.com/guillaumeblanc/ozz-animation) · [Luau sandbox](https://luau.org/sandbox/) · [Luau adoption](https://en.wikipedia.org/wiki/Luau_(programming_language)) · [Recast](https://github.com/recastnavigation/recastnavigation) · [Gambetta](https://www.gabrielgambetta.com/client-server-game-architecture.html) · [Gaffer](https://gafferongames.com/post/networked_physics_2004/) · [Gregory 4th ed](https://www.routledge.com/Game-Engine-Architecture-Two-Volume-Set/Gregory/p/book/9781041162599) · [learncpp](https://www.learncpp.com/) · [LearnOpenGL](https://learnopengl.com/) · [TheCherno/Hazel](https://github.com/TheCherno/Hazel)