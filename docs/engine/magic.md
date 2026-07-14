# Magic

## What it is

The first seam of the **magic trunk** — a third resource beside health and stamina, and a first
spell that spends it. Magic is deliberately **learned, not innate**: everyone carries a trickle of
mana, but it does nothing until you have *learned* to cast.

| Piece | What it is |
|---|---|
| **`Stats.mp`** | the **mana** bar — a `Vital` like stamina, spent by casting and regenerating at rest |
| **`SkillId::Spellcasting`** | the **learned gate** — an entity that carries this skill can cast; one that doesn't, can't |
| **`AttrId::Intellect`** | the design's 7th attribute, the **magic** one — scales a spell's power, fed by casting |
| **`magic_bolt`** | the first **spell** — a homing bolt at the nearest hostile, the magic mirror of a throw |
| **`Cast` command** | the funnel path (press **C**) that runs `magic_bolt` for the player |
| **`heal_spell`** | the **support** spell — mends the nearest wounded ally, trains `Healing → Wisdom` |
| **`CastHeal` command** | the funnel path (press **H**) that runs `heal_spell` for the player |
| **`shield_spell`** | the **defensive** spell — raises a timed `Shielded` barrier on yourself, `Intellect`-scaled |
| **`CastShield` command** | the funnel path (press **B**) that runs `shield_spell` for the player |
| **`Shielded` + `tick_shield`** | the first **timed buff** — soaks `absorb` off each blow, aged/reaped each tick |

## Why it matters

Physical fighting is *innate* — anyone can swing. Magic is the design's other trunk: a power you
**earn the right to use**. Gating the very first bolt on a learned skill (not just a full mana bar)
is what makes that real from line one — a plain colonist with full mana still can't fling a spell.
It also lands **Intellect**, the last of the seven attributes, with its first job: sharpening a bolt
the way Strength sharpens a swing.

## How it works

### Mana — the third bar

`Stats.mp` is a `Vital` (`current` / `max` / regen), exactly the shape stamina uses: it is **spent**
(a cast) and **regenerates** steadily (`regenerate_vitals`), *faster the hardier the caster* —
Endurance speeds mp regen just as it speeds HP and stamina, so VIT governs the capacity **and** the
regen of all three resources (it already grows `mp.max`, so a bigger pool now also refills faster
rather than sustaining worse as you level). Its regen sits *before* the
starvation/venom gate that suppresses *healing* — a fed-and-clean body mends, but magic energy isn't
food, so a starving mage still recharges. Everyone has the bar; it just sits full and unused until
you can cast, so a world with no caster is **bit-identical** to before mana existed.

### The learned gate — `Spellcasting`

`magic_bolt` casts **only if the caster carries the `Spellcasting` skill**. That one check is the
whole "magic is taught, not innate" rule: a colonist who never learned it does nothing, whatever its
mana. And you **earn** it — the player starts *without* Spellcasting and learns it by **reading a
`Spellbook`** (`study_spellbooks`): walk onto the arcane-violet tome `build_scene` places off to one
side, and you gain the skill. So learning magic is a small quest — go find the tome — not a freebie at
spawn. The tome is a **permanent library**, not a one-shot scroll: reading it does **not** consume it,
so the whole colony can learn from one book over time (and the player no longer "steals" the only tome
by reaching it first — the supply a `Scholar`-aspiration colonist needs to reliably become a mage).
`study_spellbooks` teaches **any person** — the player *and* an NPC (a colonist who finds a tome becomes
a mage too, the player==NPC parity) — and skips a reader who already knows the spell (so a re-read is a
no-op; the lectern teaches each newcomer once). A learned caster then wears a small **arcane-violet
core** on the field (`draw_entities`), an inner spark distinct from the transient guard/downed/shield
rings — the visible payoff of the whole loop, so you can *see* which colonists a Spellbook has turned
into mages. Learning *more* spells from further books or a mentor is the next slice of the trunk.

### The first spell — `magic_bolt`

