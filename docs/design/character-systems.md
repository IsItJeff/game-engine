# Character, Progression, Equipment, Survival & Social Systems — Design Spec

> Companion to `master-plan.md`. The long-horizon design for the colony-sim's character layer, built incrementally in phases P0–P8 (see the Implementation plan). Every number is a playtest-tuned data knob, not a hardcoded value.

## Context

A deep, coherent character-development + living-world design for the co-op colony-sim, **planned before building**. It generalizes the shipped `advance_progression` pipeline (engine/sim), fills the master plan's OPEN slots (morality/personality/relationships), and schedules each piece onto the Game-ladder rings (K6-respecting). Becomes **`docs/design/character-systems.md`** when executed — a companion to `docs/design/master-plan.md`, not a replacement.

**Guardrails (non-negotiable):** entity-is-an-ID → **components, never class hierarchies**; all mutation via the **command funnel**; AI via **behavior trees** (C++ nodes, Luau leaves on a C++ blackboard); **deterministic** fixed-60Hz (seeded RNG, replay in CI); **Luau-only** mods + JSON `extends`; hot loops dispatch-free (social/morality run staggered at 5–10 Hz).

---

## The one law: uncapped, ever-slower, never zero

Every number obeys this. **Cost rises forever** (`xp_to_next = 100·Lᵖ`, p=1 ships) so specializing never caps, only ever-slows. **Effect grows ever-slower but never stops**, via a diminishing curve **`POWER(L) = 1 + 0.35·ln(1+L/10)`** baked to a fixed-point LUT (L5≈1.14, L50≈1.63, L100≈1.84). Result: growth is unlimited, a master is clearly best (~2×, not 10×), nothing is unreachable for the determined.

**Determinism = FIXED-POINT EVERYWHERE (Q16.16), from line one** — bit-exact on every OS, a hard lockstep/replay gate. Converting existing floats shifts numbers within precision, so P0 is "equivalent within precision", not literally bit-identical.

---

## Vocabulary

| Term | Is | Grows by |
|---|---|---|
| **Attribute** | one of the **7** — a core stat, a skill-domain, AND a learning-proficiency, all in one. Has its OWN level+XP. | the skills that use it (main a lot, contributors a little) |
| **Resource** | a spendable bar (HP/Stamina/MP), governed by **VIT** | its two VIT-skills (capacity + regen), by use |
| **Skill** | a levelable node with a **main Attribute** + flexible **contributions**; branches parent→child | learned/discovered/earned, then used |
| **Character Level** | a global multiplier on everything | *general EXP* — a fraction of all activity |
| **Need** | a draining survival bar (Food/Water/Fatigue) | drains; refilled by eat/drink/rest |
| **Affinity** | soft per-attribute talent modifier | innate + personality-shaped |
| **Title / Standing** | derived recognition / moral-fame values | from build + the behaviour ledger |

---

## The 7 Attributes

Each is at once a **stat**, a **skill-domain**, and the **proficiency** that speeds its skills. **Each has its own level + XP**; a skill grants its **main** Attribute a lot of XP and each **contributor** a little.

| Attribute | as a stat | domain (main-attribute of these skills) |
|---|---|---|
| **STR** Strength | physical attack, carry | Melee · Mining · Woodcutting · Smithing · Construction · Hauling · Grappling |
| **DEX** Dexterity | move/attack speed, aim, reflex | Archery · Throwing · Accuracy · Evasion · Athletics · Acrobatics · Stealth · Sleight-of-hand · Tailoring |
| **INT** Intellect | magic attack, tech, learning | Spellcasting · Enchanting · Alchemy · Engineering · Electronics · Research · Cooking |
| **WIS** Wisdom | magic defence, awareness, nature | Healing→Medicine · Foraging · Herbalism · Farming · Fishing · Beast-taming · Tracking · Awareness |
| **VIT** Vitality | physical defence, hardiness; **governs HP/Stamina/MP** | Toughness+Recovery(HP) · Conditioning+Second-Wind(Stamina) · Focus+Attunement(MP) · Guarding · Survivalist · Resistance |
| **CHA** Charisma | social force, presence, morale | Trade · Leadership · Teaching · Diplomacy · Performance · Intimidation |
| **LCK** Luck | fortune — crit, rare finds, discovery, quality | Scavenging · Gambling · Treasure-sense *(+ contributes crit/rare/discovery everywhere)* |

*(Old "Specializations" are dissolved into these; "Perception" the stat split into the WIS **Awareness** skill + the DEX **Accuracy** skill.)*

