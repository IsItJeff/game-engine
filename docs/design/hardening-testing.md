All research done. Here is the report.

---

# Testing Strategy & TDD for the Engine — Researched Recommendation

## Headline position (one recommendation)

**Do not adopt TDD as a blanket methodology. Adopt a three-lane policy: strict test-first for the deterministic core (~25% of the code, ~80% of the risk), test-after for glue, and zero automated tests for look/feel — with a Factorio-style determinism/replay harness as the backbone instead of a big unit-test suite.** Blanket TDD on a game engine is how solo projects die in the mock-object mines; zero testing is how netcode milestones (M5) die in un-reproducible desync hell. The research below says every studio that ships reliably lands on this same split.

Honest cons of my position: you lose the "everything is covered" safety blanket; renderer and gameplay regressions will reach you via your own eyeballs; and the determinism harness I recommend is real upfront work in M2 that delays visible progress.

---

## 1. How real engines and studios actually test (findings)

**Godot (the engine itself):** C++ unit tests via **doctest**, in a `tests/` dir, compiled only with `scons tests=yes`, plus GDScript integration tests (feature/warning/error cases). Philosophy is failure-path heavy — "the engine should not crash and should gracefully recover" — with macros for signal-watching and error-suppression. Unit tests arrived *late* (the 4.0 era, ~15 years into the project) and there is **no documented rendering regression testing**. Test-after culture throughout ([Godot docs](https://docs.godotengine.org/en/stable/engine_details/architecture/unit_testing.html)).

**Unreal/Epic:** three tiers — the [Automation Test Framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-test-framework-in-unreal-engine) (unit/feature/content-stress tests), **Functional Tests** placed in levels, and [Gauntlet](https://dev.epicgames.com/documentation/en-us/unreal-engine/gauntlet-automation-framework-in-unreal-engine) running the *cooked game* end-to-end (boot, run scenario, validate, collect artifacts). Epic's own suites (UE.EditorAutomation, UE.Networking, UE.ErrorTest) are integration-level, not unit-level. Their guidelines are about test *hygiene* (no shared state, clean up files, assume prior bad state) — nothing about test-first.

**Unity ecosystem:** the [Unity Test Framework](https://docs.unity3d.com/Packages/com.unity.test-framework@1.1/manual/edit-mode-vs-play-mode-tests.html) (NUnit; edit-mode for pure logic, play-mode for in-engine coroutine tests) exists and is decent — and the academic literature is blunt that almost nobody uses it seriously: the 2021 [Survey of Video Game Testing](https://arxiv.org/pdf/2103.06431) found game postmortems "mention playtesting but do not mention unit testing or integration testing"; testing is dominated by manual, ad-hoc human play.

**Rare / Sea of Thieves** (the talk you mean is Robert Masella's GDC 2019 ["Automated Testing of Gameplay Features in 'Sea of Thieves'"](https://www.gdcvault.com/play/1026366/Automated-Testing-of-Gameplay-Features) — there is no talk titled "One Team's Quest to Test"; likely a misremembering): the strongest pro-testing data point in games. Built with automation **from day one**, tiered exactly like a pyramid: C++ unit tests → integration tests (test maps/blueprints, behavior-tree transition tests) → gameplay/functional tests (e.g., "skeleton loses sight of player, searches last known position") → **overnight** performance/network/platform tests with virtual server+client processes. Build server runs the suite ~every 20 minutes; devs run tests before code review. Result: weekly live releases with a tiny manual QA team and 100+ internal deployments pre-launch ([Game Developer write-up](https://www.gamedeveloper.com/design/how-rare-automates-testing-for-ai-and-more-in-sea-of-thieves-part-4-of-4-)). The [GDC 2021 Minecraft follow-up](https://www.linkedin.com/pulse/gdc-2021-lessons-learned-adapting-sea-thieves-testing-henry-golding) is instructive for retrofitting: they built a "middle-ground framework that allows unit-like tests to be written for game code *without decoupling the code under test*" — i.e., even Microsoft-scale teams reject the TDD purist's decoupling tax; adoption first, purity never.

**Riot:** the [Build Verification System](https://technology.riotgames.com/news/automated-testing-league-legends) runs ~5,500 *in-game* scripted test cases in ~18 minutes per LoL build (~100k/day), catching ~50% of critical bugs; humans on PBE catch the rest. Note what BVS is: **integration tests driving the real game headlessly**, not unit tests.

**Factorio** (most relevant to your M2/M5): ~800 automated integration tests; determinism verified by **CRC-hashing the whole game state every tick** and comparing across replay runs; desyncs hunted by replaying saves in "heavy mode" (save + hash every tick, diff at divergence) ([FFF-60](https://factorio.com/blog/post/fff-60), [FFF-62](https://www.factorio.com/blog/post/fff-62), [FFF-188](https://factorio.com/blog/post/fff-188)).

**Luau itself** is assured by Roblox via **fuzzing** for memory safety, not unit tests; the sandbox guarantee explicitly excludes "vulnerabilities in custom C functions exposed by the host" ([luau.org/sandbox](https://luau.org/sandbox/), [SECURITY.md](https://github.com/Roblox/luau/blob/master/SECURITY.md)). **Your bindings are the attack surface, not the VM** — that's what your hostile-mod suite must target.

**What everyone tests-first/early:** deterministic sim logic, protocols, save/load. **What everyone tests-after:** integration glue, content. **What nobody automates:** fun, feel, visual quality — Rare uses the Insider Programme, Riot uses PBE, and [Handmade Hero shipped zero test code](https://hero.handmade.network/forums/code-discussion/t/42-test_driven_development_in_game_programming) on the argument that "verifying a polygon is drawn to spec" requires abstraction layers that cost performance, readability and sanity. Strict red-green-refactor TDD is essentially absent from shipped-game practice; the honest industry mode is **"test-with"** — tests written in the same commit as the feature.

**TDD evidence base:** the best industrial study ([Nagappan et al., Microsoft/IBM, 4 teams](https://www.microsoft.com/en-us/research/wp-content/uploads/2009/10/Realizing-Quality-Improvement-Through-Test-Driven-Development-Results-and-Experiences-of-Four-Industrial-Teams-nagappan_tdd.pdf)): pre-release defect density **down 40–90%**, development time **up 15–35%**. That trade is *excellent* for netcode/serialization (defects cost days of debugging) and *terrible* for gameplay iteration (defects cost nothing; iteration speed is everything).

---

## 2. Where TDD pays in *this* stack — and the C++-learner angle

**Genuinely pays (test-FIRST, red-green):**

- **Bitstream serializer** — the single best TDD target in the whole plan. Round-trip is a perfect property (`read(write(x)) == x`), failures are catastrophic-and-silent otherwise, and it's wire *and* saves.
- **Command funnel** — your one-funnel principle makes this the engine's trust boundary (client input AND mod calls flow through it). Test-first: validation, rejection, ordering, tick-assignment. This is also where server-side cheat resistance lives.
- **Protocol/replication state machines** (M3/M5): handshake, join/leave, snapshot/ack, reconciliation. Pure logic once the transport is behind your 6-function interface — which means a **deterministic fake transport** (in-memory queues with scripted latency/drop/reorder) is the highest-leverage test asset you'll build. This is the pay-off for that abstraction.
- **Save migration**: each migration written test-first against golden fixture files of the old version.
- **Math conventions**: a small `test_conventions.cpp` locking handedness, matrix multiply order, quaternion order, GLM↔Jolt↔wire conversions. Written once, test-first, in M1/M2. Cheap insurance against the classic weeks-lost-to-flipped-axis bug.
- **BT interpreter** (M7): tick a tree of scripted stub nodes, assert traversal/reset semantics. Textbook TDD material.

**Pays as test-WITH / test-after:**

- ECS glue and systems (don't test EnTT itself — upstream does), asset loading, JSON schema validation, Luau binding surface.
- **Character controller** — not TDD (you can't red-green "feels good"), but a **simulation harness** with invariants: on a golden test terrain, feed N seeded-random input sequences; assert never-NaN, never-below-floor, step-height respected, deterministic across two runs. Plus golden replays (below).

**Counterproductive (do NOT test):**

- Renderer output (see §3 on snapshots), animation look, audio, gameplay tuning values, UI.
- **Exploratory spikes.** Rule: spikes are throwaway; if a spike survives, it gets rewritten with tests before merging to `main` ("spike and stabilize"). Without this rule, TDD guilt will stop you spiking, and spiking is how you'll learn C++.

**TDD as a C++ learning tool — honest assessment: strong, with one trap.** For your situation the pro case is unusually good: (a) each test is a tiny, fast-compiling program — the fastest feedback loop C++ offers short of a REPL; (b) **tests are the delivery vehicle for ASan/UBSan** — a beginner's UB (dangling refs, iterator invalidation, uninitialized reads) gets caught at test time instead of as a heisenbug in M5; this is arguably worth more than the tests' assertion value; (c) refactoring courage — you *will* rewrite your early C++ as you learn idioms, and a behavioral test net makes that safe. The trap: beginner TDD tends to pin *implementation details* (testing private state, mock-heavy tests of glue), which doubles rework when you rewrite. Mitigation rule: **test only at module boundaries, only observable behavior, no mocking framework — ever.** Hand-rolled fakes (the fake transport) only. Infrastructure yak-shaving is a non-issue in 2026: vcpkg `catch2` + `catch_discover_tests()` in CMake is under an hour, not a project.

---

## 3. Concrete testing policy for this repo

### Per-module policy

| Module | Test type | Notes |
|---|---|---|
| Bitstream serializer | **TDD** + property round-trips + fuzz loop | Seeded-random values, all types, boundary bit-widths; malformed-buffer decode never crashes (run under ASan) |
| Command funnel | **TDD** | Accept/reject/ordering; every new command type ships with tests |
| Math conventions | **TDD-once** conventions file | Locks GLM/Jolt/wire conventions |
| Save/load + migration | **TDD per migration** + golden fixture saves per schema version | Keep old save files in-repo forever |
| Net protocol & replication | **TDD state machines** + integration over fake transport (latency/loss/reorder/duplication scripted) + malformed-packet fuzz | Server must survive arbitrary bytes — trust boundary |
| Prediction/reconciliation (M5) | Deterministic scenario tests: client+server in one process, fake transport, assert convergence after induced misprediction | This is the milestone tests save |
| Sim core (ECS systems) | Test-after unit + **golden replay determinism** | See harness below |
| Character controller | **Simulation harness**: seeded-input invariant tests + golden replays | Feel stays manual |
| Luau sandbox | **Hostile-mod fixture suite** (test-with) | Directory of evil `.luau` files: infinite loop, memory bomb, deep recursion, huge string/table allocs, attempts at `os`/`io`/`require`/metatable escape, cross-VM reach, malformed command spam. Each asserts the limit fires and *server keeps running*. Grows with every binding added. Fuzzing the VM itself: not your job ([Roblox does](https://github.com/Roblox/luau/blob/master/SECURITY.md)) |
| BT interpreter | **TDD** | Stub leaves, assert traversal |
| Navmesh/AI (M7) | Test-after smoke: Detour queries against a golden baked mesh | Don't test Recast internals |
| Renderer/audio/animation look | **None automated.** Headless boot-smoke only | Manual + recorded demo per milestone |
| Whole engine | Headless smoke: boot listen server, run 600 ticks with scripted bot input, clean exit — all 3 CI platforms | Later grows into Riot/Rare-style scenario tests |

### The determinism/replay harness (the backbone — build in M2)

Since the sim is already planned headless-first with a fixed 60 Hz tick and a command funnel, you get Factorio's scheme almost free, and it replaces hundreds of hand-written tests:

1. **State hash**: a function hashing all sim-relevant state (via the bitstream serializer — dual use). 
2. **Record/replay**: record the command stream; replay must reproduce identical per-tick hashes.
3. **Determinism test in CI**: same seed + same recorded commands, run twice → identical hash sequences. Run per-platform strictly; *compare across platforms* as a monitored (non-gating) job — your architecture (server-authoritative, own-character prediction only) absorbs occasional FP divergence via reconciliation, but *systematic* client/server divergence = permanent rubber-banding, so you want to see it early.
4. **Golden replays**: record a canonical replay per milestone (M4: obstacle-course run; M5: netcode scenario). CI replays them and compares hashes. A behavior change = hash change = you consciously re-record. This is your cheap regression net for "did my refactor change simulation behavior."

### Property-based testing: which library?

**Use Catch2's built-in generators (`GENERATE(take(100, random(...)))`) with `--rng-seed` printed on failure; skip rapidcheck.** [rapidcheck](https://github.com/emil-e/rapidcheck) is the only real C++ QuickCheck and it is effectively unmaintained (activity stalled years ago); it works and is in vcpkg, but you'd be adding a dead dependency for one feature — shrinking — that a fixed seed + small generators mostly substitutes for. [Catch2 generators are data-driven rather than true property-based](https://github.com/catchorg/Catch2/blob/devel/docs/generators.md) (no shrinking), which is an acceptable loss at your scale. Revisit only if you hit a failure you genuinely can't minimize by hand.

### Render snapshot testing: **no — deferred indefinitely, and never across real GPUs**

Research verdict: golden-image comparison across GPU vendors/drivers is a known tar pit — driver-specific rasterization differences, timing, and precision make cross-machine image equality meaningless ([visual-test flakiness](https://argos-ci.com/blog/screenshot-stabilization), [driver-specific rendering bugs](https://bugnet.io/blog/how-to-debug-gpu-driver-specific-rendering-bugs)). The industry answer is deterministic **software rasterizers** — Chromium moved its fallback to [Mesa llvmpipe precisely because it "preserves deterministic rendering behavior"](https://botbrowser.io/en/blog/mesa-llvmpipe-vs-swiftshader-chromium-linux/) where [SwiftShader-based image tests were flaky](https://github.com/google/swiftshader). Godot ships without rendering regression tests at all. Policy: nothing until M8+; if a specific shader regression bites you repeatedly then, add golden images rendered **only** on the Linux-headless CI job under **lavapipe** (Vulkan software rasterizer), tolerance-based compare, treated as canary-not-gate. Never image-compare macOS/Metal vs Windows output.

### The solo-dev testing pyramid

- **Base (large): fast pure-logic unit/property tests** — deterministic-core modules only. Full run <10 s locally. This is your inner loop.
- **Middle (medium): headless sim integration** — replay determinism, golden replays, controller harness, client+server-in-one-process netcode scenarios, hostile-mod suite. Full run <2 min.
- **Tip (tiny): 3-platform headless smoke** + soak (nightly-only from M5: 1-hour headless server under bot input, ASan, no leak/crash).
- **Above the pyramid, not in it: you playing the game** — feel, look, fun. Schedule it; it's QA, not procrastination. Record a demo video per milestone as the "manual test artifact."

### Coverage & CI

**No coverage gates, ever.** Coverage on this codebase bifurcates by design: deterministic core should organically sit ~90%+, presentation ~0%, both correctly. Use a coverage report (llvm-cov, occasionally, locally) as a *map* — "which sim branch does no test execute" — never as a target; gating on % makes you write junk tests for glue (Goodhart). CI wiring on your existing 3-job matrix: every push → build + unit/property + sim-integration on all three platforms, **Linux job under ASan/UBSan** (fast machines, no signing hassle); nightly → soak + cross-platform hash comparison + hostile-mod suite under sanitizers. `ctest` via `catch_discover_tests()`; one test binary per module group to keep link times sane.

---

## 4. Roadmap amendments (exact changes)

**M0 — add the TDD kata as the C++ graduation exercise: build the bitstream serializer test-first.** This is the single best amendment available: bit manipulation, integer promotion/UB traps, templates, RAII, `std::span` — every C++ concept you need to graduate, in a module you keep forever, in the format (red-green, sanitizers on) that builds the habit. New M0 exit criteria: (1) Catch2+CTest wired into CMake/vcpkg with sanitizer config; (2) CI runs tests on all 3 platforms; (3) bitstream v1 with round-trip property tests and a malformed-input fuzz loop passing under ASan/UBSan. Add ~1–2 weeks to M0. Also write the one-page `TESTING.md` policy (the table above) into the docs site now — it's a ratchet, like gameplay-as-a-mod.

**M1 (pixels):** no test requirements (it's a spike zone), except `test_conventions.cpp` exit criterion once a coordinate convention is chosen.

**M2 (engine core) — biggest amendment.** Exit criteria add: headless `sim_tests` target; command funnel built TDD with rejection tests; state-hash function; record/replay working; **determinism test green in CI** (same inputs twice → identical hashes). This is ~2 extra weeks that M5 pays back tenfold — Factorio's evidence is that determinism bugs found by harness cost minutes and found in multiplayer cost weeks ([FFF-188](https://factorio.com/blog/post/fff-188)).

**M3 (client/server):** exit adds: fake-transport implementation + handshake/join/leave state-machine tests + loopback integration test in one process + server survives malformed packets (fuzz loop, ASan).

**M4 (character):** exit adds: controller simulation harness with invariant suite on golden terrain; one recorded golden replay; Jolt version bumps must pass the harness before merging.

**M5 (prediction):** exit adds: reconciliation convergence tests under scripted 100–300 ms latency + 5% loss + reordering on the fake transport; nightly soak job begins. *This milestone is why the whole policy exists.*

**M6 (Luau):** exit adds: hostile-mod suite ≥15 fixtures green; every exposed binding lands with at least one abuse test; documented resource limits each proven by a test.

**M7 (AI):** exit adds: BT interpreter TDD suite; navmesh golden-mesh smoke.

**M8 (slice):** exit adds: 1-hour multi-client headless soak, zero crashes/leaks; all golden replays re-recorded and green; snapshot-testing decision revisited (default: still no).

**Honest solo-dev cost accounting:** expect the Nagappan-range 15–35% time overhead *on the deterministic core only* — roughly a net +10–15% on total engine time, front-loaded in M0/M2. What you're buying, per the strongest analog available (Rare: weekly solo-QA-scale releases; Factorio: shippable multiplayer), is M5 and M6 not becoming the milestones where a part-time solo project dies. What you're explicitly *not* buying: any automated opinion on whether the game is good. Nothing in this policy tests fun. Keep playing your own build every week; that habit outranks every test in this document.

**Sources:** [Rare GDC 2019 talk](https://www.gdcvault.com/play/1026366/Automated-Testing-of-Gameplay-Features) ([YouTube](https://www.youtube.com/watch?v=X673tOi8pU8), [Game Developer part 4](https://www.gamedeveloper.com/design/how-rare-automates-testing-for-ai-and-more-in-sea-of-thieves-part-4-of-4-), [GDC 2021 Minecraft adaptation](https://www.linkedin.com/pulse/gdc-2021-lessons-learned-adapting-sea-thieves-testing-henry-golding)) · [Riot BVS](https://technology.riotgames.com/news/automated-testing-league-legends) · [Riot client pipeline](https://technology.riotgames.com/news/running-automated-test-pipeline-league-client-update) · [Factorio FFF-60](https://factorio.com/blog/post/fff-60), [FFF-62](https://www.factorio.com/blog/post/fff-62), [FFF-188](https://factorio.com/blog/post/fff-188) · [Godot unit testing docs](https://docs.godotengine.org/en/stable/engine_details/architecture/unit_testing.html) · [Unreal Automation](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-test-framework-in-unreal-engine), [Gauntlet](https://dev.epicgames.com/documentation/en-us/unreal-engine/gauntlet-automation-framework-in-unreal-engine) · [Unity Test Framework](https://docs.unity3d.com/Packages/com.unity.test-framework@1.1/manual/edit-mode-vs-play-mode-tests.html) · [A Survey of Video Game Testing (arXiv)](https://arxiv.org/pdf/2103.06431) · [Nagappan et al. TDD study](https://www.microsoft.com/en-us/research/wp-content/uploads/2009/10/Realizing-Quality-Improvement-Through-Test-Driven-Development-Results-and-Experiences-of-Four-Industrial-Teams-nagappan_tdd.pdf) · [Luau sandbox](https://luau.org/sandbox/), [Luau SECURITY.md](https://github.com/Roblox/luau/blob/master/SECURITY.md) · [rapidcheck](https://github.com/emil-e/rapidcheck), [Catch2 generators](https://github.com/catchorg/Catch2/blob/devel/docs/generators.md) · [llvmpipe determinism](https://botbrowser.io/en/blog/mesa-llvmpipe-vs-swiftshader-chromium-linux/), [visual-test flakiness](https://argos-ci.com/blog/screenshot-stabilization) · [Handmade Network TDD thread](https://hero.handmade.network/forums/code-discussion/t/42-test_driven_development_in_game_programming)