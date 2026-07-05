All verification done (the load-bearing citations I spot-checked — cpp-httplib WebSocket support, GNS v1.5, Gregory 4th ed. — are real, so the critique below is about design, not fabricated sources). Findings follow.

# Adversarial Review — Feasibility Lens (solo dev, new to C++)

## Findings

**1. BLOCKER — Nobody has decided where predicted gameplay code lives, and three docs assume three different answers.**
Targets: architecture §4/§6 ("predicted entities use CharacterVirtual + *your movement code*" — C++, re-runnable N times/frame), modding-product §3 ("the *entire* action-adventure gameplay layer ships as `base-action`, a normal mod… the only C++ in gameplay is engine systems"), roadmap M5/M6.
If the third-person controller "glue" is Luau (modding doc's `base-action`), then client-side prediction must re-execute Luau N times per reconciliation tick, inside a VM whose state the same doc declares "disposable" for hot reload — prediction history and disposable VM state are directly incompatible, and cross-machine float determinism of scripted movement is untested anywhere. If the controller is C++ (architecture's position), then `base-action` is **not** the whole gameplay layer, and the "the API is provably complete because the base game is a mod" claim — the product's backbone argument — is false for the single most important system.
**Fix:** declare an explicit carve-out now: *movement/prediction is a C++ engine system configured by data; mods script authoritative server logic and presentation, never the predicted path.* Rewrite modding-product §3's claim to "all gameplay *except the predicted movement core*." This costs one paragraph today; discovered at M6 it costs a rewrite of either the netcode or the mod doctrine.

**2. BLOCKER — The modder asset pipeline contradicts the cooked-only runtime.**
Targets: architecture §4 ("the runtime **never** parses glTF/PNG"; "cooked packs become the mod-distribution surface") vs modding-product §4's flagship demo: `"model": "mod://my-goblins/assets/bomb_goblin.glb"` — a raw glTF loaded from a mod folder at runtime.
Either the runtime parses source formats (contradicting the architecture) or every 15-year-old modder must run your cooker CLI correctly on their machine before their goblin appears — which guts the "JSON + 20 lines of Luau" accessibility pitch. Neither doc notices the other.
**Fix:** decide now: dev/mod path parses glTF/PNG at runtime (cgltf + stb are cheap); the cooker is an *optimization* for first-party shipping content, not a gate. This also resolves finding 13 in your favor of less work.

**3. MAJOR — NPC behavior model: two docs give opposite verdicts with confidence.**
Modding-product: "Behavior trees… utility AI is nearly impossible for a novice to author or debug." Roadmap M7: "NPC behavior authored in Luau (utility-scoring — simpler than BTs to expose to modders)," and its syllabus cites Dave Mark talks arguing utility *beats* BTs for moddable NPCs. A synthesizer merging these blindly ships both.
**Fix:** pick BTs (modding-product's reasoning is stronger for the extensibility persona: trees are inspectable, "why did it do that" has a visual answer, `insert_before` grafting is a real extension mechanism that utility scores don't have). Strike the utility-AI line from M7 and its syllabus entry.

**4. MAJOR — The M6 acceptance test is the exact thing the architecture doc forbids in v1.**
Roadmap M6: "a mod adds a *pickup-and-throw* mechanic." Architecture §4: "Do not attempt… predicted interaction with dynamics (throwing objects in co-op)" — explicitly deferred as a post-v1 scoped experiment. The milestone's proof-of-modding demo sits on the deferred physics path.
**Fix:** either accept a visibly laggy non-predicted throw (fine for co-op — say so in the milestone) or swap the acceptance mod to something clean of dynamics (a dash ability, a healing shrine).