---

## Skills — main + contributions, trees, tiers

**Each skill declares a main Attribute + any number of contributing Attributes, each driving a different *aspect* of the outcome.** Example — **Mining**: main **STR→Yield**, **DEX→Speed**, **LCK→Crit** (bonus haul), **VIT→Cost** (stamina efficiency). `SkillDef = { main_attr, contributions:[{attr, aspect, weight}] }` — data-driven; mods add rows.

**Aspect enum (starter, expandable):** `Yield · Speed · Crit · Quality · Cost · Capacity · Regen · Defence · Range`.

**Doing a skill:** raises the **Skill** level; grants **main-Attribute XP (big)** + **contributor-Attribute XP (small)**; a fraction goes to **Character Level**. The main Attribute's level **speeds learning of every other skill it homes** (compounding domain-transfer — a master-STR miner picks up Smithing faster), checked by the ever-harder law.

**Trees (parent→child):** a child needs its parent (Accuracy→Throwing, Healing→Medicine, Evasion→Dodge); child XP flows up. Method-variants attach as gated siblings and **swap the main Attribute**: Physical Mining→STR, Magic Mining→INT, Tech Mining→INT.

**Tiers F→S:** F = anyone learns easily (no gate); A/B = long, gated by Attribute/Skill level; **S = world-unique legendary** — earned-only, never taught, **locks to its discoverer**, returns to the world only if relinquished or the holder dies (hunt/guard/kill for a technique). Any tier can rarely roll at spawn.

**Acquisition:** explicit (taught/read/built) **and** chance-discovery from related activity (odds raised by Affinity/INT). **Mentorship:** a higher-skill character teaches a lower one (grant first level / boost XP); tier gates teaching (F easy … S impossible).

---

## The action formula (multi-aspect, one resolver)

```
per aspect a (Yield, Speed, Crit, Quality, Defence…):
  value_a = base_a · POWER(skill.level) · (1 + contrib_attr_a.level · k_a)
                   · POWER(char_level) · equip_mult
    Yield←STR   Speed/interval←DEX   Crit←LCK   Quality←INT/WIS   defence←VIT/WIS
outputs → whole numbers via FRACTIONAL CARRY (floor now, carry the remainder, +1 when it crosses 1.0).
XP: skill += xp_base ; main_attr += big ; contributors += small ; char_level += xp_base·k.
```
`POWER(char_level)` is the automatic global multiplier (replaces manual allocation — **no point-spending**; a `creation_offset` only sets your start). **Combat = the same resolver** with an added hit-vs-Evasion contest + damage-vs-defence (physical STR-vs-VIT, magical INT-vs-WIS); ratio mitigation `dmg = max(raw·raw/(raw+def), 0.10·raw)` softens forever but never negates, 10% chip floor. Combat HP≤0 routes through the existing `handle_deaths`: **player → Downed (ally-rescuable / expiry respawns / hardcore permadeath); NPC → permadeath.** Attacks spend the method resource; exhaustion reuses the shipped `stamina==0` crawl.

---

## Growth identity, affinity, the three trees

**You are what you do.** All growth is from activity; **choice is only a creation offset**. Effort compounds, talent multiplies: XP only comes from doing, so a gifted idler is overtaken by a grinder. **Affinity** = a soft per-Attribute modifier (learn-rate, resource efficiency, head-start), personality-shaped, **never a lock**.

**Three trees, three fuels, distinct entry:**
- **Physical / Stamina** — *do it* (no gate; universal).
- **Magic / MP (innate)** — *learn it* (a trickle of MP exists but is inert until taught/read → grants Skill lvl 1; MP + its Focus/Attunement skills hidden until then).
- **Tech / ENERGY (gear-based)** — *build it* (a tech item carries a **battery**; abilities draw its Energy). Charging IS tech + skills: a **dynamo** (spend Stamina), a **mana-converter** (spend MP), a learned **auto-charge** (MP trickle). So magic is innate, tech is equipped — no 4th character bar.

Magic/tech method-variants are gated by the **Scholarship-style knowledge** now living in **INT** (Research/Reading). Twin-trunk magic↔tech divergence deepens at R4; combined-resource abilities (spellblade = Stamina+MP) are the cross-tree payoff.

---

## Equipment (traits with real downsides)

