# Internal Consistency Review — Final Pass

## Findings

**1. BLOCKER — The slice's core verb, "build," has no owning milestone.**
M8 requires "task assignment (gather/**build**/craft/guard) via tactical view" and a production chain, and the pillar text promises "building production chains" — but no milestone builds a construction system (blueprint/building placement, build sites, material delivery). M7 builds the "task/job system (work queues, priorities, auto-assignment by skill)" — the queue, not the thing queued. Nav-mesh, BT, tactical camera are all owned; placement/construction is not, and it is not trivial (snapping, validity checks, staged build states, hauling).
*Amendment:* "M7 additionally delivers construction v1: catalog-driven blueprint placement (grid snap, validity check), build-site entities that consume hauled materials via the job system; M7 estimate rises to 3.5–5 mo."

**2. MAJOR — Save policy contradicts the testing lane.**
M8: "save/load versioned header … **refuse-load pre-1.0**." Testing table: "Save/load | **TDD per migration** + golden fixture saves per schema version (old saves kept in-repo forever)." If everything pre-1.0 refuses old saves, there are no migrations to TDD until 1.0 — yet 1.0 is "the ladder complete," meaning Early Access players (from M8/M9 per "Early Access starts at the kernel") lose saves on every ring for years.
*Amendment:* "Replace 'refuse-load pre-1.0' with 'refuse-load pre-Early-Access; from the first paid EA build, save migrations are mandatory and follow the Save/load testing lane.'"

**3. MAJOR — The plan's identity sentence still says "action game."**
Title: "Plan: Moddable Co-op 3D **Action Game** → Engine Product." Locked decision: *"We are building a moddable co-op **action game**; the engine is the long-term product…"* This is the sentence the plan itself designates as strategy; a future session using it as the descope razor will razor toward the wrong genre. Also stale: Context line "co-op 3D action game" framing and locked-decision bullet "first slice = third-person co-op" (accurate only via its appended concretization clause).
*Amendment:* "Retitle to 'Moddable Co-op Colony-Sim → Engine Product' and amend the locked quote to 'We are building a moddable co-op embodied colony-sim; the engine is the long-term product, earned through the game.'"

**4. MAJOR — Milestone estimates no longer sum to the stated total.**
M0–M9 sum to ~23–37 months (≈2–3.1 years); the plan states "**Total honest estimate: 3.5–5.5 years** part-time to the sliced game on Steam." Even the high end of the sum misses the low end of the total by ~6 months. A future session cannot tell which number is authoritative, and the mismatch suggests per-milestone estimates were not re-based after the colony-sim scope landed (M7 in particular absorbed task/jobs + tactical camera + identity tiers at an unchanged 3–4 mo).
*Amendment:* "Add after the total: 'Milestone estimates sum to ~24–37 months; the 3.5–5.5-year total additionally budgets the upfront handbook push and a 1.4× solo-overrun multiplier — the total is the commitment, the sum is the floor.'"

**5. MAJOR — "Zero new architecture" dungeon claim is false at M8.**
Concept: "dungeons — which are hostile settlement tokens **in the strategic layer (zero new architecture)**." But the strategic layer ships at R2 ("strategic layer v1 … full Director with dungeon placement"), while M8 ships "one escalating dungeon (Director-lite…)." At M8 the dungeon must be bespoke because the substrate it claims to reuse doesn't exist. Same problem for M8's "one rival faction camp (**abstract until raided**)" — abstract-token hydration is R2/R3 machinery.
*Amendment:* "At M8, Director-lite runs on a standalone per-dungeon ledger and the rival camp is a hand-authored dormant/active site — 'zero new architecture' and abstract-until-hydrated apply only from R2 onward, when both refactor onto strategic-layer tokens."

**6. MAJOR — Promised NPC scale vs. replication: no milestone builds interest management.**
Concept promises "Crew (~50–150/settlement)" and "Ambient (hundreds, cities only)." M3 ships "server-authoritative **full-state** replication"; M5 adds snapshot interpolation. Nothing in the roadmap or Game ladder ever adds relevancy/interest management or delta compression — the systems that make hundreds of replicated NPCs viable. The identity-tier promise silently depends on unscheduled netcode work.
*Amendment:* "R3 gains an exit criterion: replication relevancy (per-client region + radius filtering) so Crew/Ambient counts scale; until R3, NPC counts are capped at what full-snapshot replication sustains under the loss simulator."

