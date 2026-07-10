# Relationships: directed bonds

## What it is

The first trace of a character's **social ties** — who they've come to care about.
[Personality](npc-behaviour.md) is who a colonist *is* and [morality](morality.md) is
what they've *done*; relationships are who they're *bonded to* — or *against*. It is the seed of
the design's P8 social layer, and it starts, deliberately, as one component and a small handful of
affinity-moving events:

- **`Relation`** — one **directed** tie: `THIS entity → other`, carrying an `int8`
  **`affinity`** (`−100` dislike … `+100` like). Directed because A→B need not equal
  B→A.
- **`Relationships`** — a lazy, sparse, append-ordered `std::vector<Relation>` on the
  subject: how *this* character regards others.
- **`nudge_affinity(from, toward, delta)`** — the single write-point every affinity-moving
  event funnels through (the [`record_deed`](morality.md#how-it-works) twin). Three fire it today:
  a **rescue** forms a bond, a **cruel strike** forms a grudge, and **felling a foe near allies**
  forges *camaraderie* (nearby colonists warm to the killer).

## Why it matters

This is the same three-part discipline the morality ledger shipped, and for the same
reason — the *schema* is painful to retrofit, so it is locked from the first line:

- a **single write-point** (so each event — fighting side by side and a betrayal already, sharing
  food still to come — is one `nudge_affinity` call, never new plumbing);
- a **lazy component** (an entity earns a `Relationships` only on its first bond, so a
  never-bonding colonist — and the whole pre-relationships world — carries nothing and
  replays **bit-identically**);
- and a **locked, minimal schema** — `affinity` is fed now; its two design siblings wire
  later with *no reshape*. `trust` is a one-field append the day its own event lands; the
  bond ladder (Acquaintance → Friend → Partner / Rival → Nemesis) is a **derived** band
  `bond_tier(affinity)` — a pure query, never a stored slot — exactly the
  `standing → standing_title` split.

## How it works

**Three events** move affinity today — two that bond, one that grudges. The first positive one lives
at the **rescue** in `handle_deaths` — the same line that already credits the rescuer with
**Charity**:

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

The negative event is the **cruel strike** in `perform_attack` (`nudge_affinity(victim, attacker,
kCrueltyGrudge)` — the struck colonist resents its striker). The *second positive* one is
**camaraderie**: the same killing blow that credits the attacker **Valor** also bonds nearby allies
to them — `bond_witnesses` scans standing colonists within `kCamaraderieRadius` of the kill and
`nudge_affinity(witness, killer, kCamaraderieAffinity)` for each. It's the design's *"fighting a
common foe"*, directed **witness → killer**, so it feeds the very readers below: the colony
**clusters toward** (bond-pull) and **rescues from farther** (the graded rescue reach) a champion who
fights beside it. Lighter than a rescue's `+20` (witnessing is a smaller thing than being saved), so
devotion accrues over several shared victories. Because both player and NPC kills route through the
shared `perform_attack`, a colonist earns camaraderie whether *you* or a fellow colonist lands the
blow. *(ponytail: only MELEE kills bond today — the ranged `advance_projectiles` kill is the same
event and is the noted follow-up.)*

The affinity is **read two ways** — a positive draw and a negative repulsion, the two faces of a
tie:

- **Bond-pull** (positive) — a new lowest rung in `steer_npcs`, the *personal* twin of the
  **hero-rally** (see [NPC behaviour](npc-behaviour.md)): an idle colonist with **no public hero**
  to gather to drifts toward its nearest well-liked **friend** — so *the colony clusters by bond,
  not only around the famous*. A colonist you rescued sticks by you even while you're Unproven. It
  scans its own edges for the closest positive tie, reuses the rally toward-vector and speed
  verbatim, sits *below* the hero-rally (guarded by `if (champion == null)`) so the public rally
  stays byte-identical, and gates on `reg.valid(other)` — a stale tie to a recycled id is skipped,
  never dereferenced.
- **Grudge / abandonment** (negative) — the resentful side. Every cruel strike, besides the Cruelty
  deed, fires a *second* `nudge_affinity(victim, attacker, kCrueltyGrudge)` — the struck colonist
  forms a **grudge** toward the striker (the mirror of the rescue's bond). Its reader is a check in
  the **rescue** path, via `affinity_toward(from, toward)` — the targeted read-side counterpart of
  `nudge_affinity` (the tie's value, or 0 if none): a colonist whose affinity to the fallen has
  sunk to `kGrudgeThreshold` won't cross the field to save them (`steer_npcs`) *nor* haul up one
  already at its feet (`handle_deaths`)
  — **the resented are abandoned.** Because one strike (`−25`) clears the threshold (`−20`), it is a
  *personal, earlier* consequence than the global [villain-fear](morality.md) (which needs several
  strikes to push `standing` past the Suspect line): hurt one colonist and *that* colonist won't
  save you, long before the whole colony fears you.
  Above that hard cutoff the same `affinity_toward` read now **grades the rescue reach**: the
  fallen's distance is discounted by affinity (the `/200` shape the other rungs use), so a bonded
  ally — its affinity grown by past saves — is worth a *longer* field, while a mild dislike (still
  above the grudge line) is dropped *sooner*. The reader is a smooth dial from "cross the whole map
  for my dearest friend" down through neutral to the hard "abandon the resented" floor. It closes a
  small loop: **save an ally → its affinity climbs → you reach it from farther next time**, so a
  colonist becomes a devoted protector of those it has saved.

## The tradeoffs

- **Three events, a few readers.** No personality-match seeding (that's a later ring), no bond
  *stages*, no decay — just the smallest struct those grow into. Exactly as the morality
  seed shipped a couple of deeds and let the rest wire themselves.
- **The readers are still coarse.** A gentle gather, a rescue-veto, and now a graded rescue reach
  (a friend reached from farther); the *deeper* ones — fleeing *with* a friend, refusing to *fight*
  them, healing them *first* — are later work.
- **Unbounded edge list.** Bounded in practice (ties form only on a rescue, a grudge, or a shared
  kill — a finite set of triggers), but formally open — a `ponytail:` comment names the `cap-N +
  evict-weakest` upgrade so it's tracked debt, not silent.
- **`int8`, not the ledger's `int32`.** Affinity *saturates into bands*; it doesn't
  accumulate over a life toward a gate the way `standing` does. So it takes the
  Personality-axis width, and no float or wide int enters the sim.

## Where it goes next

The immediate follow-up already landed: this seed **unblocked the last personality axis**.
`loyalty` now scales the bond-pull's radius — `kBondRadius × (1 + loyalty/200)`, the identical
trait-scaled-radius shape every other axis uses — so a loyal colonist crosses the field to stay
near a bonded ally while a fickle one follows only a friend underfoot. **All six personality
axes are now wired.**

Beyond that, the write-point is the whole point: three events already prove it out (a rescue bonds,
a cruel strike grudges, a shared kill forges camaraderie), so each further event — a **shared meal**,
a **ranged** kill (the melee camaraderie's twin) — is just one more `nudge_affinity` call; `trust`
appends as a second `Relation` field; the derived `bond_tier` names the ladder; and a **leaky decay**
lets cold ties fade (and a grudge cool). Then the social `perceive` layer reads affinity *and*
standing to choose stances (befriend / protect / exploit).

## Key files

- `engine/sim/components.hpp` — `Relation` (one directed tie) and `Relationships` (the
  lazy sparse edge list), placed beside `BehaviorLedger`/`standing`.
- `engine/sim/systems.hpp` / `systems.cpp` — `nudge_affinity` (the single write-point, the
  `record_deed` twin) and `affinity_toward` (its read-side counterpart); the bond formed at
  `handle_deaths`' rescue branch, the grudge at `perform_attack`'s cruel-strike branch, and
  `bond_witnesses` (camaraderie) at `perform_attack`'s killing-blow branch; the bond-pull rung in
  `steer_npcs` (below the hero-rally), the grudge-veto in both rescue paths, and the
  affinity-discounted rescue reach on the `steer_npcs` rescue rung.
- `tests/sim/test_simulation.cpp` — the write-point (find-or-update + clamp), the bond
  wired at a rescue, the bond-pull steering toward a friend (and the range gate), the
  stale-handle guard, and the grudge (a cruel strike resents the striker; a grudge-holder
  won't cross to rescue nor haul up the resented); and the graded rescue reach (a bond extends the
  trek beyond the base radius, a mild dislike shortens it — isolated from the bond-pull rung).

## Go deeper

- [Morality](morality.md) — `standing` and `record_deed`, the seed this one mirrors.
- [NPC behaviour](npc-behaviour.md) — the steer ladder the bond-pull joins, and the
  `Personality` axes (`loyalty` is the one this unblocks).