Items are **world entities**; `Equipment{slots[Tool,Weapon,Armour,Gear]}` on the wearer; each `Item{def, quality, durability, mods, traits[]}`; equipping folds `mods` into a cached `EquipMods` on change (not per-tick); equip/unequip are **Commands**. **Gear can grant raw +Attribute AND +skill/+aspect** (e.g. "+2 STR", "+3 Yield on mining", "+Speed"). **Every item has positive AND negative traits** (CI-linted: nothing rolls pure-upside); **mastery shrinks banes (~50%) + adds prestige but never removes them** — the tradeoff pillar survives to endgame. Found or crafted (crafter skill + materials roll quality/traits). Tech gear additionally carries the **Energy battery**. Durability now; wear/repair/reforge at R1.

---

## Survival — Needs (Food / Water / Fatigue)

Draining `Need{current,max,drain}`; drain **faster when exerting**, slowest resting; **Fatigue** recovers idle(slow)/sit(med)/sleep(fast). Empty → escalating inefficiency debuff → **Downed → death only if no ally rescues you** (regardless of level — the great equalizer; hardcore = outright). **Growth lengthens but never removes the timer**, sourced from **gear + the Survivalist skill** (Endurance is now pure combat defence and does NOT buffer needs). Exertion-drains-needs = **no 24/7 grind**; forces the food/water production chains. NPCs run the identical system → feeding everyone is the economy. Difficulty presets scale severity.

---

## Personality, Morality, Titles, Social

**Personality: 6 fixed `int8` axes** (bravery, compassion, industry, loyalty, greed, sociability) + a small bounded **drift** knob ("the war changed him"). Read by BTs as thresholds/weights. NPCs roll an **archetype** (Stalwart, Schemer, Zealot, Drudge, Rogue, Kindler, Loner, Firebrand) + jitter; players start neutral, drift from deeds.

**Morality: a multi-dimension behaviour ledger → one derived `standing` scalar.** Dimensions: Violence, Honesty, Loyalty, Charity, **Cruelty** (villain signal), **Valor** (hero signal). `standing = charity·.8 + valor·1.0 + honesty·.6 + loyalty·.6 − cruelty·1.2 − violence_unjust·.8` (violence counts only vs standing≥0 victims — killing bandits barely dents you). One `record_deed(actor,kind,mag)` write-point; deed weights JSON; leaky ~44-day decay (redemption/corruption free).

**Titles: derived recognition, no stat bonuses, uncapped/stackable** — from build + gear + deeds ("Master Smith", "the Butcher", "Dragonslayer", "the Coward"). Roles + hero/villain are the same: derived queries, never stored slots (HERO = standing>+500 ∧ high Valor ∧ famous; VILLAIN = the mirror). They change how the world *reacts*, never your numbers.

**Social layer:** `perceive(A,B)` from A's personality + might + B's *believed* standing → `{threat, affinity, safety}` → a BT stance (BEFRIEND / PROTECT / FEAR / EXPLOIT / NEUTRAL) — the hero-defends-the-weak and villain-robs-the-weak paths both fall out. **Players are symmetric subjects** (NPCs fear/rally-to/mistrust a player by their own might+standing+renown). **Relationships:** directed sparse `{trust, affinity, bond}`, small event-deltas, seeded by personality-match; bonds Acquaintance→Friend→**Partner** / Rival→**Nemesis** (latch, resist decay). **Emergent heroes/villains, no cap** via uncapped renown/notoriety; **Aspirations** (hopes/dreams) steer behaviour and (R5) spawn NPC-founded towns. **Teaming + mentorship** reuse the job/squad structure.

---

## Scope — who carries the model

The **full character model** (7 Attributes, skills, resources, needs, personality, ledger, relationships) runs on **Named AND Crew** NPCs; **Ambient** get none (pure crowd). *Cost to watch:* this is heavier than the master plan's "Crew = a few stats" — ~50–150 Crew × full model per settlement raises the per-tick budget, so it leans harder on staggered 5–10 Hz thinking and likely pulls the **R3 replication/scale work** a little sooner. Promotion between tiers = adding/removing components, as the master plan specifies. Build order is confirmed **P0→P8 (foundation-first)**.

## Ring schedule (K6-honest)

| Ring | Character / progression | Social / personality |
|---|---|---|
| **Slice (M7/M8a)** | STR/DEX/VIT skills + the multi-aspect resolver; physical only; equipment component set + traits; Food need; auto-assign by skill level. | Personality on Named; BTs read bravery/industry/greed; trust scalar; one shop; need-quests. |
| **R1** | Full 7 Attributes with own XP; INT/WIS/CHA/LCK skills; A-tier gating; affinity; mentorship; Water+Fatigue; gear wear/repair; the fixed-point-migration completed. | BehaviorLedger + standing + role labels + drift; relationship affinity. |
| **R2** | economy/era depth | renown/notoriety; hero/villain labels; Aspirations. |
| **R4** | Magic (MP) + Tech (gear Energy) trees; active resource-gated Ability Commands; combined-resource skills; S-tier lifecycle; divergent perks. | full perception (safe/scary/protect/prey); reputation + gossip; bond latching; players symmetric. |
| **R5** | skills/gear persist through exodus | bonds→succession/exodus/feuds; NPC-founded settlements; legacy; love/hopes/dreams depth. |