The magic mirror of `perform_throw`, and it reuses the very same homing **`Projectile`** the throw
flies (`advance_projectiles` delivers it, tinted arcane violet so a spell reads apart from a throw's
yellow bolt). What sets it apart:

- **It spends mana, not stamina** — `kSpellManaCost` (25) per cast, so a full bar is ~4 bolts. An
  empty bar **fizzles** (nothing spent, no XP), the magic echo of an exhausted thrower.
- **Intellect scales it** — `kBaseSpellDamage` (14) + a per-`Intellect`-level delta, softened by the
  target's VIT and by the same `need_efficiency` every attack uses (a starving mage casts weaker
  too). No crit, no dodge, no RNG — a plain, reliable bolt, like the throw.
- **Casting trains it** — a connecting cast grants `Spellcasting → Intellect`, so a mage sharpens
  its bolts by casting them, the learn-by-doing loop that a throw uses for `Throwing → Dexterity`.
  See [progression](progression.md).

Enemies only (a bolt never targets a colonist — villainy stays a deliberate melee choice). And it is
**not player-only**: `npc_cast` has any NPC that has learned Spellcasting fling the *same* bolt at a
hostile in range, on a full mana bar (a throttle to a considered shot, then a recharge — no per-tick
spam). So a **colonist mage fights beside you** — the player==NPC parity the design wants. The kill
credit and Valor flow through `advance_projectiles` exactly as a throw's do.

### The support spell — `heal_spell`

The first spell that reaches for a **friend**, not a foe — the co-op heart of an embodied colony that
**heals its own** (press **H**, the `CastHeal` command). It is the bolt's support twin, built on the
same shape:

