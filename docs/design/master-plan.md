# Plan: Moddable Co-op Colony-Sim → Engine Product, plus a Verified Learning Handbook

## Context

The user (solo, experienced programmer, **new to C++**, first engine, first game, works on **macOS**, UK-based) wants to build a C++ game engine for Windows + macOS with: single-player and multiplayer (listen server + dedicated server), secure third-party modding ("custom modules"), extensible NPC/genre presets, eventual Steam release, web/mobile **companion second-screen** features (phone inventory, live world map), and an **MkDocs Material documentation site** containing both a researched game-dev/engine-dev learning handbook (ADHD/dyslexia-friendly, verified for accuracy) and the engine's own docs.

A previous session's work exists in `gameengine.bundle` (a scaffolded learning-first engine repo). **The user chose to start over from scratch.** Leave the bundle file untouched; it remains recoverable. This plan supersedes it.

Produced by a fresh 5-designer + 3-adversarial-critic multi-agent pass, a hardening pass (testing/TDD research, SOLID/OOP-vs-DOD research, blind-spot hunt), and a final production-readiness + consistency review pass. All 13 documents live in scratchpad at
`/private/tmp/claude-501/-Users-raven-Documents-GitHub-game-engine/52b1e7c7-be84-4661-8695-b3406821f0dd/scratchpad/design-pass/` — **copy all 13 into `docs/design/` at scaffold time so they survive the session**.

### User decisions locked during planning
- **3D committed**; first slice = third-person co-op — concretized July 2026 as the **colony kernel** of the game concept below (embodied colony-sim hybrid; see *The game*).
- **Game-first strategy**: *"We are building a moddable co-op embodied colony-sim; the engine is the long-term product, earned through the game."* Engine-as-product becomes a time-boxed experiment (M10).
- **MIT license, public repo.** Engine free forever; game content proprietary; revenue = game sales on Steam. Engine/SDK on GitHub — **not** a Steam tool listing at v1.
- Web/mobile = companion second-screen only; the game never runs in browsers/phones.
- Docs: **MkDocs Material**, handbook + engine docs, **big upfront handbook push** (Claude researches/writes via verified workflows; user reviews batches).

## The game (concept locked July 2026 — expands into the M0 pitch page)

**One line:** an embodied co-op colony-sim: you live inside the town you run — assigning tasks to NPCs you know by name, building production chains, defending against factions — in a procedurally growing world where towns evolve, empires rise and fall, death is permanent, choices have consequences, and everything is moddable.

**Genre + feel-reference library:** Bellwright, Medieval Dynasty, Going Medieval (embodied colony management), RimWorld (job priorities, consequence), Kenshi (lived-in factions), Mount & Blade (world layer), V Rising (co-op base + servant missions). Hot, underserved niche in co-op — answers "why would a stranger play this."

**Pillars (the descope razor):** 1. NPCs are people, not units (names, personality, morality, trust, permadeath). 2. You are *in* the world (embodied; the tactical view is a tool, not a place you live). 3. The world moves without you. 4. Session-moddable co-op.

