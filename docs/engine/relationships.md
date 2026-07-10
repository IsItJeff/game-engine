# Relationships: directed bonds

## What it is

The first trace of a character's **social ties** — who they've come to care about.
[Personality](npc-behaviour.md) is who a colonist *is* and [morality](morality.md) is
what they've *done*; relationships are who they're *bonded to*. It is the seed of the
design's P8 social layer, and it starts, deliberately, as one component and one event:

- **`Relation`** — one **directed** tie: `THIS entity → other`, carrying an `int8`
  **`affinity`** (`−100` dislike … `+100` like). Directed because A→B need not equal
  B→A.
- **`Relationships`** — a lazy, sparse, append-ordered `std::vector<Relation>` on the
  subject: how *this* character regards others.
- **`nudge_affinity(from, toward, delta)`** — the single write-point every future
  bond-forming event will funnel through (the [`record_deed`](morality.md#how-it-works)
  twin). The **rescue** already fires it: hauling up a fallen ally forms a bond.

## Why it matters

This is the same three-part discipline the morality ledger shipped, and for the same
reason — the *schema* is painful to retrofit, so it is locked from the first line:

- a **single write-point** (so every future event — fighting side by side, sharing food,
  a betrayal — is one `nudge_affinity` call, never new plumbing);
- a **lazy component** (an entity earns a `Relationships` only on its first bond, so a
  never-bonding colonist — and the whole pre-relationships world — carries nothing and
  replays **bit-identically**);
- and a **locked, minimal schema** — `affinity` is fed now; its two design siblings wire
  later with *no reshape*. `trust` is a one-field append the day its own event lands; the
  bond ladder (Acquaintance → Friend → Partner / Rival → Nemesis) is a **derived** band
  `bond_tier(affinity)` — a pure query, never a stored slot — exactly the
  `standing → standing_title` split.

## How it works

One event forms a bond today. It lives at the **rescue** in `handle_deaths` — the same
line that already credits the rescuer with **Charity**:

```cpp
record_deed(reg, rescuer, Deed::Charity, kRescueCharity);
nudge_affinity(reg, rescuer, e, kRescueAffinity);  // the rescuer bonds to the one it saved
```

The direction is deliberate: only players go **Downed**, so the rescued (`e`) is a player,
which doesn't run `steer_npcs`. Putting the edge on the **rescuer** (usually an NPC that
*does* steer) is what lets the bond produce **visible motion**. (If NPCs ever go Downed,
that rationale needs a second look — noted at the call site.)

`nudge_affinity` is the `record_deed` twin: `get_or_emplace` the component on the first
bond, then **find-or-update** the edge toward the target — deepen an existing tie or append
a new one — so the edge count is the number of *distinct partners*, never per-tick growth.
Pure integer add, clamped to `±100`, touching no view (safe mid-iteration).

The first **reader** is a new lowest rung in `steer_npcs`, the *personal* twin of the
**hero-rally** (see [NPC behaviour](npc-behaviour.md)): an idle colonist with **no public
hero** to gather to drifts toward its nearest
well-liked **friend** — so *the colony clusters by bond, not only around the famous*. A
colonist you rescued now sticks by you even while you're Unproven. It reuses the rally
toward-vector and speed verbatim, reading affinity instead of standing, and sits *below*
the hero-rally (guarded by `if (champion == null)`), so the public rally stays
byte-identical. It **must** gate on `reg.valid(other)` before steering — edges store
entity ids by value and ids recycle, so a stale tie to a dead entity is skipped, never
dereferenced.

## The tradeoffs

- **One event, one reader.** No personality-match seeding (that's a later ring), no bond
  *stages*, no decay — just the smallest struct those grow into. Exactly as the morality
  seed shipped two of six deeds and let the rest wire themselves.
- **`affinity` has one reader.** The bond-pull is a presentation-ish behaviour (a gentle
  gather); the *deeper* readers — a colonist rescuing a friend *first*, fleeing *with*
  them, refusing to fight them — are later work.
- **Unbounded edge list.** Bounded in practice (ties form only on a rescue, only toward a
  downed player), but formally open — a `ponytail:` comment names the `cap-N +
  evict-weakest` upgrade so it's tracked debt, not silent.
- **`int8`, not the ledger's `int32`.** Affinity *saturates into bands*; it doesn't
  accumulate over a life toward a gate the way `standing` does. So it takes the
  Personality-axis width, and no float or wide int enters the sim.

## Where it goes next

The immediate follow-up unblocks the **last personality axis**. `loyalty` — whose comment
already says it "waits, appending here once relationships give a behaviour to read it" —
wires as the bond-pull's radius knob, the identical trait-scaled-radius shape every other
axis uses: `kBondRadius × (1 + loyalty/200)`. A loyal colonist crosses the field to stay
near a bonded ally; a disloyal one drifts off unless the friend is underfoot. One field,
one line.

Beyond that, the write-point is the whole point: more events (fighting a common foe, a
shared meal, a betrayal) each become one `nudge_affinity` call; `trust` appends as a
second `Relation` field; the derived `bond_tier` names the ladder; and a **leaky decay**
lets cold ties fade. Then the social `perceive` layer reads affinity *and* standing to
choose stances (befriend / protect / exploit).

## Key files

- `engine/sim/components.hpp` — `Relation` (one directed tie) and `Relationships` (the
  lazy sparse edge list), placed beside `BehaviorLedger`/`standing`.
- `engine/sim/systems.hpp` / `systems.cpp` — `nudge_affinity` (the single write-point,
  the `record_deed` twin); the bond formed at `handle_deaths`' rescue branch; the
  bond-pull rung in `steer_npcs` (below the hero-rally, gated on no champion).
- `tests/sim/test_simulation.cpp` — the write-point (find-or-update + clamp), the bond
  wired at a rescue, the reader steering toward a friend (and the range gate), and the
  stale-handle guard.

## Go deeper

- [Morality](morality.md) — `standing` and `record_deed`, the seed this one mirrors.
- [NPC behaviour](npc-behaviour.md) — the steer ladder the bond-pull joins, and the
  `Personality` axes (`loyalty` is the one this unblocks).
