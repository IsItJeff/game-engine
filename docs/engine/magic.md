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
find the tome — not a freebie at spawn. `study_spellbooks` is player-only for now and skips a caster
who already knows the spell (a book is spent only on a real lesson); teaching it to NPCs, and learning
*more* spells from further books or a mentor, are the next slices of the trunk.

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

Enemies only (a bolt never targets a colonist — villainy stays a deliberate melee choice), and
player-only for now (there is no NPC caster yet). The kill credit and Valor flow through
`advance_projectiles` exactly as a throw's do.

## What's next

This is a seam, not the whole trunk. **Learning** now exists — a `Spellbook` you read — so growing
from here: **more spells** (a heal, an area blast, each its own book), an **NPC caster** (and NPCs
*learning* from tomes or a mentor, the parity the design wants), the **Focus / Attunement** skills
that govern the mana pool's capacity and regen, and the **tech** branch (an Energy battery on gear,
the design's twin trunk). Each is a small add on this foundation.