**5. MAJOR — Cross-OS determinism hash in CI is a fragile gate guarding a property the design doesn't need.**
Architecture §7: replay test "hash final state, compare across runs **and across OSes**." Server-authoritative + reconciliation *tolerates* misprediction by construction; nothing in the netcode requires bit-identical MSVC-vs-AppleClang floats. Achieving it requires Jolt's determinism flag plus strict FP flags plus auditing every `std::sin`/GLM path — weeks of work for a newbie, and once Luau mutates the sim (M6) the hash must also cover VM execution. This test will be red constantly and teach the dev to ignore CI.
**Fix:** keep same-binary replay determinism (cheap, catches real bugs like frame-order nondeterminism); delete the cross-OS clause.

**6. MAJOR — No calendar estimates anywhere, and the total is 3–5 years.**
All five docs; roadmap especially. M0–M11 contains at least four multi-month-for-professionals systems (renderer, prediction netcode, sandboxed scripting, the slice's content). No milestone carries a time range, so there is no mechanism to notice being 2x over budget, and "multi-year horizon acceptable" quietly becomes 6 years. For a motivation-constrained solo dev this is the top killer and it's unmanaged.
**Fix:** attach honest ranges per milestone (M0: 4–8 wks; M1: 6–10; … M8: 4–6 *months*) and a pre-committed survivable cut line at each: the interpolation-only netcode fallback exists (good); add equivalents for M6 (server-side scripting only, no client.luau in v1), M7 (2 enemy types, no boss), M8 (one 20-min loop), M11 (map topic only, no inventory commands).

**7. MAJOR — A gameplay UI system is promised by three docs and specified by none.**
Roadmap M8 needs menus/HUD; modding-product promises a "UI widget" sample mod and a `client.luau` UI API; roadmap K5 correctly bans ImGui-as-product-UI thinking ("ImGui inspectors are the editor"). A *modder-facing, scriptable* UI API is a quarter-scale project by itself and appears in no milestone, no dependency list, no killer list.
**Fix:** for the slice, ship hardcoded C++ HUD/menus (ImGui or hand-drawn quads — it's a vertical slice, not the product surface) and explicitly defer the scripted UI API past M10; delete the "UI widget" sample mod from the v1 obligation list.

**8. MAJOR — The docs DoD is a team-sized process assigned to one person.**
Docs-site §3: per page — 2+ independent sources per claim, a second adversarial session compiling and running *every* code block, weekly lychee triage, 12-month review issues; roadmap adds 2–4 handbook pages per milestone. At realistic effort that's 3–6 hours per page × a ten-track handbook. The docs are a product requirement, but this pipeline will either be abandoned (making "verified" a false label) or eat engine time.
**Fix:** keep the template, word caps, `--strict` build, and accessed-dates. Downgrade adversarial verification to *engine-docs quick starts and claims with version numbers*; handbook theory pages get a self-review checklist. Cut the weekly link cron to monthly.

**9. MAJOR — Sandbox runtime: architecture contradicts the doc that actually did the analysis.**
Architecture §8: "hostile-mod requirement points at **WASM/wasmtime** rather than Lua." Modding-product's researched verdict: Luau tier 1, Wasm only-if-demanded; roadmap: Luau. Left unresolved, the synthesizer may keep architecture's note and re-litigate this at M6.
**Fix:** Luau wins (the accessibility requirement — teenagers, no compiler toolchain — is decisive and architecture never weighed it). Delete architecture's WASM lean; keep only "API defined as an IDL-ish surface so a Wasm tier is mechanical later."

**10. MAJOR — Companion's "decide EARLY" list never made it into the roadmap, and one item conflicts with the serializer design.**
Companion §5 correctly flags the one-command-funnel and PlayerId seam as retrofit-expensive — but no roadmap milestone's exit criteria include them, so they will be skipped under M3–M5 pressure. Worse, "replication schema **queryable at runtime** and JSON-capable" conflicts with architecture's templated `Serialize(Stream&, T&)` bitstream design, which is compile-time and has no runtime schema.
**Fix:** add "all sim mutations enter as PlayerId-tagged commands" to M3's exit criteria (it's nearly free there). Resolve the schema claim downward: a `JsonWriteStream` satisfying the same `Serialize` interface gives JSON export with zero new machinery; drop "queryable at runtime" from the requirement.

**11. MAJOR — GNS in month one doubles the newbie's learning surface for no schedule benefit.**
Roadmap M1 runs rendering *and* a GNS chat program in parallel — the heaviest dependency in the stack (OpenSSL/protobuf), on two OSes, wielded by someone still learning what a translation unit is. Nothing consumes the chat program until M3, and the transport is already isolated behind a 6-function interface, so early GNS de-risks nothing that a CI build-smoke-test doesn't.
**Fix:** M0 CI proves GNS *builds* via vcpkg on both OSes (an afternoon); the chat program moves into M3 where its lessons are immediately used. M1 becomes pixels only.

**12. MINOR — Renderer fallback contradicts the architecture and would strand the shader pipeline.**
Roadmap K1 pre-authorizes bgfx as the SDL_GPU fallback; architecture explicitly rejects bgfx (bus factor, bespoke shader dialect) — and a mid-project swap discards the HLSL/shadercross cooking path. **Fix:** the pre-authorized fallback is *cutting renderer features* (flat-shaded art direction, already listed), not swapping APIs. Delete the bgfx line.

**13. MINOR — Cooker "from day one" vs M2's runtime cgltf hot-reload.**
Architecture demands cook-from-day-one and "runtime never parses glTF"; roadmap M2 loads glTF at runtime with hot reload. Per finding 2, resolve toward runtime source parsing for dev and mods; build the cooker at M8/pre-Steam for shipping content. Keep the `version` field and content-hash manifest from day one — those are the actually retrofit-expensive parts.

**14. MINOR — Warnings/exceptions policy contradictions will confuse the learner they're meant to protect.**
Architecture: warnings-as-errors "CI only — locally it kills learning momentum"; roadmap: "`-Werror` from the first commit" as a day-one habit. Architecture: exceptions compiled on, contained at boundaries; roadmap: "exceptions off in the sim." **Fix:** adopt architecture's versions of both (they're the considered ones) and edit the roadmap's habit list.

