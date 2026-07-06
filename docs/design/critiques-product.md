# Adversarial Review — Product Viability Lens

*Verification note: I attacked the companion doc's most suspicious factual claim — cpp-httplib server-side WebSocket support — expecting a hallucination. It's real and documented ([README-websocket.md](https://github.com/yhirose/cpp-httplib/blob/master/README-websocket.md), [cpp-httplib repo](https://github.com/yhirose/cpp-httplib)). That claim survives.*

---

## Findings

**1. BLOCKER — The five docs disagree about what the product is, and nobody has ruled.**
*Targets: brief + modding-product §6 vs. architecture, roadmap M10, docs-site (whole).*
The modding-product doc quietly overturns the project's founding premise: "the game is the wedge, not the engine — 'engine as product' gets earned later or not at all." It is correct. But the other four docs still spend as if engine-as-product were live: the architecture doc justifies choices by "your future users will expect it from a product engine," the roadmap reserves M10 ("Engine product v1," API-stability pass, license, template repo, stranger-builds-a-module), and the docs site is architected for an engine-adopter persona that does not exist. You cannot half-pivot: every "product engine" justification that survives into the synthesis will keep pulling effort toward an audience of zero. Godot is MIT with thousands of contributors; Unity/Unreal own the defaults; a solo engine's GitHub page with no shipped game is indistinguishable from the hundreds of dead ones. And strip the branding: this "engine" is an integration of ~10 excellent third-party libraries (SDL3, EnTT, Jolt, GNS, ozz, Recast, Luau…). The only defensible IP is the mod API + netcode integration — which is exactly the game.
**Fix:** The synthesizer must write one sentence at the top of the unified plan: *"We are building a moddable co-op action game. The engine is an internal asset until the game proves demand."* Then delete every downstream commitment that only makes sense for engine-adopters (see findings 2, 5, 12). This is the Minecraft/Factorio path the modding doc already identified; make it official rather than aspirational.

**2. BLOCKER — The Handbook is a second full product with zero adoption value; kill it as spec'd.**
*Targets: docs-site §1–§3; roadmap §5 docs cadence; brief.*
Ten handbook tracks × one-concept-per-page × ≤900 words = plausibly 150–300 pages, each requiring 2+ sources per claim, compiled-and-run code blocks, an adversarial verification pass, Mermaid diagrams, 12-month rot reviews, and weekly link CI. That is a technical-writing career, not a side deliverable. Worse, it fails the product test twice: (a) it competes head-on with learnopengl, Gaffer, Gambetta, learncpp — free, canonical, already winning search — and the docs-site's own source catalog proves the ground is occupied; (b) its audience (people learning engine dev) is precisely the audience that will *not* adopt your engine — learners follow tutorials for Godot/Unity or build their own. A handbook attracts readers, not users. The "2–4 verified pages per milestone" cadence is a permanent ~15–20% tax on the only scarce resource: momentum.
**Fix:** Handbook → an unverified devlog ("what I learned building M5's netcode"), written freely, no template, no verification pipeline, no rot policy. The full verification/style machinery applies ONLY to the mod-author docs (API reference + 5 sample mods + getting-started), which is the surface real users touch. That's ~30 pages, sustainable solo. Revisit the handbook as a product only if the devlog demonstrably draws an audience.

**3. MAJOR — "Secure modding" as the core differentiator dissolves under its own document.**
*Targets: brief ("secure... assume hostile mods... core differentiator"); modding-product §1–§2.*
The modding doc's honest-limits paragraph concedes the position: in-process Luau, no OS isolation, a VM zero-day = arbitrary code execution, dedicated servers should containerize anyway. So the actual guarantee is "well-behaved sandbox against casual/buggy mods" — which is table stakes (Roblox, Factorio, Luanti all have it), not a differentiator. And no *player* has ever chosen a game for its mod sandbox; no *server host* will trust an in-process sandbox over a container. Marketing "secure modding" invites both security researchers and disappointment.
**Fix:** Reposition the differentiator as the thing the bomb-goblin demo actually shows: *"your friend group adds an enemy with a JSON file and 20 lines of Luau, hash-verified into the co-op session, and a bad mod can't crash the game or corrupt saves."* That's ease + multiplayer-compatibility + crash-containment — real, demonstrable, rare. The Luau engineering stays exactly as designed; only the claim shrinks. Never let the word "secure" into marketing copy (the modding doc already says this — enforce it in the synthesis).

**4. MAJOR — No document confronts whether the game will be any good, and the entire strategy is levered on it.**
*Targets: modding-product §6 ("ship the co-op vertical slice as a game people want"); roadmap M8.*
The wedge is "a compelling game came first" — yet across five docs the game is pure dogfood: "one biome, one dungeon, 3 enemy types, 1 boss," Mixamo animations, pre-authorized flat-shaded art. That describes a competent tech demo, and Steam in 2026 buries competent tech demos by the hundred per week. There is no game-design pillar, no answer to "why would a stranger play this over the other co-op action games," no art-production plan beyond "refuse FBX." If the slice lands as mediocre, the modding community never forms, and per finding 1 there is no fallback product. This is the single largest unpriced risk in the plan.
**Fix:** Two actions. (a) The synthesis must demand a one-page game pitch with a hook that is *itself the differentiator in play* — e.g., the game is designed around session-modding ("the host warps tonight's run with mods"), so the product thesis is tested by the game's core loop, not after it. (b) Move the evidence-gathering earlier: at M6–M7, post the grey-box co-op + bomb-goblin-style mod demo publicly (devlog, r/gamedev, Discord) and measure whether anyone asks to try it. That's the cheapest falsification of the whole wedge — 18 months before the slice is done, not after.

**5. MAJOR — "First-party gameplay as a mod, zero private hooks, from day one" is purity you can't afford pre-users.**
*Targets: modding-product §3 (first-party-as-a-mod); roadmap M6 acceptance test.*
The argument for it is genuinely strong (API completeness, dogfooding, docs-by-example) — as a *destination*. As a day-one constraint it means every gameplay feature costs twice: design a public API, then build the feature on it, while you're still learning C++ and the API surface is guaranteed to be wrong the first three times. Factorio and Minecraft both extracted their mod APIs from shipped, private-hook games; neither started API-first. With zero external modders, the discipline has no beneficiary yet and a real velocity cost.
**Fix:** Downgrade from rule to ratchet: gameplay may use private C++ hooks, but each milestone must move one shipped feature onto the public API (the M6 acceptance test — one feature entirely as a module — is the right mechanism; keep it, apply it per-milestone). Hard-enforce "everything as a mod" only at the point a real external modder exists.

**6. MAJOR — The companion feature keeps a milestone and a relay roadmap that all five docs agree it hasn't earned.**
*Targets: companion (whole doc, esp. §4 backend, §4 identity/GDPR); roadmap M11; brief.*
The companion doc itself scores the feature honestly ("maybe one in ten prospective users will care") and its best insight is that the real value is the command-funnel discipline. Then it spends half its length designing the part it says not to build: Durable Objects pricing, Steam `AuthenticateUserTicket`, GDPR posture, hybrid relay phases. That's design inventory that will rot, and M11 grants milestone status to a trailer stunt.
**Fix:** Keep three things: the one-command-funnel rule (free, needed by netcode anyway), the JSON-capable replication schema (needed by mod tooling anyway), and the 1–2 week QR-map demo slotted opportunistically post-slice. Delete M11 as a milestone and delete relay/identity/GDPR sections from the plan entirely — one line ("relay = Cloudflare DO if ever needed") suffices. Never present second-screen as an adoption reason; it's a trailer beat.

**7. MAJOR — RTS/RPG "genre preservation" is a phantom constraint generating real drag.**
*Targets: brief; architecture §4 (flecs revisit trigger); modding-product §4 (preset-rpg/rts).*
No future user adopts an engine for genre presets that don't exist, and no shipped co-op action game is harmed by an ECS that would need rework for RTS-scale queries in 2029. Yet the constraint is already producing costs: flecs re-evaluation triggers, "nothing genre-specific ever enters the C++ core" purity guarantees, preset-pack architecture for genres with zero content. Every hour of generality spent on unbuilt genres is the roadmap's own K5 (engine astronautics) wearing a lanyard.
**Fix:** Strike "preserve RTS/RPG support" from active constraints. The honest version costs nothing: "we use a general-purpose ECS and data-driven content, which forecloses nothing." Re-admit genres as constraints only when a second game is actually planned.

**8. MAJOR — No one has totaled the timeline, and the momentum constraint is load-bearing.**
*Targets: roadmap §1 (M0–M11); brief ("momentum... is a real design constraint").*
A C++ novice doing renderer + physics + skeletal animation + prediction netcode + sandboxed scripting + AI + a content slice + Steam shipping is realistically 3–5 years solo — the roadmap never says a number, and several commitments (API level N−1 support windows, docs DoD, verification passes) assume maintenance capacity that a solo dev mid-build does not have. The brief names motivation as a design constraint; no doc designs for the scenario where it runs out at M5.
**Fix:** Attach rough wall-clock estimates per milestone in the synthesis and define one explicit checkpoint: e.g., "if the M6–M7 public demo (finding 4) produces zero external interest AND I'm >24 months in, the project re-scopes to 'ship the game, archive the platform ambitions.'" A pre-committed off-ramp protects the project from sunk-cost death.

**9. MINOR — License and monetization are punted to M10; decide them now, they're one line.**
*Targets: roadmap M10 ("license chosen"); brief (open-source strategy).*
Deferring the license blocks nothing technically but muddies everything socially: you can't publish source, accept a PR, or state the mod-API terms without it. Realism check the docs never do: donations at this scale are a rounding error; an asset store needs a store; dual-licensing needs a legal entity and buyers.
**Fix:** Decide now: engine/framework code MIT (or Apache-2.0 if patent language matters to you), game content proprietary, revenue = game sales on Steam, engine free forever. Matches the Godot-adjacent ecosystem's expectations, costs nothing, removes a standing decision.

**10. MINOR — Direct contradiction on NPC behavior model; synthesizer must pick.**
*Targets: modding-product §4 ("behavior trees... utility AI nearly impossible for a novice to debug") vs. roadmap M7 ("utility-scoring — simpler than BTs to expose to modders").*
Both cite it as settled; they settle it oppositely.
**Fix:** Behavior trees. The modding doc's argument wins on the product's own terms: the authoring audience is third-party modders, and a BT is inspectable as a tree while utility scores answer "why did it do that?" with a spreadsheet. Roadmap M7 text should be corrected.

**11. MINOR — Docs-site internal inconsistencies and over-tooling at the surviving scale.**
*Targets: docs-site §1 ("one site, two top-level sections" — then three tabs: Handbook | Engine Docs | Roadmap); §3 pipeline.*
Trivial tab-count contradiction; more substantively, if finding 2 lands, the weekly lychee cron, 12-month rot reviews, style-lint CI, and PR-label state machine are dimensioned for a site 10× the size of the surviving mod docs.
**Fix:** Keep `mkdocs build --strict` + the fixed page template + Cloudflare Pages (all cheap and good). Defer lychee/rot-review/style-CI until page count exceeds ~40.

**12. MINOR — M10's "engine product v1" deliverables should become an experiment, not a milestone.**
*Targets: roadmap M10.*
Given finding 1, "template module repo + one stranger builds a module using only the docs" is exactly the right *test* — but as a milestone it presumes the answer. If no stranger volunteers, the milestone can never close and sits as a permanent failure marker (bad for the motivation system the roadmap otherwise engineers carefully).
**Fix:** Reframe M10 as a time-boxed experiment with both outcomes defined: stranger succeeds → invest in the platform story; no takers in 60 days → the game is the product, close the experiment as *answered*, not failed.

---

## Decisions I actively agree with (survived scrutiny)

- **Luau over Wasm/LuaJIT/QuickJS** for v1 scripting — the comparison table is honest and the verdict is right for the actual audience (teenage modders), with Wasm correctly deferred.
- **The game-is-the-wedge analysis** in modding-product §6 — the best strategic paragraph in all five docs; the synthesis should promote it to governing principle (finding 1).
- **The "explicitly do NOT build" list** (no editor pre-M8, no mod portal, no Wasm tier, no in-game browser, no native mobile apps, no Steam tool listing at v1) — correct in every particular.
- **One command funnel + opaque PlayerId reserved early** — cheap now, brutal to retrofit; the companion doc's real contribution.
- **SDL3 + SDL_GPU, Jolt `CharacterVirtual`, EnTT, GNS→Steam Sockets path** — boring, durable, correctly reasoned; the GNS choice specifically because the Steam relay solves player-hosted NAT is a genuinely product-aware call.
- **M3-before-M4 reorder** (client/server split before it's expensive) and **single-player = listen server, no offline branch**.
- **Bomb-goblin as the canonical demo** — JSON + 20 lines of Luau is the whole pitch in one screenshot; build marketing around it.
- **Workshop-shaped-but-not-built distribution** (folders/zips, content-hash matching, mount-from-any-path).
- **The motivation machinery**: STOP rule, demo-video ratchet, 48-hour renderer note, track-switching rule — rare to see solo-psychology engineered this concretely.
- **Docs accessibility mechanics** (fixed H2 template, Atkinson Hyperlegible, link-don't-embed video policy) — keep, applied to the smaller mod-docs surface.

Sources: [cpp-httplib README-websocket.md](https://github.com/yhirose/cpp-httplib/blob/master/README-websocket.md) · [cpp-httplib](https://github.com/yhirose/cpp-httplib) · [cpp-httplib WebSocket issue #260](https://github.com/yhirose/cpp-httplib/issues/260)