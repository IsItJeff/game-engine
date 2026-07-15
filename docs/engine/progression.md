# Progression: skills feed attributes

## What it is

Characters grow by *doing*. A **skill** improves with the activity that trains it,
skills roll up into broad **attributes**, and attributes shape what you feel in play.
Eighteen strands are wired end to end so far, across all seven attributes:

- staying active trains **Conditioning**, **surviving damage** trains **Toughness**,
  **resting to recover** spent stamina trains **Recovery** (`update_stamina` — and, like Survivalist,
  its own *level* then quickens that second wind DIRECTLY, on top of the Endurance the resting also
  feeds it: a skill matters both through its attribute and by itself), **turning a blow with a raised
  guard** trains **Guarding** (`resolve_creature_contacts`, Toughness's *active* twin — grow
  Endurance by *blocking* a hit rather than *surviving* it), and **enduring venom** trains
  **Resistance** (`tick_poison`, Toughness's *poison* twin — keep shrugging off venom and you grow
  the very VIT that shaves it, immunity through exposure), and **pushing into exhaustion** trains
  **Survivalist** (drive your fatigue to the edge and the *skill* itself then *slows* how fast
  **every** need drains — fatigue, hunger, AND water, via the shared `survivalist_relief`, so hard
  living lets you last longer in the field on all fronts; it's the Survivalist *skill level* that
  buffers, not Endurance — VIT stays pure combat, never a survival-need buffer), and **casting a
  spell** trains **Attunement** (`regenerate_vitals` — the mana twin of Recovery: its *level* speeds
  mana regen DIRECTLY on top of feeding Endurance, so a caster **deepens and quickens** its mana bar
  by casting, the design's MP resource-skill) — all seven raise
  **Endurance**, which grows your **max health, stamina, and mana** (the design's "VIT governs
  HP/Stamina/MP" — all three pools off the one attribute, so a hardy caster carries a bigger reserve
  too), speeds how fast stamina comes back, softens the venom you take, eases the **stamina cost** of
  every swing and throw (a hardy body spends less per action, so it sustains a longer fight — the
  design's **Cost** action-aspect, `eased_cost`, up to a half floor), *and* lets you **bear armour**
  better (a hardy body shrugs off part of plate's stamina-recovery bane, `borne_regen_penalty` — the
  armour twin of Strength's weapon carry);
- **attacking** trains **Striking**, which mainly raises **Strength** (plus a little
  **Dexterity** — footwork), and Strength lengthens your
  **attack reach**, your **attack damage**, *and* how much of a wielded weapon's **heft** you
  shrug off (the design's **carry**) — against the hostile **creatures** (red
  dots with HP that hunt you), a higher Strength kills faster, while your Endurance
  (VIT) softens the blows they land. Damage is `Strength`-vs-`VIT` ratio mitigation. The **carry**
  effect is the design's *mastery shrinks a bane but never removes it* rule applied to equipment: a
  weapon slows you (`Equipped::move_penalty`), and each Strength level eases that heft
  (`carried_move_penalty`) up to a **half** floor — so a strong wielder moves nearer its unarmed pace
  but a weapon *always* costs some speed, on the player and NPCs alike;
- **facing a creature's swing** trains **Evasion**, and **sprinting** (a burst of speed) trains
  **Athletics** — both raise **Dexterity**, your
  chance to **dodge** a blow entirely (and creatures dodge yours, but your **aim** now cuts through
  their dodge — DEX is a hit-vs-Evasion contest both ways — see
  [combat](combat.md#slipping-the-blow-evasion-dexterity)). Athletics is Conditioning's *burst* twin:
  steady movement builds Endurance, a **sprint** builds the agility (DEX) that sharpens dodge, **melee
  aim** (accuracy), *and* throw-aim — so a kiter who dashes a lot becomes genuinely harder to hit AND
  connects more reliably (player-triggered, like Throwing, since only the player sprints). Agility also **wades a mire's mud faster**
  (`waded_mire_factor`) — the *movement twin* of Strength's weapon-carry and Endurance's armour-bear,
  the same `eased_bane` half-floor so mud always still slows;
- **collecting loot** trains **Scavenging**, which raises **Luck** — fortune with *three* effects:
  your chance to land a **critical hit** for doubled damage (see
  [combat](combat.md#lucky-strikes-crits-luck)), how much **health a found orb restores**
  (`collect_pickups` scales the orb's heal by `1 + (LCK − 1)·0.1`, capped ×2), **and** how slowly your
  **gear wears** — fortune *preserves* a blade or plate, shaving the per-hit `durability` loss
  (`durability_wear`, down to a half floor). The design's *richer finds / quality* on three fronts. So
  a lucky scavenger crits harder, mends more from the same loot, *and* keeps its gear longer, and the
  loot→Scavenging→Luck loop feeds all three;
- **landing a thrown hit** trains **Throwing**, which raises **Dexterity** (aim — plus a little
  **Strength** for hurl power), the ranged mirror of Striking (see [combat](combat.md));
- **grazing a food plot** trains **Foraging**, which raises **Wisdom**, the first *non-combat*
  attribute — each level lets you draw **more food per second** from a patch, sharpens **danger
  awareness** (a wider flee sense radius in `steer_npcs`, a distinct source from bravery's nerve),
  **and now wards MAGIC** — a hostile bolt is softened by the target's Wisdom, *not* its VIT/armour
  (`magic_defence_of`, the design's magical **INT-vs-WIS**, so magic pierces the plate that blunts a
  blade). So the design's WIS = *magic-defence + awareness + nature*: a seasoned forager gathers more,
  spots trouble sooner, and shrugs off a spell. It's the loot loop's survival twin (gather food →
  Foraging → Wisdom → forage faster *and* stay alert), and Wisdom doesn't grow the pools; its one
  combat role is that purely **defensive** ward, never offence;
- **public heroism with allies watching — felling a foe *or* hauling up a downed ally** — trains
  **Leadership**, which raises **Charisma**, the second *non-combat* attribute (the design's **social**
  stat), with **three** effects — depth, reach, *and* persistence. Each level deepens the
  **camaraderie** a witness feels toward you per heroic act (the shared
  `bond_witnesses` grant, at a kill *and* a rescue, see [relationships](relationships.md)), up to a ×2
  cap; it widens the **reach** — how far a deed is witnessed and admired (`kCamaraderieRadius`
  scales with Charisma, up to ×1.5), so a charismatic champion's heroism inspires **more onlookers**,
  each **more deeply**. Those two are about how OTHERS bond to a charismatic leader (both keyed on the
  *leader's* CHA, at `bond_witnesses`). The **third** acts the other way, on a character's OWN ties:
  a charismatic edge-owner's bonds cool **SLOWER** (`decay_bonds`' period lengthens with the owner's
  Charisma, up to ×2), so the social-glue stat doesn't just FORGE bonds but HOLDS them against the
  leak. So Charisma **compounds**: leading heroically forges ever-deeper bonds in an ever-wider ring
  of onlookers (be seen being heroic → Leadership → Charisma → allies bond harder *and* from farther),
  while a charismatic character's own ties **fade slower** — the social mirror of a striker building
  Strength by hitting. Like Wisdom it grows neither the pools nor a fighter build — it grows the
  colony's *bonds*.
- **casting a spell** (a `magic_bolt`, the **C** command) trains **Spellcasting**, which raises
  **Intellect** — the **seventh** attribute, completing the set, and the design's **magic** stat. Each
  level sharpens a bolt's damage, the arcane mirror of Strength on a swing. It also **eases a spell's
  mana cost** (`eased_mana_cost`, INT's *second* effect): a cleverer caster spends less per cast
  through the same `eased_bane` half-floor VIT uses for a swing's stamina cost — a spell still costs mana
  (never free), but a sharp mind casts more before the bar runs dry, the arcane twin of `eased_cost`
  (and distinct from Attunement, which grows/regens the *pool*, not the *spend*). Magic is *learned*:
  only a caster who carries the Spellcasting skill can cast at all (see [magic](magic.md)), so
  Intellect grows by casting the way Strength grows by striking — from a power you earned the right to use.
- **teaching a nearby novice** (`teach`) trains **Teaching**, the **second** feeder of **Charisma**
  beside Leadership — *leading by instruction*. This one is different in kind: it's the first strand a
  colonist grows from **another person**, not its own toil. A colonist far ahead in a skill passes it
  to a much-lower one standing near — the student gains XP in that skill (learning it if new), the
  mentor grows Teaching → Charisma. So a craft **spreads** through the colony beside its master, not
  only by each hand's own doing. It needs a real skill **gap** to fire (a mentor at level ≥ 3, a
  student well behind), which can't exist at spawn — so it emerges only after a colony has veterans.
  And **a skilled teacher teaches faster**: the mentor's own **Teaching level scales the lesson XP**
  (+10% per level past the first, capped at ×2) — the design's *"a skill's own level scales its own
  payoff"* (like Survivalist easing the drain it trains), so Teaching is no longer a pure XP-sink. A
  Teaching-1 (or untaught) mentor imparts the flat base rate, so a short-run colony is bit-identical.
- **mending a wounded ally** (a `heal_spell`, the **H** command) trains **Healing**, a **second**
  feeder of **Wisdom** beside Foraging — the design's WIS support domain. Each level mends more, the
  restorative mirror of casting sharpening a bolt. Like a bolt it's *learned* (it rides the same
  Spellcasting gate for now) and spends mana — but it reaches for a **friend**, not a foe, so it's the
  first strand grown by *helping* another rather than fighting or gathering.
- **preparing a meal** (harvesting a ripe crop, the **G** command) trains **Cooking**, a **second**
  feeder of **Intellect** beside Spellcasting — the design's INT *non-magic* domain. Each level
  stretches the same crop into **more food** (`harvest_nearest_crop` scales the meal by the cook's
  Cooking level), so a colonist who farms a lot becomes the colony's cook. Because the base meal sits
  *below* the hunger cap, that surplus actually **lands on a famished eater** — a starving colonist is
  filled more by a skilled cook's meal, while a nearly-full one tops off from either (a belly holds
  only so much; *carryable leftovers that feed more mouths are a follow-up*). Learn-by-doing needs no
  gate: anyone can harvest, and the first prepare learns it at level 1 (a plain ×1.0 meal, so a fresh
  world is unchanged) — the food-economy twin of a bolt growing a mage.

The player and NPCs run the identical machinery — progression *and* combat — so a
long-lived NPC that has moved, been hurt, and fought grows genuinely tougher and
stronger, no special-casing. The only difference is *how the swing is triggered*: the
player attacks on command (`J`), while NPCs attack through the `npc_attack` system
(strike the nearest threat in reach). Both run the same `perform_attack` resolver, so
both build Strength the same way.

## Why it matters

You asked for two things: the player should grow and *feel* stronger over time,
and NPCs should grow too. "Learn by doing" delivers both from one mechanism — the
activity *is* the training, so growth happens organically as the world is played,
for a person or an NPC alike. It is the same pillar as permadeath: NPCs are people,
and people change.

## How it works

One system, `advance_progression`, runs each tick over every entity that has
`Skills`, `Attributes`, `Stats`, `Velocity`, and `CharacterLevel` — four steps, top
to bottom (movement is shown; damage is a second feeder, see the note below):

```mermaid
flowchart LR
  act[moving this tick] --> xp[conditioning + endurance += rate; character += ¼ rate]
  xp --> lvl{a bar full?}
  lvl -->|yes| up[level that one up, carry the remainder]
  lvl -->|no| keep[wait]
  up --> stat[max = base + endurance bonus × per-point × POWER of character level]
  keep --> stat
```

1. **Activity earns XP for the skill *and* the attribute(s) it feeds** — every grant now
   flows through one funnel, `grant_skill_xp`, which reads a data-driven **`SkillDef`**
   table: it trains the skill, gives its **main** attribute the full share, and gives each
   **contributor** attribute a fraction. Moving trains `conditioning` → **`endurance`** (its
   main, no contributors). **Attacking** trains `striking` → **`strength`** (main) **and a
   quarter to `dexterity`** (contributor) — so a pure striker slowly picks up a little
   footwork, the design's "you are what you do" cross-training. And the **main attribute's
   *level* speeds the skill's own learning** — the attribute's **third role** (a stat, a
   skill-domain, *and* a learning-proficiency): a high-**Strength** character banks *more*
   `striking` XP per swing (the design's "a master-STR miner picks up Smithing faster",
   compounding domain-transfer), **+5% per main-attr level past the first, capped at ×2**, so
   specialising snowballs but never runs away — the "checked by the ever-harder law" (`xp_to_next`
   rises too). Only the *skill*'s XP is sped; the attribute/character shares stay flat (scaling an
   attribute's XP by its own level would runaway-compound). A fresh character (main-attr level 1) is
   bit-identical. Through that same funnel a
   **quarter-share** of *every* grant also feeds the global **`CharacterLevel`** — so **all**
   activity grows the veteran layer (moving, resting, striking, enduring blows, looting), not
   just walking. Standing still trains nothing.
2. **A full bar levels it up** — the same `while`-loop carry works on the skill,
   the attribute, and the character level; each has its own `{level, Fixed xp}` and
   climbs independently. (XP is a `Fixed` so 20/sec accrues cleanly as ~0.33 per
   60 Hz tick — an `int` would round every tick's gain to nothing.)
3. **The attribute's level shapes derived stats** — each Endurance level past the
   first adds to the pools, on top of each Vital's own `base`. Only the **max**
   grows: a longer bar, not a free heal, and regen fills the new room in.
4. **The character level compounds the earned bonus** — that pool bonus is scaled
   by `POWER(character level − 1)`, the same diminishing curve skills use. Level 1
   is `POWER(0)` = 1.0 (no head start); a veteran's earned toughness then compounds
   a little. It multiplies what you *earned*, never the base floor. The **same
   multiplier scales the earned attack delta on combat damage** — a swing's earned
   Strength (`perform_attack`), a throw's earned Dexterity (`perform_throw`), and a
   bolt's earned Intellect (`magic_bolt`), all through the one `veteran_mult` — so the
   veteran layer isn't just bigger bars: a seasoned fighter hits harder with blade,
   throw, and spell alike. (Reach is left flat, so a veteran hits harder without
   changing which targets a swing can reach.) The character level also **wears a rank**: `veteran_title` names
   it Novice → Seasoned → Veteran → Grizzled in the HUD — the *experience* twin of the
   `standing_title` (how good/bad) and `build_title` (what fighter) derived recognitions,
   a pure query the sim never reads.

!!! note "XP comes from many places; leveling happens in one"
    Step 1 grants **either** the *movement* XP (Conditioning) **or**, when idle with
    stamina to recover, the *resting* XP (Recovery) — both feed Endurance. Other
    activities feed skills from their own sites: **taking damage** feeds **Toughness**
    → Endurance (`train_on_damage`, wherever damage lands), and **attacking** feeds
    **Striking** → Strength (`perform_attack`, via the player's `Attack` command or the
    `npc_attack` system). Every one of those grants also drips a quarter into the
    **character level** (the funnel does it), so combat and looting build the veteran layer
    too — not just movement. Step 2's loop then levels *every* owned skill — plus both
    attributes and the character level — so those climb here too without `advance_progression`
    knowing where the XP came from. Many sources, one place they turn into levels.

Because the view targets `Skills + Attributes + Stats + Velocity + CharacterLevel`,
it lands on the player and the NPCs and skips the motes — one system, everyone who
should grow.

!!! info "Learn by doing, not spend points"
    There is no XP pool to allocate and no level-up screen. Doing the thing levels
    the thing (Skyrim / UnReal World lineage). Attributes are *recomputed* from
    skills every tick, never set by hand, so a skill and its attribute can never
    drift out of sync.

## What to expect

Move around the demo and watch the panel: the conditioning bar fills, ticks over
to level 2, `endurance` becomes 1, and the health, stamina, and mana bars lengthen as the
bigger pools take hold. The NPCs are doing the same thing off to the side — one
that survives a while is measurably harder to kill than a fresh arrival.

## The balancing dial

You flagged NPC growth as something to tune, and this is built so tuning is a
*number*, not a rewrite:

- The whole curve is one constant (`xp_to_next` is linear in level). Right now NPCs
  train at the **same** rates as the player; a per-entity or per-faction multiplier
  is the knob that keeps their growth in check, and it slots into step 1.
- Each strand is one skill → one attribute → one effect, and they compose freely:
  Endurance already has **seven** feeders (Conditioning, Toughness, Recovery, Guarding, Resistance,
  Survivalist, Attunement), and Strength is the second attribute (via Striking → reach). More skills/attributes are more of the
  same fields plus the activity that trains each — the shape widens, it doesn't change.

## Where it goes next

More skills, each with an activity that trains it; more attributes, each shaped by
its skills and feeding a different derived stat or system. Then the balancing pass
for NPC growth against the player's. The shape you see here — activity → skill →
attribute → stat — is what stays as it widens into a full character sheet.

## Key files

- `engine/sim/components.hpp` — `Skill`, `Skills`, `Attributes` (Endurance, Strength, Dexterity, Luck, Wisdom, Charisma), the `AttrId` enum, `CharacterLevel`; the `SkillId` enum (18 skills across all seven attributes, from `Conditioning` through the newest, `Attunement` — the MP resource-skill).
- `engine/sim/systems.cpp` (anon namespace) — the `SkillDef` table, `attr_ref`, and `grant_skill_xp` (the one funnel every skill→attribute XP grant flows through: the skill's XP sped by its **main attribute's level** (learning-proficiency, +5%/level ×2), then flat main + contributor + character shares).
- `engine/sim/systems.hpp` / `systems.cpp` — `xp_to_next`, `advance_progression` (movement→Conditioning / resting→Recovery), `update_stamina` (Endurance speeds recovery), `train_on_damage` (the damage → Toughness feeder), `perform_attack` (the shared swing resolver) and `npc_attack` (NPCs fight too).
- `engine/sim/command.hpp` / `world.cpp` — the `Attack` command (the striking feeder, computes reach from Strength); progression components on the player and NPCs.
- `game/app/main.cpp` — the endurance/strength/wisdom/charisma/character-level readout, each equipped item's remaining durability (hits/blows left), and the skill XP bars; the `J` = attack key.
- `engine/sim/systems.cpp` — `bond_witnesses` (the camaraderie grant, called at both a kill and a rescue): Charisma scales a witness's devotion, and a witnessed heroic act trains Leadership → Charisma (the compounding social loop).
- `tests/sim/test_simulation.cpp` — activity trains-and-grows, idle trains nothing, damage trains Toughness, attacking trains Striking → Strength, grazing trains Foraging → Wisdom (and a wiser forager yields more), collecting trains Scavenging → Luck (and a lucky scavenger mends more from an orb), leading trains Leadership → Charisma (and a charismatic champion is bonded harder AND witnessed from farther), blocking a blow trains Guarding → Endurance (but an open stance does not), enduring venom trains Resistance → Endurance (but an unpoisoned character does not), sprinting trains Athletics → Dexterity (but a plain walker does not), and Strength eases a weapon's heft up to a half floor (the carry mastery, on the player and NPCs alike), and Endurance eases an armour's stamina bane up to a half floor (the armour twin, isolated from its recovery boost by the armoured-to-bare ratio).

## Go deeper

- [The stats system](stats-system.md) — the vitals Endurance grows.
- [The tick and the systems](skeleton/tick-and-systems.md) — where `advance_progression` sits in the tick.
- [NPC behaviour](npc-behaviour.md) — the other thing NPCs now do on their own.