**15. MINOR — docs-site catalogs sol2 as the scripting primary source, but the stack chose Luau.**
sol2 targets PUC-Lua/LuaJIT; it does not support Luau. **Fix:** replace with luau.org C-API docs and the Luau sandbox guide (which two other docs already cite).

**16. MINOR — "Steam swap is nearly a link-target change" oversells M9.**
Architecture §6. The *transport* swap is small; M9's actual deliverable (friend invites, lobbies, auth tickets, macOS notarization, crash reporting) is a multi-week platform-integration milestone. Keep the claim about transport, but size M9 honestly — it reads like a victory lap and it isn't one.

**17. MINOR — Roadmap M8 says "audio (SDL3 or miniaudio)" — architecture already decided miniaudio.** Delete the "or"; undecided-sounding text in a locked plan invites re-litigation at the worst time (mid-slice).

## What survives scrutiny (actively agree)

- **M3-before-M4**: client/server split before the character exists. The retrofit argument is correct and this is the plan's best sequencing call.
- **SDL3 + SDL_GPU, one renderer, no RHI abstraction until a second backend exists.** Right call for solo + product; the "namespace, not abstraction" framing is exactly right.
- **The K1 renderer hard budget and the 48-hour rabbit-hole protocol** — the strongest anti-killer device in any of the five docs.
- **Luau over Wasm/LuaJIT** for tier-1 modding, with the honest in-process containment disclaimer written out verbatim.
- **"The game is the wedge, not the engine"** — modding-product's brutal §6 is the truest paragraph in the packet and should override the brief's framing wherever they conflict.
- **CharacterVirtual as a pure re-simulable function; predict only the local character; props interpolated.** Correct and correctly justified.
- **Single-player = listen server over loopback, no offline branch.** Non-negotiable and all docs agree.
- **GNS for the Steam-relay endgame**, hash-based mod matching, first-party content as a normal mod (*after* finding 1's carve-out), the STOP rule / demo-video ratchet, docs in-repo on Cloudflare Pages, skipping `mike` until a stability promise exists, and companion v1 as LAN-direct with zero infra.