# Roadmap

A living checklist. Order is the contract; estimates are honest part-time
guesses. Every milestone ends with something that **runs**, and (from M1) a
sub-2-minute public demo video that also shows the previous demo still
working. Full rationale lives in the
[design archive](https://github.com/IsItJeff/game-engine/tree/main/docs/design).

**Total honest estimate: 4–6 years part-time to Early Access launch.**

## Milestones

| # | Milestone | Est. | Cut line | Status |
|---|-----------|------|----------|--------|
| M0 | Toolchain: 3-OS CI, sanitizers, docs site live, GNS build-smoke, Catch2/CTest. Bitstream serializer built test-first (the C++ graduation kata). Console mini-game. Game pitch page. TESTING.md + code-design-rules.md | 6–10 wk | — | In progress |
| M1 | First pixels: SDL_GPU triangle → textured cube → camera + glTF mesh. Conventions test. Buy the Windows test box | 6–10 wk | Flat-shaded look pre-authorized | Not started |
| M2 | Engine core: fixed timestep + interpolation, EnTT world, JSON scenes, ImGui overlay, hot reload. Determinism/replay harness in CI. Command funnel (TDD) | 2.5–3.5 mo | — | Not started |
| M3 | Client/server split: headless ded target, serialized commands, server-authoritative replication, SP = loopback. Fake-transport protocol tests + malformed-packet fuzz | 1–2 mo | — | Not started |
| M4 | Character in a world: Jolt CharacterVirtual, third-person camera, ozz animation, gamepad, Mixamo→ozz retarget prototype. Frame ledger written | 3–5 mo | Ship Jolt sample config as-is | Not started |
| M5 | Real netcode: snapshot interpolation, prediction + reconciliation, lag/loss simulator first, VPS ded server, macOS notarization. Fortnightly co-op playtests begin | 3–5 mo | Interpolation-only + ~100 ms input delay | Not started |
| M6 | Scripting & modding: Luau sandbox, mod API IDL from day one, hostile-mod suite (≥15 fixtures), modder error UX. Acceptance: one feature shipped entirely as a mod | 2.5–4 mo | Server-side scripting only | Not started |
| M7 | NPCs, tasks, colony systems: navmesh + runtime obstacles, behavior trees, task/job system, construction v1, stockpiles + hauling, inventory/equipment, tactical camera + squad orders, identity tiers v1, melee. First stranger playtest | 4–6 mo | Named tier only; 2 enemy types; construction on pre-designated plots | Not started |
| M8a | The colony kernel playable, 30–60 min: one region, ~12 Named + ~20 Crew, task assignment, one deep production chain, NPC shop with trust, hunger + hunting + farming, one escalating dungeon, permadeath + newcomers, difficulty presets, management HUD, co-op | 3–5 mo | One 20-min loop — the kernel list IS the slice | Not started |
| M8b | Productization: versioned save/load + migration policy, kill-during-save fuzz, UI production pass + Deck validation, accessibility criteria, crash reporting, audio mix, FTUE, RELEASE-GATES.md. Steam page ships at M8b start | 3–4 mo | — | Not started |
| M9 | Steam release integration: Steamworks, Steam Sockets + SDR invites, Deck verification, Steam Cloud, EA language decision, UK Ltd before release | 1.5–2.5 mo | — | Not started |
| M9.5 | EA launch gate (a gate, not a time-box): ≥7k wishlists or a written go, ≥99% crash-free, ≥10 testers at 5+ hours, all release gates green. EA ops policy activates | gate | — | Not started |
| M10 | Engine-product experiment, time-boxed 60 days: mod-docs polish, template repo, "a stranger builds a mod using only the docs." No takers → the game is the product; experiment closes as answered | 60 days | — | Not started |
| — | Companion demo (post-slice): HTTP+WS in the sim process, QR pairing, live map topic, commands via the M3 funnel | 1–2 wk | Map topic only | Not started |

## Game ladder

Post-slice content rings. Each ring is one shippable Early Access update, and
each is optional if the previous ring's reception says stop. 1.0 = the ladder
complete; Early Access starts at the kernel.

| Ring | Contents | Status |
|------|----------|--------|
| R1 | Survival + economy depth: thirst/temperature, agriculture chains, research centre, factories, shop network, era tier 3. Exit decides the real-UI-stack go/no-go | Not started |
| R2 | Strategic layer v1: region graph, faction tokens, abstract battles, send-NPCs-on-missions, full Director with dungeon placement | Not started |
| R3 | Procedural regions: tiled navmesh streaming, chunked persistence, hydration, replication relevancy | Not started |
| R4 | Diplomacy: alliances, reputation, trade routes; magic/tech divergence deepens | Not started |
| R5 | Living history: NPC-founded settlements, overseer succession, empire rise/fall, ruins + exodus legacy | Not started |
| R6 | Server persistence modes 2/3 (world-advances, always-on) | Not started |
| R7+ | Era rings to the full stone-age → advanced ladder — the 1.0 promise | Not started |
