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

## Why it matters

Physical fighting is *innate* — anyone can swing. Magic is the design's other trunk: a power you
**earn the right to use**. Gating the very first bolt on a learned skill (not just a full mana bar)
is what makes that real from line one — a plain colonist with full mana still can't fling a spell.
It also lands **Intellect**, the last of the seven attributes, with its first job: sharpening a bolt
the way Strength sharpens a swing.

## How it works

### Mana — the third bar

`Stats.mp` is a `Vital` (`current` / `max` / regen), exactly the shape stamina uses: it is **spent**
(a cast) and **regenerates** steadily (`regenerate_vitals`). Its regen sits *before* the
starvation/venom gate that suppresses *healing* — a fed-and-clean body mends, but magic energy isn't
food, so a starving mage still recharges. Everyone has the bar; it just sits full and unused until
you can cast, so a world with no caster is **bit-identical** to before mana existed.

### The learned gate — `Spellcasting`

`magic_bolt` casts **only if the caster carries the `Spellcasting` skill**. That one check is the
whole "magic is taught, not innate" rule: a colonist who never learned it does nothing, whatever its
mana. And you **earn** it — the player starts *without* Spellcasting and learns it by **reading a
`Spellbook`** (`study_spellbooks`): walk onto the arcane-violet tome `build_scene` places off to one
side, and you gain the skill (the book is then consumed). So learning magic is a small quest — go
find the tome — not a freebie at spawn. `study_spellbooks` teaches **any person** — the player *and*
an NPC (a colonist who finds a tome becomes a mage too, the player==NPC parity) — and skips a reader
who already knows the spell (a book is spent only on a real lesson). Learning *more* spells from
further books or a mentor is the next slice of the trunk.

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

- **It mends the nearest wounded ally** — a *person* (not a creature, not a downed body, not the
  caster itself) whose health is below max, within `kHealRange` (300). A full-health ally is never
  targeted, so **no mana is wasted** topping up the hale, and there's **no over-heal** (the mend is
  clamped at max).
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

## What's next

This is a seam, not the whole trunk. **Learning**, **NPC casters**, and now **support magic** exist (a
`Spellbook` you read, a colonist mage that casts beside you, and a mend that heals your allies). Growing
from here: **more spells** (an area blast, a ward, each its own book), a **dedicated Healing tome** so
the support skill has its own learn-path (today it rides the Spellcasting gate), NPCs *seeking out*
tomes or a **mentor** to teach them, the **Focus / Attunement** skills that govern the mana pool's
capacity and regen, and the **tech** branch (an Energy battery on gear, the design's twin trunk). Each
is a small add on this foundation.