---

## Implementation plan (step-by-step, TDD)

**SOLID-in-an-ECS:** each system is one single-responsibility free function; new content = JSON rows (Open/Closed); systems read data-driven defs, all mutation via the funnel (Dependency-Inversion at the seams); OOP for the loaders / `fixed` type / resolver (cohesive units), **never entity subclasses**; TDD test-first on the headless deterministic sim; a dev doc page per subsystem; heavy house-style comments. **Defs hardcoded first**, JSON at the modding milestone.

**Directory:** `engine/sim/progression/{curve, attributes, skills, progression, defs}`, `combat/`, `equipment/`, `survival/`, `social/`, `data/`; `engine/core/fixed`; `tests/sim/*` mirrors; `docs/engine/character/` one page per subsystem.

**Phases** (each = one branch, test-first → dev+ci green → doc → run → PR):
- **P0 · Fixed-point + Curve + foundation** — `fixed` Q16.16 type (exhaustive tests) as the numeric bedrock; migrate existing Vitals/XP to it; `POWER` LUT + `xp_to_next` (property tests: monotonic, uncapped, ever-slower); turn `Skills{conditioning}` into a keyed `SkillId→Skill` collection; give **Attributes their own {level, xp}** fed by a skill's main/contributor split (a single feeder reproduces today's behaviour). *Equivalent-within-precision* (float→fixed), assert with tolerance.
- **P1 · Attributes, Skills & Resources** — the 7 Attributes (own XP), Resources under VIT with the 6 capacity/regen skills grown by use, Character-Level global multiplier, fractional-carry outputs. Endurance→VIT flips to pure defence.
- **P2 · Skills: main+contributions, trees, aspects** — `SkillDef` with contributions/aspects, parent→child trees, XP flow-up, method-variants, the aspect enum.
- **P3 · Actions (method-drives-growth), one resolver** — `PerformAction` command resolving an `ActionDef` (multi-aspect), granting the XP split; tool declares method/main-attr/resource.
- **P4 · Combat = a contested action** — the hit/damage/defence contest atop the P3 resolver; Downed-rescue via `handle_deaths`; procs/buffs as data.
- **P5 · Equipment** — item entities, `EquipMods` (raw +Attribute + skill/aspect), +/- traits (CI-lint), equip Commands; tech-Energy battery stub.
- **P6 · Survival needs** — Food/Water/Fatigue, exertion-drain, Downed-rescue, gear+Survivalist timer.
- **P7 · Personality & morality** — 6 axes + drift, BehaviorLedger + record_deed + standing + decay, archetypes.
- **P8 · Social layer** — perceive, relationships (latching bonds), derived Titles/hero-villain; players symmetric.

**Testing:** property tests for `curve` + `fixed`; determinism (single seeded stream, fixed-point, defined rounding — bit-exact in the record/replay harness); leave-behind asserts on refactors; player==NPC parity; combat/survival reuse `handle_deaths` + the stamina gate (existing tests stay green).

---

## Open threads (non-blocking; resolve at their ring)
- XP split ratio main-vs-contributors, and general-EXP fraction `k` — data knobs, tune at playtest.
- Personality → affinity mapping; the auto-charge/dynamo tech-charging specifics.
- Solo player-death penalty (baseline: Downed→respawn when no ally).
- BehaviorLedger dimension count / directed-vs-symmetric relations — lock schemas before R1 content authoring.
- S-tier acquisition curve + anti-griefing; temperature splitting out of Fatigue at R1.

## Verification
- Determinism: every stat/skill/deed/RNG path replays bit-identical (fixed-point + seeded); CI record/replay gates it.
- P0 leave-behind: equivalent-within-precision assert; each later phase ships one system, player==NPC, dev(ASan/UBSan)+ci(-Werror) green, sim tests, a doc page, a run-the-game check, an independent adversarial review before PR.
- Balance is tuned at playtest via data knobs (p, k, drain rates, affinity spread, deed weights, aspect coefficients), never hardcoded.
