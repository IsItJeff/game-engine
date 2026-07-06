# Production-Readiness Review — Final Pass

## Findings

**1. BLOCKER — Save policy contradicts Early Access model.**
Target: "refuse-load pre-1.0" (M8) vs. "each ring = one shippable Early Access update… Early Access starts at the kernel" (Game ladder).
Refusing old saves while selling EA updates means paying players lose long-lived colonies on every ring — the single most reliable review-bomb trigger in the genre. The plan already owns the machinery (golden fixture saves, TDD-per-migration) but the policy negates it.
Amendment: *"Refuse-load applies only pre-EA; from the first paid build, every schema change ships a forward migration verified against the golden-fixture suite, and any genuinely breaking update ships via an opt-in Steam beta branch with the prior version kept as a legacy branch."*

**2. BLOCKER — Static navmesh contradicts M8 construction gameplay.**
Target: "Recast/Detour (offline bake for slice; runtime tiled navmesh at Game-ladder R3)" vs. M8 "task assignment (gather/**build**/craft/guard)".
If players place structures and NPCs build them, walkability changes at runtime; an offline-baked navmesh cannot serve a colony sim's core verb. Discovering this mid-M8 costs months.
Amendment: *"Pull runtime navmesh mutation forward into M7 via Detour dtTileCache temp obstacles (not full R3 tiled streaming), OR write into the M8 spec that slice construction snaps to pre-designated plots whose footprints are baked into the navmesh."*

**3. MAJOR — No milestone owns construction/building placement.**
Target: roadmap M7/M8 — "build" appears only as a task verb. Ghost placement, validity checks, blueprint entities, material-delivery requirements, NPC build-over-time jobs, and structure damage states are a multi-week system unowned by any milestone.
Amendment: *"Add to M7 exit criteria: building placement v1 — grid/ghost placement with validity feedback, blueprint entity, materials-delivered precondition, NPC construct-over-time job — and raise the M7 estimate accordingly."*

**4. MAJOR — Stockpiles and hauling logistics are absent from the entire plan.**
Target: M8 "one production chain deep enough to feel (ore→smelt→smith→equip)". Items must move between workstations; that means stockpile zones, hauling jobs, item reservation, and priority interaction with the M7 job system — the classically underestimated colony-sim system (job thrash, deadlocks).
Amendment: *"Name 'stockpile zones + hauling jobs with item reservation and priority integration' as an explicit M7/M8 line item with its own code-week estimate, and add a headless hauling-throughput soak to the determinism harness."*

**5. MAJOR — Inventory/equipment and crafting recipes are unowned.**
Target: concept promises Named NPCs "individual inventory/equipment/skills"; the chain ends at "equip"; M7 ships melee combat (which needs weapons/equipment); no milestone builds the item/container/equipment component set, recipe/workstation data model, or their UI.
Amendment: *"Add to M7 alongside melee combat: item/inventory/equipment component set (player + Named NPC), recipe/workstation JSON data model, and container/equip UI."*

**6. MAJOR — M8's estimate does not survive its real contents.**
Target: "M8 … 4–6 mo". As written M8 contains the full slice content PLUS save/load, accessibility, string tables, crash reporting, Steam page, playtest ops — and findings 3–5 land there by default. At part-time pace this is 9–12 months, and the "code weeks vs content weeks" split can't fix an omitted-systems problem.
Amendment: *"Split M8 into M8a 'colony systems complete' (construction, hauling, inventory, chain, shop, dungeon) and M8b 'productization' (save/load, accessibility, crash reporting, Steam page, playtest), each separately estimated; re-baseline the total to 4–6 years or invoke the cut line now."*

**7. MAJOR — No EA launch gate exists.**
Target: the ladder implies EA begins after M9, but no milestone *is* the launch and no criteria gate it (7–10k wishlists is stated as an algorithm fact, not a gate).
Amendment: *"Insert milestone 'M9.5 — EA Launch' with explicit gates: ≥7k wishlists or a written conscious-go decision; ≥99% crash-free session rate over a final 2-week playtest (Sentry); ≥10 testers averaging 5+ hours with colonies surviving save/reload across two builds; FTUE complete; EA FAQ + pricing drafted."*

**8. MAJOR — No EA update-cadence, hotfix, or patch-testing policy.**
Target: gap — the plan defines pre-launch CI rigor but nothing about operating a live game whose players hold multi-hundred-hour saves.
Amendment: *"Adopt a written EA ops policy: rings soak on an opt-in beta branch ≥2 weeks before default; every default-branch push passes golden replays + the fixture-save migration suite + a 1-hour co-op soak; hotfixes tag from the release branch same-day with the migration suite mandatory; target one substantive update per 6–10 weeks plus a monthly development post."*