**7. MAJOR — Colony-sim UI scope vs. the unchanged UI decision.**
Stack table: "Dear ImGui for slice **menus/HUD** … Real UI stack post-slice. Scriptable UI is a quarter-scale project." But M8 now needs a colony management surface: task priorities, NPC inspection, stockpile/production views, trade/trust window, difficulty presets, squad orders. That is far more than "menus/HUD," and the pivot never re-priced it.
*Amendment:* "M8's code/content split explicitly budgets N weeks of ImGui colony-management UI (priority table, NPC panel, stockpile/production views, trade window — utilitarian, RimWorld-style); the 'real UI stack' decision moves to the R1 exit."

**8. MAJOR — Deck target vs. tactical/colony UI on controller.**
M4: "gamepad path enters"; M9: "Deck verification"; money table buys a Deck at M7/M8. But M7's "tactical camera + squad orders" and M8's colony UI never mention controller operability — for a colony-sim, gamepad character movement is the easy part and the management UI is the Deck risk.
*Amendment:* "If the Deck target stands at the M7/M8 hardware buy, 'tactical camera + task UI operable on gamepad' joins the M8 exit criteria."

**9. MINOR — Stale mod name in repo layout.**
"`mods/ base-action/ …`" is an action-slice leftover.
*Amendment:* "Rename to `mods/base-colony/`."

**10. MINOR — M6 acceptance example is action-genre residue.**
"one feature entirely as a mod, clean of dynamics (**dash / healing shrine**)" — while M7's demo correctly uses "mod adds a new job type."
*Amendment:* "M6 acceptance example becomes 'a new crop + recipe chain, or a consumable shrine buff' — sim-authoritative, prediction-free, colony-flavored."

**11. MINOR — "3-game feel-reference library" vs. the 7-game genre library.**
M4: "feel check clip vs a named **3-game** feel-reference library"; the concept section's "Genre + **feel-reference library**" names seven games. Same term, two lists, and the three are never named.
*Amendment:* "M4's character-feel references are Bellwright, Medieval Dynasty, V Rising (embodied-controller feel), distinct from the 7-game genre library."

**12. MINOR — "First stranger playtest at M5/M6" predates any colony gameplay.**
At M5/M6 the build is a character walking in a netcoded grey-box — stranger-testable for an action slice, not for this game. First stranger-worthy build is M7's "public demo checkpoint."
*Amendment:* "First stranger playtest moves to M7; M5/M6 sessions stay friends-only netcode validation."

**13. MINOR — Project-killer list never re-based for the pivot.**
K1–K5 (renderer, animation, prediction, controller feel, astronautics) are the action-game/engine killers; the pivot's largest new risks — task-system/AI scope creep and strategic-layer creep — have no K entry and no pre-authorized fallback.
*Amendment:* "Add K6: colony-systems creep → the M8 kernel list (tasks, one chain, one shop, one dungeon, one camp) IS the slice; any additional system is a Game-ladder ring by default."

**14. MINOR — XP has no owning system.**
"monsters/demons/zombies attack ALL factions and grant **XP** + unique resources per enemy family" — no milestone or ring builds a progression/XP system; the only stat machinery named is NPC job skills.
*Amendment:* "'XP' means increments to existing NPC/player skill stats (no separate progression system pre-R1); enemy-family XP tables ship with M8 combat or are cut from the slice."

**15. MINOR — Succession is double-booked.**
Concept lists "succession when an overseer dies" as a core Crew→Named promotion trigger; R5 lists "succession" as future content ("living history … succession, empire rise/fall"); M7 ships "one promotion event," unnamed. A future session won't know which trigger to build or when.
*Amendment:* "M7's single promotion trigger is 'survives a raid'; overseer succession is R5."

**16. MINOR — Governing rule 5 ratchet is impossible before the mod API exists.**
"each milestone moves ≥1 shipped feature onto the public mod API" — the API arrives at M6; M0–M5 cannot comply.
*Amendment:* "Rule 5 applies from M6 onward."

## Verdict

The plan's architecture, engineering practices, and risk machinery are in genuinely good shape after three passes — but the July-2026 genre pivot was applied unevenly: the concept section was rewritten thoroughly while the roadmap, identity sentences, and supporting lists were only patched. The result is one true execution hole (construction has no owning milestone despite being the colony kernel's central verb), two load-bearing contradictions a future session would act on incorrectly (refuse-load-pre-1.0 vs. the migration testing lane; "zero new architecture" dungeons resting on an R2 system at M8), one arithmetic drift (milestones sum to ~2–3 years against a stated 3.5–5.5), and a scattering of action-game residue (title, locked quote, `base-action`, dash/shrine, stranger-playtest timing, killer list). None of these require structural rework — every fix above is a paste-in amendment — but findings 1–4 should be applied before any execution session, because each sits at a point where the plan itself instructs future sessions to consult the offending text.