- **It mends the nearest wounded ally** — a *person* (not a creature, not a downed body, **nor a body
  chipped to 0 HP this very tick**, not the caster itself) whose health is below max, within
  `kHealRange` (300). A full-health ally is never targeted, so **no mana is wasted** topping up the
  hale, and there's **no over-heal** (the mend is clamped at max). The 0-HP skip matters at the death
  seam: `heal_spell` runs *before* `handle_deaths` in the tick, so a body just chipped to 0 (not yet
  `Downed`) would otherwise be mended back from beyond the grave — resurrecting an NPC that should
  permadeath. (The `Downed` exclusion only covers a body downed on a *prior* tick; this closes the
  same-tick window, the patient-side twin of the caster's own 0-HP inert guard.)
- **It spends the same mana** — `kHealManaCost` (25), an empty bar fizzles like the bolt. It lands
  **instantly** on the ally (a mending word, no flying `Projectile`).
- **Wisdom scales it, and it trains a new skill** — `kBaseHeal` (18) + a per-`Wisdom`-level delta,
  scaled by the caster's `need_efficiency` (a starving healer mends weaker too). A connecting mend
  grants `Healing → Wisdom` (the design's WIS support domain), so a healer sharpens by healing the way
  a mage sharpens by casting. See [progression](progression.md).
- **It shares the learned gate** — a caster who read the tome can *both* bolt and mend (Spellcasting
  unlocks both for now; a dedicated Healing tome is a follow-up). So a non-caster still can't mend, and
  a world without casters is bit-identical.

Parity again: `npc_heal` drives the *same* `heal_spell` for any NPC that has learned to cast, on a
**full mana bar**. Because it runs *after* `npc_cast` and shares that full-bar throttle, a battle-mage
naturally **bolts a threat first** (spending its mana) and only **mends in a lull** when no bolt
fired — offence-then-support falls out of the schedule order, no priority flag needed. So a colonist
mage blasts monsters *and* patches up the wounded beside you.

### The defensive spell — `shield_spell`

The third of the trio (press **B**, the `CastShield` command). Where the bolt reaches out to a foe
and the mend reaches to a friend, the **shield turns inward**: a learned caster raises a timed
**barrier on itself**. It is the design's **"ward"-role** spell, named *Shield* here so it doesn't
collide with warded **armour**'s thorns. What sets it apart:

- **It is the game's first *timed buff*** — `Shielded {remaining, absorb}`, the beneficial mirror of
  the `Poisoned` DoT (same `{timer, magnitude}` shape). While it lasts, **`absorb` is soaked off each
  incoming hit** — a **melee blow** (`resolve_creature_contacts`) *and* a **ranged shot**
  (`advance_projectiles`: a spitter's venom bolt or a thrown one) — a **general** damage buffer, the
  last line of defence *on top of* armour and VIT. That the ward covers ranged matters: `npc_shield`
  raises it when a **creature closes**, and a **spitter** is exactly such a creature, so a ward that
  stopped the claw but not the venom bolt would leave the mana spent for nothing against the one
  ranged threat. Like the melee case it stops **damage, not contact** — a venom spit still envenoms a
  shielded target (a poison-ward is a separate spell). Floored at 0: unlike mitigation's permanent 10%
  chip floor, a temporary, mana-bought, expiring barrier is allowed to fully eat a weak blow — that's
  the point of raising it.
- **`Intellect` thickens it, but hunger thins it** — `kBaseAbsorb` (6) + a per-`Intellect`-level
  delta, so the same attribute that sharpens a bolt hardens a shield, then **scaled by the caster's
  `need_efficiency`** — a starving (or parched, or freezing) mage wards weaker too, the same debuff the
  bolt and mend already carry, so there is no full-strength-defence loophole under empty needs. Casting
  it trains `Spellcasting → Intellect`, the same learn-by-doing loop (a dedicated Abjuration/Warding
  skill is a follow-up).
- **It spends the same mana** — `kShieldManaCost` (25), an empty bar fizzles like the bolt/mend — and
  **`tick_shield`** ages the barrier each tick, reaping it when `remaining` runs out. It runs *after*
  contacts, so a freshly-cast ward still soaks the blows of the tick it's raised (apply-at-cast,
  use-at-contact, decay-after — poison's rhythm).
- **It shares the learned gate and the caster-inert guard** — a non-caster can't shield (bit-identical
  world), and a body at 0 HP can't cast it (like the bolt/mend). On the field it reads as a bright
  **arcane-cyan ring**, distinct from the guard's steel-blue.

The player casts it via `CastShield`, and — like the bolt and mend — an NPC casts the *same*
`shield_spell` via **`npc_shield`**: a learned colonist mage with a full mana bar raises the barrier on
itself when a creature closes within a threat range (and isn't already warded), completing the
player==NPC parity for all three spells. It runs *before* `npc_cast` in the schedule, so a threatened
battle-mage **wards first, then bolts under the barrier** — and re-wards only after the ward lapses (the
not-already-`Shielded` gate), never wasting mana on a re-cast while it holds.

## What's next

This is a seam, not the whole trunk. **Learning**, **NPC casters**, **support magic**, a **defensive
ward**, and now **NPCs that seek magic out** exist (a `Spellbook` you read, a colonist mage that casts
beside you, a mend that heals your allies, a barrier that soaks a blow, and a [`Scholar`-aspiration
colonist](npc-behaviour.md) that walks to a tome to learn — from a **permanent library** that reading no
longer consumes, so every Scholar reliably becomes a mage) — the **offence / support / defence** trio is
complete, and a colonist mage now casts all three (`npc_cast` / `npc_heal` / `npc_shield`), the full
player==NPC parity. Growing from here: **more spells** (an area blast, each its own book), a
**dedicated Healing/Abjuration tome**
so the support and ward skills have their own learn-paths (today they ride the Spellcasting gate), the
**Focus / Attunement** skills that give the mana pool its OWN per-skill capacity and regen (its
*capacity* already grows with VIT/Endurance like the health and stamina pools — see
[progression](progression.md) — but a dedicated attunement skill and VIT-scaled regen are still to
come), and the **tech** branch (an Energy battery on gear, the design's twin trunk). Each is a small
add on this foundation.