**9. MAJOR — No FTUE/tutorial work anywhere in the plan.**
Target: gap across all milestones. A survival-colony hybrid with permadeath, a Director, and a 2-hour Steam refund window cannot ship on genre literacy alone; first-hour drop-off is the genre's primary killer.
Amendment: *"Add to M8b/M9: FTUE v1 — a guided-objective opening scenario built on the existing quest/Luau data machinery plus a contextual hint system; exit criterion: 5 cold-start testers reach the first production chain and survive the first raid unaided; re-verified at the EA gate."*

**10. MAJOR — UI plan leaves EA launching on unstyled programmer UI.**
Target: "Dear ImGui for slice menus/HUD … Real UI stack post-slice." A management-heavy game is mostly UI (priority tables, trade, build menus, colonist lists), M9 Deck verification requires controller-navigable UI, and "post-slice" is unscheduled. ImGui itself is defensible (RimWorld shipped on IMGUI) — the missing item is a production pass and a pinned decision date.
Amendment: *"Add a 'UI production pass' to the EA gate — custom ImGui theme (fonts, panel styling, iconography), full controller/Steam Input navigation validated on Deck — and pin the real-UI-stack go/no-go decision to R1 rather than an unscheduled 'post-slice'."*

**11. MINOR — Music is entirely absent; no mix pass.**
Target: content strategy covers SFX (Sonniss) and the cost table has no music line.
Amendment: *"Add to content strategy and the cost table: licensed or commissioned music (~£300–800, 6–10 tracks) and a one-week audio mix pass (bus structure, combat/UI ducking, volume sliders, master limiter) as an EA-gate item."*

**12. MINOR — EA localization scope undecided despite `tr()` from day one.**
Target: "`tr(\"key\")` string tables from first UI text" with no launch-language decision; zh-CN and de are disproportionate colony-sim markets.
Amendment: *"Add open decision (decide by M9): EA launch languages — either state English-only on the Steam page explicitly, or budget ~£1.5–3k for zh-CN/de/fr at EA."*

**13. MINOR — Quality bars are scattered, not consolidated as release gates.**
Target: >20 ms frame defect rule (M4) and 1-hour soak (M8) exist, but no load-time target, no crash-free-rate metric despite Sentry, and no crash-during-save test despite temp-rename backups.
Amendment: *"Write RELEASE-GATES.md at M8-start: p95 frame ≤16.6 ms on Deck at slice-scale colony; load <30 s; ≥99% crash-free sessions from Sentry; zero corrupted saves across a 100-run kill-process-during-save fuzz test; all gating M9.5."*

**14. MINOR — Needs UI, colonist alerts, minimap, and settings pages are unowned.**
Target: hunger "for players AND NPCs" and "menus/rebind UI" imply, but never name, the management HUD.
Amendment: *"Add named M8 line item: management HUD v1 — needs bars, colonist list with alerts (starving/idle/injured), map overlay or minimap from the tactical camera, audio/video settings page."*

**15. MINOR — Steam Cloud decision missing.**
Target: M9 Steamworks integration; long-lived saves plus rolling backups need a cloud/quota decision, and silent local-only save loss is a review risk.
Amendment: *"Add to M9: enable Steam Cloud sized to the rolling-backup save set, or explicitly document and communicate local-only saves."*

## Verdict

This plan is genuinely excellent at what three adversarial passes made it excellent at — engineering architecture, testing discipline, solo-dev sustainability, and cost honesty — but its residual risk has pooled in exactly the place prior passes didn't look: the game layer between M7 and EA launch. The roadmap quietly assumes a complete colony-sim item economy (construction, hauling, inventory, recipes, management HUD) that no milestone owns, sits on a navmesh decision that contradicts its own core verb, and carries an M8 estimate that cannot absorb the difference; meanwhile the EA phase — the actual commercial product — has no launch gate, no live-ops policy, no first-hour experience, and a save policy that would wipe paying customers' colonies. None of this is fatal: every gap has cheap machinery already in the plan (the job system, quest/Luau data, fixture-save migrations, Sentry) waiting to be pointed at it. Apply findings 1–2 and 6–8 before scaffolding begins and re-baseline the timeline; the rest can be pasted in as milestone line items. With those amendments this plan is production-credible; without them it produces an impressive M8 demo and an EA launch that bleeds reviews in its first month.