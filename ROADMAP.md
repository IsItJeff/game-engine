# Roadmap

Living checklist. The full narrative lives in
[docs/design/master-plan.md](docs/design/master-plan.md) and the docs site at
[docs/engine/roadmap.md](docs/engine/roadmap.md). Order is the contract. 🎥 = ends with a
<2-minute public demo video.

**Reminders (read before every session):**

- **STOP rule** — a milestone is done when the demo is recorded, exit criteria are met, and
  remaining flaws are *filed*, not fixed.
- **Video ratchet** — each demo must show the previous demo still working.
- **Track switching** — stuck for 2 sessions on the same thing? Switch tracks; docs work is
  the universal unstick.

## Milestones

- [ ] **M0 — Toolchain** *(ends with: hello-toolchain green everywhere + the pitch written; 6–10 wk)*
  - [ ] CI green on all 3 OSes (Windows MSVC, macOS AppleClang, Linux Clang+sanitizers+headless)
  - [ ] MkDocs Material site live on GitHub Pages
  - [ ] GameNetworkingSockets build-smoke passes
  - [ ] Bitstream serializer TDD kata (round-trip property + malformed-input fuzz, under ASan/UBSan)
  - [ ] Console mini-game runs
  - [ ] One-page game pitch written (the descope razor, re-read at every milestone exit)
  - [x] TESTING.md
  - [x] code-design-rules.md
  - [ ] Backups configured (restic→B2 nightly + second remote mirror)
- [ ] **M1 — First pixels** 🎥 *(ends with: SDL_GPU triangle → textured cube → camera + glTF mesh, verified on the newly bought Windows box; 6–10 wk; cut line: flat-shaded look pre-authorized)*
- [ ] **M2 — Engine core** *(ends with: fixed timestep + EnTT world + JSON scenes + ImGui overlay + hot reload, and the determinism/replay harness green in CI; 2.5–3.5 mo)*
- [ ] **M3 — Client/server split** 🎥 *(ends with: headless ded target, server-authoritative replication, SP = loopback, command funnel + PlayerId + protocol version in the first connect packet; 1–2 mo)*
- [ ] **M4 — Character in a world** 🎥 *(ends with: Jolt CharacterVirtual + third-person camera + ozz animation + gamepad, Mixamo→ozz retarget proven, frame ledger written; 3–5 mo; cut line: ship Jolt sample config as-is)*
- [ ] **M5 — Real netcode** 🎥 *(ends with: snapshot interpolation + prediction/reconciliation passing under 100–300 ms + 5% loss, VPS ded server, notarized macOS builds, fortnightly friend co-op begins; 3–5 mo; cut line: interpolation-only + ~100 ms input delay)*
- [ ] **M6 — Scripting & modding** 🎥 *(ends with: Luau sandbox + hostile-mod suite + mod API IDL + modder error UX, and one real feature shipped entirely as a mod; 2.5–4 mo; cut line: server-side scripting only)*
- [ ] **M7 — NPCs, tasks & colony systems** 🎥 *(ends with: navmesh + BT AI, task/job system, construction v1, stockpiles + hauling, inventory/equipment, tactical camera + squad orders, identity tiers v1, melee loop, public demo + first stranger playtest; 4–6 mo; cut line: Named tier only, 2 enemy types, construction on pre-designated plots)*
- [ ] **M8a — The colony kernel playable** 🎥 *(ends with: a 30–60 min slice — one region, ~12 Named + ~20 Crew, one deep production chain, shop/trust, hunger + hunting + farming, one escalating dungeon, permadeath + arrivals, management HUD v1, co-op; 3–5 mo; cut line: one 20-min loop — the kernel list IS the slice)*
- [ ] **M8b — Productization** 🎥 *(ends with: versioned save/load + kill-fuzz zero corruption, UI production pass + Deck-validated gamepad, accessibility, crash reporting, FTUE v1, RELEASE-GATES.md, Steam page live at M8b-START; 3–4 mo)*
- [ ] **M9 — Steam release integration** 🎥 *(ends with: Steamworks + Steam Sockets/SDR invites, Deck verification, Steam Cloud, EA language decision executed, UK Ltd incorporated before the release decision; 1.5–2.5 mo)*
- [ ] **M9.5 — EA launch gate** 🎥 *(ends with: launch — a gate, not a time-box: ≥7k wishlists or written conscious-go, ≥99% crash-free, ≥10 testers at 5+ hours across two builds, all release gates green; EA ops policy activates)*
- [ ] **M10 — Engine-product experiment** *(ends with: an answer — a stranger builds a mod using only the docs, or the experiment closes as answered, not failed; time-boxed 60 days)*
- [ ] **Companion demo** 🎥 *(post-slice: HTTP+WS in the sim process, QR pairing, live map topic, commands via the M3 funnel; ~1–2 wk; cut line: map topic only)*

## Game ladder (post-slice Early Access rings — each optional if the previous ring's reception says stop)

- [ ] **R1** — survival + economy depth (thirst/temperature, agriculture chains, research centre, factories, shop network, era tier 3); exit decides the real-UI-stack go/no-go
- [ ] **R2** — strategic layer v1 (region graph, faction tokens, abstract battles, NPC missions, full Director with dungeon placement)
- [ ] **R3** — procedural regions (tiled navmesh streaming, chunked persistence, hydration, replication relevancy)
- [ ] **R4** — diplomacy (alliances, reputation, trade routes) + magic/tech divergence deepens
- [ ] **R5** — living history (NPC-founded settlements, overseer succession, empire rise/fall, ruins + exodus legacy)
- [ ] **R6** — server persistence modes 2/3 (world-advances, always-on)
- [ ] **R7+** — era rings to the full stone-age → advanced ladder (the 1.0 promise)

## The honest total

**4–6 years part-time to EA launch.** The milestone estimates sum to ~27–42 months; the
total additionally budgets the upfront handbook push, the EA gate, and a 1.4× solo-overrun
multiplier — **the total is the commitment, the sum is the floor.** The Game ladder beyond
EA is open-ended by design. Off-ramp: if the M7 demo draws zero interest AND the project is
>24 months in, re-scope to ship-the-game and archive the platform ambitions. Plus the
annual go/no-go gate with pre-written descope options.