**Player:** hybrid embodiment — third-person character + a zoomable tactical/planning camera for base layout and squad orders (time keeps running; it's still your character issuing orders).

**NPC identity tiers** (the "bustling city where you know a select few" answer; each tier is an ECS component set, so promotion = adding components):
- **Named** (~10–30/settlement): full depth — personality, morality, trust, relationships, individual inventory/equipment/skills, quest generation, renameable (preset name pools).
- **Crew** (~50–150/settlement): name + role + few stats; work in teams under a Named overseer; **promoted to Named** by notable events (survives a raid, repeated player interaction, random chance, succession when an overseer dies).
- **Ambient** (hundreds, cities only): schedule-driven crowd, no persistent identity.
Staggered AI scheduling: each NPC "thinks" at ~5–10 Hz round-robin inside the 60 Hz tick; the strategic layer ticks in seconds.

**World model ("endless, lived-in"):** a **region graph + strategic layer**, generated lazily. Regions are seamless playable areas; new adjacent regions generate on demand behind exploration/conquest gates (seeded procgen — endless without infinite streaming). Everywhere without players runs the **strategic simulation**: settlements/factions/armies as tokens with ledgers (population by tier, resources, defenses, relations); wars, alliances, founding, and collapse resolve abstractly; a Named NPC can leave — exiled or by choice — and found a new settlement as a strategic event. Entering a region **hydrates** tokens into concrete places whose building grammar + damage states show their history (conquered looks conquered; eradicated is ruins; new is tents and scaffolding). Dwarf-Fortress-history / Kenshi / X4 lineage. Phases in post-slice via the Game ladder.

**Persistence (user: configurable per server)** — three modes, kept affordable by one rule (local full-sim never runs without players; only the strategic layer differs): (1) **pause-when-empty** (default); (2) **world-advances**: strategic layer ticks at reduced rate while empty, player colonies protected; (3) **always-on** full strategic sim (hardcore, clearly labeled).

**External threats & the Director (user, July 2026):** monsters/demons/zombies attack ALL factions and grant XP (= increments to existing NPC/player skill stats — no separate progression system pre-R1) + unique resources per enemy family. Their source: **dungeons — hostile settlement tokens in the strategic layer from R2 onward** (the M8 slice dungeon runs Director-lite on a standalone per-dungeon ledger and refactors onto strategic tokens at R2): each has a growth ledger (levels up if unmanaged), sends raids the way factions send armies, spawns satellite nests as "expansion," and is removed by clearing it. New-dungeon placement is a **Director** (Left 4 Dead / RimWorld-storyteller pattern), not dice: a per-region threat budget rises with time/prosperity/unmanaged threats and falls with player action; crossing thresholds spends budget to place dungeons under seeded rules (min distance from settlements, near the resources they guard) and pacing rules (breathing room after a crisis, escalation during quiet). The Director is data + Luau — **storytellers are a moddable, marketable feature** (RimWorld's most-loved system).

**Eras & specialization:** the **full development ladder (stone age → advanced sci-fi/high-magic) is the 1.0 promise**; the slice ships tiers 1–2 and each later era is a Game-ladder content ring — systems are cheap (research/building/recipe definitions), content is the cost, and it survives only via a committed low-poly style + per-era commissioned kits. **The whole world starts primitive and climbs in parallel** — rival factions evolve alongside you, so you can watch a neighbor become the cyberpunk city or the crystal-mage city. Magic vs science = **shared trunk + divergent perks**: same underlying mechanics (research, buildings, upkeep) with mana and electricity as two "power" profiles (different production/storage/risk), unique abilities and strengths per branch; hybrid civilizations allowed, balanced by time + material costs. Divergence deepens ring by ring.

**Survival layer (user chose full survival):** hunger/thirst/temperature for players AND NPCs; starvation kills; hunting, agriculture, and farming are jobs and production chains. Full survival × NPC permadeath × colony management is brutal stacked pressure → **difficulty presets are a first-class feature** (per-system toggles: survival severity, Director aggression, permadeath rules). Slice includes hunger + hunting + basic farming; thirst/temperature arrive in ring R1.

**Losing = exodus + legacy:** an overwhelmed settlement falls; survivors flee with carried items and their skills and **found a new settlement elsewhere** (the same strategic-layer machinery as NPC-founded towns). The fallen town persists as ruins/an occupied site — reclaimable later. Fallen civilizations leave discoverable ruins, lore, and small heirloom bonuses in the same world (roguelite legacy). Game over only if every player and Named NPC dies.

**Design positions taken (user can challenge):** no time acceleration in multiplayer (a shared world can't pause; night-skip by sleep vote); economy starts as source/sink ledgers with tuned prices, NOT agent-based simulation — headless economy soak tests run in CI via the determinism harness; resource scarcity seeded per region drives faction conflict; Director pressure scales with settlement prosperity (RimWorld's wealth-curve lesson); quests are data + Luau — NPC-generated quests come from need templates ("low on iron" → fetch/escort/clear), and NPCs evaluate player-created quests against personality/skills/trust.

**Open design questions (for the pitch doc, not blockers):** exact promotion triggers and rates; morality mechanics (axis vs reputation vs behavior weights); player death penalty in a permadeath-NPC world; how far tactical-view orders reach; era count and names for the full ladder; which enemy families map to which unique resources.

## Governing rules (each becomes an ADR at scaffold time)

1. **Fixed 60 Hz simulation tick**, render interpolates, dt clamp ~250 ms. Input is data: action mapping → tick-stamped `InputCommand`.
2. **Single-player = client + embedded server over loopback.** One sim code path for SP / listen / dedicated. No offline branch anywhere in gameplay code.
3. **One command funnel**: every sim mutation from every source (local input, net client, companion phone, console, mods) enters as a validated, PlayerId-tagged command. PlayerId is opaque (local GUID now, SteamID64 later).
4. **Predicted movement is C++** (engine system configured by data). Mods script authoritative server logic + presentation, **never** the predicted path.
5. **First-party-as-a-mod is a ratchet, not a day-one rule**: each milestone from M6 onward (when the API exists) moves ≥1 shipped feature onto the public mod API. Hard-enforce only when a real external modder exists.
6. **Layering enforced by CMake targets**, proven by the Linux headless-server CI job (`game/ded` links without gpu/audio).
7. All writes (saves, settings, logs, mod storage, `mods/` dir) go under `SDL_GetPrefPath` — never beside the executable.
8. Never market the sandbox as "secure." The claim: *a friend adds an enemy with a JSON file + 20 lines of Luau, hash-verified into the co-op session, and a bad mod can't crash the game or corrupt saves.* "Mods are Luau-only — no filesystem/network/FFI" is a written **security invariant**, not an implementation detail.

## Tech stack (verified July 2026; full rationale in design docs)

| Area | Decision | One-line why |
|---|---|---|
| Language/build | C++20, CMake + presets, **vcpkg manifest mode** (baseline pinned; quarterly bump branch must pass CI + 15-min manual smoke both OSes + release-notes read) | All deps have maintained ports |
| Platform | **SDL3** (window/input/gamepad/filesystem) | Most durable dependency in games |
| Rendering | **SDL3 GPU API**, sole v1 renderer; HLSL → SDL_shadercross (offline) → DXIL/SPIR-V/MSL | Native Metal on macOS; ~1/5 Vulkan's concepts; Valve-backed. No RHI until a second backend exists. **Fallback = cut renderer features (flat-shaded art), never swap APIs** |
| Math | GLM + one conventions header (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, right-handed, +Y up) locked by `test_conventions.cpp` | Lingua franca |
| ECS | **EnTT** (behind `engine/ecs/`) | Sparse-set pools = the natural replication substrate |
| Physics | **Jolt**; character = `CharacterVirtual` (kinematic, re-simulable N×/frame) | Prediction needs pure (state,input)→state movement. Predict local character only; props replicate |
| Animation | **ozz-animation** + glTF (Blender→glTF only; refuse FBX; Mixamo retargets — **prototype the retarget pipeline at M4 with one rig**) | Skeletal animation is a named project-killer |
| Audio | **miniaudio** | FMOD licensing would infect engine users |
| Assets | glTF via cgltf, stb_image; **runtime parses source formats** (dev + mods); cooker (KTX2/packs) pre-Steam only | Modders never need a CLI tool |
| Data | Hand-authored = **JSON with comments** (nlohmann) + **schema validation with path/expected/actual error messages**; wire/saves = engine bitstream `Serialize(Stream&, T&)` (one template, read+write); `JsonWriteStream` over the same interface | No second reflection system |
| Netcode | **GameNetworkingSockets** behind a ~6-function transport interface (Loopback / GNS / Steam Sockets); loopback supports simulated latency/loss | GNS's API is Steamworks networking → Steam Datagram Relay solves player-hosted NAT free later. M0 CI proves it builds; first use M3 |
| Scripting/mods | **Luau** (one VM per mod, frozen-global sandbox, VFS jail, ~64 MB cap, interrupt CPU budgets + poison-flag pcall defense, no bytecode loading, **codegen disabled on macOS**). Mod API defined once in an **IDL/JSON descriptor** from M6 day one → generates binding checks + MkDocs reference; semver'd independently; deprecations warn one minor version | Roblox-hardened against hostile code; teen-accessible; WASM tier deferred |
| NPC AI | Recast/Detour (offline bake + **dtTileCache runtime obstacles from M7** — construction changes walkability; full tiled streaming at R3); C++ sensors/blackboard; **behavior trees** (C++ structural nodes, Luau leaves, JSON trees with `extends`/`insert_before` grafting); identity tiers = component sets; staggered think scheduling | Inspectable; "why did it do that" has a visual answer |
| Game UI | Dear ImGui for the slice — menus/HUD **and the colony-management UI (utilitarian, RimWorld-style; explicitly budgeted in M8a)** — + constrained Luau HUD API (text/bars/icons at anchors). Real-UI-stack go/no-go pinned at the R1 exit. OFL fonts only | Scriptable UI is a quarter-scale project |
| Errors | Exceptions contained at subsystem boundaries → `tl::expected`; gameplay never writes `try`; `ENGINE_ASSERT` | One simple rule for a learner |
| Logging/profiling | spdlog · Tracy · ASan/UBSan preset (+ CI); MSVC `/permissive- /W4` from M0 | — |
| CI | GitHub Actions: Windows MSVC, macOS AppleClang, Linux Clang+sanitizers+headless-ded; sccache; warnings-as-errors CI-only | Public repo → free |

## Engineering practices (added at user request — researched)

### Testing policy: "three lanes", not blanket TDD
Strict test-first for the deterministic core (~25% of code, ~80% of risk), test-after for glue, **zero automated tests for look/feel** (manual play + recorded demo is the QA artifact). Backbone = a **Factorio-style determinism/replay harness** (M2): state-hash via the bitstream serializer, record/replay command streams, same-inputs-twice ⇒ identical per-tick hashes, gating in CI per-platform; cross-platform hash comparison runs as a **monitored non-gating nightly**. Golden replays per milestone are the cheap regression net.

| Module | Lane |
|---|---|
| Bitstream serializer | **TDD** — and it is the M0 C++ graduation kata (round-trip property `read(write(x))==x`, malformed-input fuzz loop, under ASan/UBSan) |
| Command funnel | **TDD** (validation/rejection/ordering — it's the trust boundary) |
| Protocol/replication state machines | **TDD** + integration over a **deterministic fake transport** (scripted latency/loss/reorder/duplication); server survives arbitrary bytes |
| Save/load | TDD per migration + golden fixture saves per schema version (old saves kept in-repo forever) |
| Prediction (M5) | Client+server in one process over fake transport; assert convergence after induced misprediction under 100–300 ms + 5% loss |
| Character controller | Simulation harness: seeded-input invariants on golden terrain + golden replays; feel stays manual |
| Luau sandbox | **Hostile-mod fixture suite** (≥15 evil `.luau` files; every new binding lands with an abuse test; each documented limit proven by a test) |
| BT interpreter | TDD with stub leaves |
| Renderer/audio/animation | None automated; headless boot-smoke only. **No golden-image testing** (GPU/driver variance tar pit); revisit M8+ only via Linux lavapipe, canary-not-gate |
| Whole engine | Headless smoke (boot listen server, 600 ticks scripted bot input, clean exit, 3 platforms); nightly 1-hour ASan soak from M5 |

Property testing via **Catch2 generators** (skip rapidcheck — unmaintained). **No coverage gates ever** (core sits ~90% organically, presentation ~0%, both correctly); llvm-cov used occasionally as a map. Honest cost: +10–15% total engine time, front-loaded — bought back at M5/M6, the milestones where solo multiplayer projects die. `TESTING.md` (the table above) written at M0.

### Code design rules: "SOLID at the seams, DOD at the core"
No locked decision changes — the plan's seams already embody the useful parts (ITransport = ISP/DIP/LSP; command funnel = SRP for state; ECS + JSON `extends` = composition/OCP). A 12-rule **`code-design-rules.md` charter** ships at scaffold; highlights:
1. **An entity is an ID** — no entity class hierarchies, ever; capabilities are components (`class Enemy : public Character` never appears).
2. Variation lives in data + scripts, not subclasses.
3. All sim mutation via the command funnel.
4. **Hot loops are dispatch-free**: no virtual calls, allocation, strings, or exceptions in per-entity per-tick code (virtual dispatch at boundaries crossed dozens-of-times-per-tick is fine — Martin/Muratori consensus).
5. No interface without two real implementations today.
6. **Quarantine replaceable libs** (Jolt/GNS/miniaudio types never leave their module); **vocabulary libs used bare** (GLM/EnTT/spdlog — wrapping them is K5 astronautics).
7. Every resource is RAII; no naked new/delete. 8. Values by default; `unique_ptr` owns; `shared_ptr` needs a justifying comment.
9. Module includes point one way (sim never includes gpu/platform) — enforced by a ~20-line CI include-graph script.
10. Seam implementations pass one shared test suite (all 3 transports, every wire type round-trips).
11. Systems are plain functions in one explicit ordered schedule — no ISystem base class, no self-registration.
12. Write the concrete thing first; abstract on the second use.
Enforcement: clang-tidy (`cppcoreguidelines-special-member-functions`, `-owning-memory`, `-no-malloc`, `performance-*`, `modernize-*`), the include-graph CI script, and a PR self-review checklist (solo substitute for a reviewer): *new base class — why not a component? new interface — where's the second impl? virtual/alloc in a tick loop? mutation outside the funnel?*
Handbook gains 6 pages: SOLID in game engines (what transfers/what doesn't) · Composition over inheritance · DOD basics · RAII · The seams of this engine · Value vs reference semantics.

### Solo-dev process
Trunk-based, short-lived branches; every merge to main via self-PR with an **AI review pass** (correctness + over-engineering check). **Backups 3-2-1**: Time Machine + nightly restic/Arq → Backblaze B2 (~£5/mo) covering repo, LFS objects, and raw source assets (Blender/audio files are NOT in git and are the most irreplaceable); WIP branches pushed daily; weekly mirror to a second remote; quarterly restore test (calendar reminder). **Annual go/no-go gate** with pre-written descope options per milestone — burnout, not technology, is the statistically likely killer; the plan says so.

## Repository layout

```
engine/   core/ math/ ecs/ gpu/ physics/ audio/ asset/ net/ script/   # static libs, no game knowledge
game/     shared/ (sim) · server/ · client/ · app/ (client+embedded server) · ded/ (headless)
mods/     base-colony/ …            # first-party gameplay migrates here via the ratchet
tools/    cooker/                    # arrives pre-Steam
assets_src/                          # storage decision BEFORE first binary asset (LFS $ budget vs S3/B2); .gitattributes day one
tests/    docs/ (MkDocs site)
LICENSE (MIT) · THIRDPARTY-NOTICES (CI-generated from vcpkg at M2; fails on copyleft) · SECURITY.md (M6) · TESTING.md · code-design-rules.md
```

## Milestone roadmap (order is the contract; honest part-time estimates; 🎥 = <2-min public demo video; each demo must show the previous demo still working)

| # | Milestone (ends with something that RUNS) | Est. | Cut line |
|---|---|---|---|
| M0 | Toolchain: CI 3-OS green, sanitizers, clang-format, LICENSE/NOTICES, MkDocs live, GNS build-smoke, Catch2+CTest wired. **TDD kata: bitstream serializer test-first** (the C++ graduation exam). Console mini-game. **One-page game pitch** written from *The game* section (fantasy, pillars, one-session player story, co-op + modding hook; re-read at every milestone exit as the descope razor). TESTING.md + code-design-rules.md | 6–10 wk | — |
| M1 | First pixels: SDL_GPU triangle → textured cube → camera + glTF mesh. `test_conventions.cpp` once conventions chosen. **Buy the Windows test box** (~£400–600 used gaming mini-PC) — CI can't catch D3D12 driver behavior, DXIL-only shader bugs, XInput quirks; learn PIX the same week as Xcode GPU capture. "Renders + plays correctly on the Windows box" joins every exit from here on 🎥 | 6–10 wk | Flat-shaded look pre-authorized |
| M2 | Engine core: fixed timestep+interpolation, EnTT world, JSON scenes (`version` field from file #1), ImGui overlay, asset hot-reload. **Determinism/replay harness: state hash + record/replay + CI determinism test green.** Command funnel built TDD. Allocation-tagging hooks (counted allocs per system) while the codebase is small | 2.5–3.5 mo | — |
| M3 | **Client/server split before it's expensive**: headless ded target, input→serialized commands, server-authoritative full-state replication, SP=loopback. Fake transport + handshake/join/leave state-machine tests + malformed-packet fuzz (server survives arbitrary bytes). Exit: command funnel + PlayerId tagging + protocol version int in first connect packet 🎥 | 1–2 mo | — |
| M4 | Character in a world: Jolt + CharacterVirtual (copy samples, tune 1 param at a time, timeboxed), third-person camera, ozz animation, **gamepad path enters** (feel = K4), **Mixamo→ozz retarget prototyped with one rig**. Controller simulation harness + first golden replay. **Frame ledger written** (16.6 ms budget split, Deck-class reference floor; Tracy capture archived at every milestone exit; >20 ms frame in normal play = filed defect). Monthly 2-min "feel check" clip vs the embodied-feel references (Bellwright, Medieval Dynasty, V Rising) begins 🎥 | 3–5 mo | Ship Jolt sample config as-is |
| M5 | Real netcode: snapshot interpolation, prediction+reconciliation (own character only), **lag/loss simulator FIRST**, VPS ded server, `package` CI job. **macOS notarization enters here** (Apple $99/yr — Sequoia+ blocks unsigned builds for playtesters; Windows code-signing cert: skip, Steam doesn't need it). Reconciliation convergence tests under 100–300 ms + 5% loss. Nightly soak begins. **Standing fortnightly co-op session with 1–2 committed friends starts** (scheduled, not "when convenient"); Network Link Conditioner (macOS)/clumsy (Windows) on the checklist. Playtests stay friends-only until M7's public demo 🎥 | 3–5 mo | Interpolation-only + ~100 ms input delay |
| M6 | Scripting & modding: Luau sandbox, manifest/load-order/hash matching, hot reload, NPC preset JSON, **mod API IDL from day one**, SECURITY.md + hostile-mod suite (≥15 fixtures). **Modder error UX is an exit criterion (~20% of M6)**: schema-validated JSON with line-level errors; Luau errors in-game console with mod+script+line+stack; `--validate-mods` headless CLI; broken mod disables itself cleanly. Acceptance: one feature entirely as a mod, clean of dynamics (a new crop + recipe chain, or a consumable shrine buff — sim-authoritative, prediction-free) 🎥 | 2.5–4 mo | Server-side scripting only |
| M7 | NPCs, tasks & colony systems: navmesh bake + **dtTileCache runtime obstacles** (construction changes walkability — pulled forward from R3), sensors/blackboard, BT interpreter (TDD), **task/job system** (work queues, priorities, auto-assignment by skill), **construction v1** (catalog-driven blueprint placement: grid snap + validity feedback; build-site entities consume hauled materials; NPC construct-over-time jobs), **stockpiles + hauling** (zones, hauling jobs, item reservation, priority integration — the classically underestimated colony system; headless hauling-throughput soak joins the harness), **item/inventory/equipment component set** (player + Named NPC) + recipe/workstation JSON model + container/equip UI, **tactical camera + squad orders**, identity tiers v1 (Named + Crew; promotion trigger = survives a raid; overseer succession is R5), melee combat loop. **Public demo checkpoint + first stranger playtest**: post grey-box co-op + a task-mod demo (mod adds a new job type with JSON + Luau); measure interest. Buy used Steam Deck (~£250) 🎥 | 4–6 mo | Named tier only; 2 enemy types; construction snaps to pre-designated plots |
| M8a | **The colony kernel playable**, 30–60 min (**estimate split explicitly into code weeks vs content weeks**): one authored region; ~12 Named + ~20 Crew NPCs; task assignment (gather/build/craft/guard) via tactical view; one production chain deep enough to feel (ore→smelt→smith→equip); one NPC-run shop with trade/trust (stealing damages trust); hunger + hunting + basic farming; **one escalating dungeon** (Director-lite on a standalone per-dungeon ledger: grows if unmanaged, raids at thresholds, clearable to stop spawns; refactors onto strategic tokens at R2) + one hand-authored rival camp (dormant/active states); permadeath + newcomer arrival; difficulty presets v1; **management HUD v1** (needs bars, colonist list with alerts — starving/idle/injured, map overlay from the tactical camera, settings pages); co-op 🎥 solo+co-op | 3–5 mo | One 20-min loop — the kernel list IS the slice (K6) |
| M8b | **Productization**: save/load versioned header {engine ver, API level, mod list+hashes, tick} — **refuse-load applies pre-EA only; from the first paid EA build, every schema change ships a forward migration verified against the golden-fixture suite, and breaking updates ship via an opt-in Steam beta branch with the prior version kept as a legacy branch**; temp-rename + rolling backup + **100-run kill-process-during-save fuzz (zero corruption)**; menus/rebind UI + **colony UI production pass** (custom ImGui theme — fonts, panels, iconography; **tactical camera + task UI operable on gamepad, validated on Deck**); accessibility exit criteria (subtitles, text scale, no color-alone info, motion toggle); `tr("key")` string tables from first UI text; crash reporting (Crashpad+Sentry free tier, dumps carry mod list, opt-in consent + privacy page, symbol archiving per tagged build); audio device-follow + **audio mix pass** (bus structure, combat/UI ducking, volume sliders, master limiter); focus/pause matrix (listen-host alt-tab must NOT pause friends' sim); **FTUE v1** (guided-objective opening scenario on the quest/Luau machinery + contextual hints; exit: 5 cold-start testers reach the first production chain and survive the first raid unaided); **RELEASE-GATES.md written at M8b-start** (p95 frame ≤16.6 ms on Deck at slice-scale colony; load <30 s; ≥99% crash-free sessions via Sentry; zero corrupted saves in the kill-fuzz). **Steam page ships at M8b-START** ($100 fee; wishlists don't decay — early beats late; trademark search UK IPO/EUIPO/USPTO/Steam/domains BEFORE the page); Steam Playtest = co-op playtest channel + wishlist funnel; golden replays re-recorded; 1-hour multi-client soak zero crashes 🎥 | 3–4 mo | — |
| M9 | Steam release integration: Steamworks, GNS→Steam Sockets + SDR friend invites, Steam Input decision, Deck verification, **Steam Cloud** enabled and sized to the rolling-backup save set (or local-only saves explicitly documented on the page), **EA language decision executed** (English-only stated on the page, or zh-CN/de/fr budgeted), **Next Fest = the last fest before launch** (once per game, ever). **Incorporate UK Ltd before release decision** (liability for a multiplayer UGC game + Video Games Expenditure Credit is company-only; W-8BEN in Steamworks tax interview → 0% US withholding; one accountant consult) 🎥 | 1.5–2.5 mo | — |
| M9.5 | **EA LAUNCH GATE** (the commercial milestone — a gate, not a time-box): ≥7k wishlists or a written conscious-go decision; ≥99% crash-free sessions (Sentry) over a final 2-week playtest; ≥10 testers averaging 5+ hours with colonies surviving save/reload across two builds; FTUE re-verified with cold-start testers; all RELEASE-GATES.md bars green; EA FAQ + pricing + public roadmap post drafted. **EA ops policy activates at launch**: rings soak ≥2 weeks on an opt-in beta branch before default; every default-branch push passes golden replays + the save-migration suite + a 1-hour co-op soak; same-day hotfix path from the release branch (migration suite mandatory); one substantive update per 6–10 weeks + a monthly dev post 🎥 launch trailer | gate | — |
| M10 | **Engine-product experiment (time-boxed 60 days)**: mod-docs polish, template repo, "a stranger builds a mod using only the docs." Success → invest in platform; no takers → the game is the product, experiment closes as *answered*, not failed | 60 days | — |
| — | Companion demo (post-slice, ~1–2 wk): cpp-httplib HTTP+WS in the sim process, QR pairing (128-bit token), `map.players` topic at 2–5 Hz, static HTML in binary, commands via the M3 funnel. LAN-only plain HTTP; internet exposure needs admin TLS proxy; relay unbuilt ("Cloudflare DO if ever needed") 🎥 | | Map topic only |

**Game ladder (post-slice rings — each = one shippable Early Access update, each optional if the previous ring's reception says stop):** R1 survival + economy depth (thirst/temperature, agriculture chains, research centre, factories, shop network, era tier 3) → R2 strategic layer v1 (region graph, faction tokens, abstract battles, send-NPCs-on-missions, full Director with dungeon placement) → R3 procedural regions (runtime tiled navmesh streaming, chunked persistence, hydration, **replication relevancy** — per-client region+radius filtering; until R3, NPC counts stay capped at what full-snapshot replication sustains under the loss simulator) → R4 diplomacy (alliances, reputation, trade routes) + magic/tech divergence deepens → R5 living history (NPC-founded settlements, overseer succession, empire rise/fall, ruins + exodus legacy/heirlooms) → R6 server persistence modes 2/3 → R7+ era rings to the full stone-age→advanced ladder (the 1.0 promise). R1's exit also decides the real-UI-stack go/no-go. This ladder is how "almost endless" and "full ladder at launch" both become buildable: 1.0 launch = the ladder complete; Early Access starts at the kernel.

**Total honest estimate: 4–6 years part-time to EA launch.** Milestone estimates sum to ~27–42 months; the total additionally budgets the upfront handbook push, the EA gate, and a 1.4× solo-overrun multiplier — **the total is the commitment, the sum is the floor.** The Game ladder beyond EA is open-ended by design. Off-ramp: M7 demo draws zero interest AND >24 months in → re-scope to ship-the-game, archive platform ambitions. Plus the annual go/no-go gate.

**Motivation machinery:** STOP rule (done = demo recorded + exit criteria met + flaws filed, not fixed) · video ratchet · track-switching after 2 stuck sessions (docs = universal unstick) · renderer rabbit-hole 48-h note protocol · weekly play-your-own-build habit (outranks every automated test).

**Project-killers & pre-authorized fallbacks:** K1 renderer scope creep → the hard budget list (triangle→textured→camera→blinn-phong→1 shadow cascade→skinning→tonemap) IS the whole v1 renderer. K2 skeletal animation → ozz + Blender→glTF only. K3 prediction → interpolation fallback. K4 controller feel → Jolt sample config ground truth. K5 engine astronautics → never hand-roll the pre-authorized list; no editor GUI before M8. **K6 colony-systems creep** → the M8a kernel list IS the slice; any additional system defaults to a Game-ladder ring.

## Content strategy (P0 addition — art is the unbudgeted majority of M8 otherwise)

Stylized flat-shaded/low-poly art direction **chosen explicitly to minimize authored-asset skill**. Base kit = CC0/cheap packs (Kenney, Quaternius, Synty) + Mixamo animation libraries retargeted through ozz (pipeline proven at M4). Audio from royalty-free libraries (Sonniss GDC bundles) + **licensed or commissioned music (~£300–800, 6–10 tracks)**. Budget ~£500–1500 for commissioned hero assets (main character, key SFX, capsule art). Blender for level blockouts only — character modeling/rigging is a separate multi-month craft; do not plan to learn it. Per-asset license manifest in the cooker (CI fails on empty); sample/template mods use CC0 sources only; Mixamo confined to the game depot.

## Money & marketing (P0 addition)

**Lifetime pre-launch cost table (~£1.5–3k, written down so it's a decision, not a surprise):** Apple Developer $99/yr (from M5) · Steam $100 (M8) · Windows test box ~£500 (M1) · used Steam Deck ~£250 (M7/M8) · VPS ~£60–120/yr (M5) · offsite backup ~£60/yr · domain ~£15/yr · capsule art ~£400–1200 · asset/audio packs ~£200–500 · music ~£300–800 · fonts £0 (OFL only) · optional EA localization zh-CN/de/fr ~£1.5–3k (decide by M9).

**Community/marketing:** one home base — written devlog on the docs site, cross-posted to Bluesky/Reddit; **mailing list from the very first public post** (the only owned channel; becomes the playtest pipeline); no Discord before there are players (empty Discord is anti-marketing — defer to M8/Playtest); handbook chapters recycled into devlog posts (same muscle, half the cost). Steam page at M8-start; ~7–10k wishlists is the launch-day algorithm floor; Next Fest last-before-launch.

## Documentation site (MkDocs Material — big upfront handbook push)

**Scaffold (M0, ~day one):** `docs/` in-repo; tabs *Handbook | Engine Docs | Roadmap*; Atkinson Hyperlegible Next + JetBrains Mono; `navigation.indexes`, `content.code.copy/annotate`, admonitions, `pymdownx.details/tabbed/superfences`+Mermaid, `tags`, `git-revision-date-localized`, timetoread. Deploy via **GitHub Pages Actions**. CI: `mkdocs build --strict` + 20-line style-lint on docs PRs; monthly lychee cron.

**Style guide (enforced):** ≤900 words prose/page; one concept per page (title contains no "and"); ≤4 consecutive paragraphs without a visual break; ≤3 heading levels; fixed H2 template — *What it is / Why you care / Quick start / How it works / Pros-Cons / What to expect / Go deeper*; ≤3 admonitions, fixed semantics; Mermaid for any mechanism >2 sentences; code blocks runnable-as-pasted; OS variants in tabs; line-height 1.7, ~70ch, bold never italics; **link videos, never embed** ("22 min — watch after reading").

**Handbook production (upfront push, Claude-executed, user reviews batches):** tracks in consumption order — C++ for Game Devs → Engine Architecture (incl. the 6 new design-principles pages) → Rendering → Physics → Animation → Netcode → AI & Navigation → Scripting & Modding → Tooling (incl. testing-strategy pages) → Shipping. ~10–15 pages/track (~100–150 total). Pipeline per track: multi-source research fan-out → draft to template → **adversarial verify** (different agent: claims vs cited sources; compile/run every code block; version numbers) → publish. Full verification for code/version pages; checklist for theory pages. Sources block + accessed dates on every page. Honest site banner: *"Every page is fact-checked against primary sources before publishing. Process reduces errors; it cannot guarantee zero — found one? File an issue."* Source catalog verified live July 2026 in `designs-docs-site.md` (learncpp, cppreference, learnopengl, gafferongames, Gambetta, gameprogrammingpatterns, Red Blob, Jolt/SDL3/Recast/glTF/ImGui/luau.org docs; videos: TheCherno, Handmade Hero guide, Sebastian Lague, Freya Holmér, SimonDev, Acerola, GDC talks — incl. Rare's Sea of Thieves automated-testing talk and the Overwatch netcode talk).

**Engine Docs** grow with milestones: Getting Started M2+, Multiplayer M5, Modding concepts/**IDL-generated API reference**/examples M6+ (product surface, full verification). ADRs one per page. `mike` versioning deferred until a compatibility promise exists.

## Explicitly NOT building

Native C++ plugins (ever — security invariant) · game on web/mobile/console · agent-based economy simulation in v1 (ledgers first) · time acceleration in multiplayer (sleep-vote night-skip instead) · full always-on persistence before Game-ladder R6 · editor GUI before M8 · scripted UI framework in v1 · WASM mod tier until demanded · mod portal/in-game browser · native companion apps · companion relay · RTS/RPG preset packs ("general ECS + data-driven content forecloses nothing" costs zero) · rollback/lockstep netcode · cross-OS determinism as a gate (monitored nightly only) · golden-image render tests · coverage gates · visual scripting · voice chat · Steam tool listing · Workshop before players (but mod = self-contained dir, id-addressed, mountable from any path — honored now) · Discord before players · rapidcheck.

## Open decisions (owner: user, decide by the milestone named)

1. **Engine + game name** (trademark search before the M8 Steam page; repo/site currently "game-engine").
2. **Binary asset storage** (before first art commit): Git LFS with ~$5/50GB budget vs S3/B2 + hash manifest.
3. Steam Deck: target confirmed enough to buy hardware at M7/M8; formal verification statement by M9.
4. **EA launch languages** (by M9): English-only stated explicitly on the Steam page, or budget zh-CN/de/fr (~£1.5–3k) — colony sims over-index in those markets.

## Execution order (first sessions after approval)

1. **Scaffold**: LICENSE (MIT), README, `.gitattributes`, CMake+presets+vcpkg.json (SDL3, EnTT, GLM, spdlog, Catch2, tl::expected; Jolt/GNS/ozz/Luau at their milestones + GNS build-smoke now), 3-OS CI, clang-format + clang-tidy config, include-graph CI script, `docs/design/` populated from the 11 scratchpad design-pass docs, ADRs from Governing rules + stack table, ROADMAP.md, TESTING.md, code-design-rules.md.
2. **Docs site scaffold + GitHub Pages deploy** (landing page: two doors + quick start + honest pre-1.0 banner).
3. **Handbook push**: research/write/verify track batches via workflows (C++ track first — it IS the M0 curriculum), user reviews each batch.
4. **M0 engine work**: hello-toolchain green on all 3 CI jobs; bitstream TDD kata; console mini-game; game pitch page; backup automation (restic→B2 + second remote mirror).

## Verification

- **Scaffold**: all 3 CI jobs green on first push; `cmake --preset` builds + `ctest` passes locally; `mkdocs build --strict` green; Pages live.
- **Each handbook batch**: adversarial verify pass with zero unresolved findings; code blocks compiled/ran as pasted.
- **Each engine milestone**: runnable artifact + recorded demo; determinism replay, save round-trip, hostile-mod suite (from M6), kill-process-during-save fuzz (from M8b), golden replays in CI on every push; net milestones must pass under the loss/latency simulator (100 ms + 5% loss), not just localhost; from M1, "renders + plays correctly on the Windows box" is part of every exit.
