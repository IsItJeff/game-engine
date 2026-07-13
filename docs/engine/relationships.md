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
  event funnels through (the [`record_deed`](morality.md#how-it-works) twin). Five fire it today:
  a **rescue** forms a **mutual** bond between rescuer and saved *and* (if witnessed) earns the
  rescuer **admiration** from onlookers, a **cruel strike** forms a grudge, **felling a foe near
  allies** forges *camaraderie* (nearby colonists warm to the hero), and a **first lesson**
  forges *gratitude* (a student bonds to the mentor who teaches it a new craft).

## Why it matters

This is the same three-part discipline the morality ledger shipped, and for the same
reason — the *schema* is painful to retrofit, so it is locked from the first line:

- a **single write-point** (so each event — fighting side by side and a betrayal already, sharing
  food still to come — is one `nudge_affinity` call, never new plumbing);
- a **lazy component** (an entity earns a `Relationships` only on its first bond, so a
  never-bonding colonist — and the whole pre-relationships world — carries nothing and
  replays **bit-identically**);
- and a **locked, minimal schema** — `affinity` is fed now; `trust` is a one-field append the
  day its own event lands (still future, *no reshape*). The bond ladder (Acquaintance → Friend →
  Partner / Rival → Nemesis) has **landed** as a **derived** band `bond_tier(affinity)` — a pure
  query naming where an affinity falls, never a stored slot, exactly the `standing → standing_title`
  split. Its edges reuse the **behavioural** thresholds so the name matches what the sim already does
  at that affinity: Acquaintance begins at `kBondPull` (+10, a tie that pulls you toward a friend),
  Rival at `kGrudgeThreshold` (−20, a grudge deep enough to abandon the resented); the debug HUD
  reads out the player's **closest bond** by it — and, beside it, an **`allies`** count: how many
  colonists have bonded *to* the player (`allies_of`, the incoming mirror of the closest outgoing
  tie), which is exactly the set the [defend rung](npc-behaviour.md) will send rushing to your side.
  The deep bands **latch**: `bond_latched` (a Partner
  `≥ +80` or a Nemesis `≤ −60`) marks a tie that **resists decay**, so the strongest bonds and grudges
  persist while casual ones fade (see the leak below). A **Partner** earns defend teeth to match its
  durability: the [defend rung](npc-behaviour.md) sends a colonist across `kPartnerDefendBoost`× the
  reach to *defend* one — the active twin of the farther reach a dear ally already gets when **Downed**
  (the rescue rung, which grades reach continuously by raw affinity rather than the tier). And a
  **Nemesis** earns the mirror of those teeth: the [avoid rung](npc-behaviour.md) gives it a berth
  `kNemesisAvoidBoost`× wider than a merely-resented rival gets — you keep the most distance from your
  worst enemy. Both are tier-keyed, so only the two *latched* extremes (Partner / Nemesis) feel the
  boost; a passing bond or grudge in between behaves exactly as before.

## How it works

**Five events** move affinity today — four that bond, one that grudges. The first positive one lives
at the **rescue** in `handle_deaths` — the same line that already credits the rescuer with
**Charity**:

```cpp
record_deed(reg, rescuer, Deed::Charity, kRescueCharity);
nudge_affinity(reg, rescuer, e, kRescueAffinity);  // the rescuer bonds to the one it saved...
nudge_affinity(reg, e, rescuer, kRescueAffinity);  // ...and the saved bonds back — MUTUAL
```

The bond is **mutual** — hauling someone off the ground ties you both — but the two halves do
different work. The **rescuer → rescued** edge drives **visible motion**: the rescuer is usually an
NPC that runs `steer_npcs`, so it can later drift back toward the ally it saved (the bond-pull rung).
The **rescued → rescuer** edge is inert in the sim — the rescued (`e`) is always a player (the view
is `PlayerControlled`-gated), and a player doesn't steer — but it is the *one outgoing bond a player
ever forms*: every **other** event puts the edge on someone else (camaraderie bonds witnesses **to**
a killer, a grudge points a victim **at** their attacker), so being **saved** is the single thing
that finally fills the player's own **closest bond** readout, with the ally who saved their life.
Both halves use the same `kRescueAffinity` — a rescue is felt equally on both sides. (The original
one-way edge deferred the reciprocal half precisely because it adds no motion; this lands it for the
readout.)

`nudge_affinity` is the `record_deed` twin: `get_or_emplace` the component on the first
bond, then **find-or-update** the edge toward the target — deepen an existing tie or append
a new one — so the edge count is the number of *distinct partners*, never per-tick growth.
Pure integer add, clamped to `±100`, touching no view (safe mid-iteration).

The negative event is the **cruel strike** in `perform_attack` (`nudge_affinity(victim, attacker,
kCrueltyGrudge)` — the struck colonist resents its striker). And that cruelty is **witnessed**: the
same strike scans the nearby colonists (within `kCamaraderieRadius`, skipping the victim) and gives
each a *smaller* `kWitnessGrudge` toward the striker — the negative mirror of camaraderie, so a
reputation for cruelty **spreads**. One witnessed cruelty is milder than the victim's own grudge and
won't cross the abandonment line, but a *pattern* of them will: hurt colonists in front of the colony
and it stops trusting you, not just the ones you struck. The *second positive* one is
**camaraderie**: the same killing blow that credits the attacker **Valor** also bonds nearby allies
to them — `bond_witnesses` scans standing colonists within `kCamaraderieRadius` of the kill and
`nudge_affinity(witness, killer, kCamaraderieAffinity)` for each. It's the design's *"fighting a
common foe"*, directed **witness → killer**, so it feeds the very readers below: the colony
**clusters toward** (bond-pull) and **rescues from farther** (the graded rescue reach) a champion who
fights beside it. Lighter than a rescue's `+20` (witnessing is a smaller thing than being saved), so
devotion accrues over several shared victories. Because both player and NPC kills route through the
shared `perform_attack`, a colonist earns camaraderie whether *you* or a fellow colonist lands the
blow. A **ranged** kill counts too: `advance_projectiles` calls the same `bond_witnesses` on a
felling shot, centred on the **shooter** (the one who fought), not the distant impact — so a
throw earns the same regard as a swing.

A **rescue** earns the same regard — the last forming event to spread. When a rescuer hauls up a
downed ally, `handle_deaths` calls that same `bond_witnesses` centred on the rescue, so nearby
onlookers **admire the hero** (witness → rescuer), exactly as they warm to a killer. The
witnessed-event set is now symmetric: a cruel strike spreads grudges, and a kill *and* a rescue both
spread bonds — the one heroism that used to go unseen no longer does.

A **first lesson** forges the fifth tie — **gratitude**. When `teach` (mentorship — a veteran passing
a skill to a nearby novice) lands the student a craft it never had (the skill first appears,
`0 → 1`), the student bonds to its mentor: `nudge_affinity(student, mentor, kGratitudeAffinity)`. It's
a **discrete** moment like the other three — a skill is *learned once*, so a lesson bonds once per
**new craft** passed (a mentor who later teaches a second craft the student lacks bonds it again), not
the per-tick XP trickle `teach` grants every adjacent tick. (Only the first-**learn** is caught, not a
rank-up: `grant_skill_xp` banks XP, and levels are applied a step later by `advance_progression`, so
within `teach` the only visible breakthrough is the skill *appearing*.) Unlike the witnessed events
it's a **direct** tie (student → mentor), not one spread to onlookers. Set just above `kBondPull`, so
**one** lesson is a real Acquaintance bond the readers act on — the apprentice **clusters toward**,
**defends**, and is **rescued from farther** by the master who taught it — yet unlatched, so it fades
if the apprenticeship doesn't continue.

A witnessed heroic act — a kill *or* a rescue — is scaled by the hero's **Charisma** (see
[progression](progression.md)) in **two** ways. *How much* devotion each witness feels:
`kCamaraderieAffinity` grows by `1 + (CHA − 1) × 0.1`, capped at ×2, so a **charismatic champion**
inspires a deeper bond than a plain one. *How far* the deed is witnessed: `kCamaraderieRadius` grows
by `1 + (CHA − 1) × 0.05`, capped at ×1.5, so a charismatic hero's heroism reaches a **wider ring**
of onlookers (a gentler cap than the depth, since the radius grows the witnessed area quadratically).
So a charismatic champion inspires **more onlookers, each more deeply** — and because
leading those acts *trains* Charisma (kills and rescues alike, through the shared `bond_witnesses`),
the effect **compounds** (lead more → higher CHA → allies bond harder *and* from farther). At CHA 1
(the spawn default) both are exactly ×1, so the pre-Charisma world is byte-identical.

The affinity is **read two ways** — a positive draw and a negative repulsion, the two faces of a
tie:

- **Bond-pull** (positive) — a new lowest rung in `steer_npcs`, the *personal* twin of the
  **hero-rally** (see [NPC behaviour](npc-behaviour.md)): an idle colonist with **no public hero**
  to gather to drifts toward its nearest well-liked **friend** — so *the colony clusters by bond,
  not only around the famous*. A colonist you rescued sticks by you even while you're Unproven. It
  scans its own edges for the closest positive tie, reuses the rally toward-vector and speed
  verbatim, sits *below* the hero-rally (guarded by `if (champion == null)`) so the public rally
  stays byte-identical, and gates on `reg.valid(other)` — a stale tie to a recycled id is skipped,
  never dereferenced. Its **urgent twin** is the **defend** rung much higher up (just below the
  downed-rescue): where bond-pull *drifts* toward an idle friend, defend *charges* to a bonded friend
  a **creature is bearing down on** — same tie read (`affinity ≥ kBondPull`), but gated on a threat
  and outranking the colonist's own hunger, the active slice of the design's *PROTECT* stance (its
  reach scales with **bravery**, the nerve to charge into danger for a friend).
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
  save you, long before the whole colony fears you. That global line now has its own rescue veto: a
  colonist crossed the Suspect line (`standing ≤ −kKnownAt`) is **abandoned by the whole colony** —
  the [villain-veto](morality.md), checked beside this affinity read in *both* rescue paths. So the
  fallen are left down two ways: **personally** (a grudge from someone you wronged) and **globally**
  (infamy the whole colony shuns) — affinity and standing, the same abandonment from two scales.
  Above that hard cutoff the same `affinity_toward` read now **grades the rescue reach**: the
  fallen's distance is discounted by affinity (the `/200` shape the other rungs use), so a bonded
  ally — its affinity grown by past saves — is worth a *longer* field, while a mild dislike (still
  above the grudge line) is dropped *sooner*. The reader is a smooth dial from "cross the whole map
  for my dearest friend" down through neutral to the hard "abandon the resented" floor. It closes a
  small loop: **save an ally → its affinity climbs → you reach it from farther next time**, so a
  colonist becomes a devoted protector of those it has saved. **Public fame** grades the same reach in
  parallel: a downed **hero** (`standing ≥ +kKnownAt`) is discounted too, so the colony rushes to a
  fallen champion from farther *even without a personal bond* (the [morality](morality.md) villain-veto's
  bright mirror). Affinity and standing **stack** — a bonded hero is worth the longest trek of all.
- **Avoidance** (negative, active) — the grudge's *active* completion, the negative twin of
  bond-pull. Where a friend draws an idle colonist **toward** it, a resented one (affinity at/below
  `kGrudgeThreshold`) pushes it **away**: a new `steer_npcs` rung, just above the gather rungs, steers
  an otherwise-idle colonist straight away from its nearest resented (non-downed) entity within
  `kAvoidRadius`. So a colonist you struck not only refuses to save you — it *keeps its distance*.
  It's the **personal** counterpart of the global [villain-fear](morality.md): fear flees a famous
  villain (by `standing`); avoidance shies from someone *you personally* wronged (by affinity), and
  lands earlier (one strike clears the line). It reads **bravery** for its radius — the same
  threat-reactivity shape the danger-flee uses, so a coward keeps further off — and **skips a Downed
  target**: you don't flee a helpless body, you just don't help it (the rescue veto already covers
  that), which also keeps the abandonment rung byte-identical.

Beyond those steering reads, a bond is read one more place — at **death**. When a colonist a
survivor was bonded to at **Friend or above** (`kBondFriendAt`) is slain, `handle_deaths` drifts the
survivor's **bravery down** a step (`kGriefDrift`) — **grief**, the [morality drift](morality.md#drift-deeds-reshape-character)
path reaching the *living*, the negative mirror of a Valor deed's bravery-*up*. So a positive tie has
a price the day it breaks: **permadeath marks those left behind**, not just an empty slot (a rattled
mourner flees a hazard sooner). Only a real friend-bond grieves — a mere acquaintance, or a passing
rival, is neither mourned nor celebrated — the gate that keeps the pre-bond world bit-identical.

Grief has an **acute** twin. Besides the permanent nerve-slip, the loss **panics** the survivor
*now*: `handle_deaths` emplaces a `Panicked` marker for `kPanicDuration` (3 s), and while it lasts
`steer_npcs` routs the colonist — it senses danger from much farther (a widened flee radius), **bolts
faster**, and flees even the **creatures** it would normally stand and fight, until `tick_panic`
counts the rout down and its nerve returns. So a friend's death both **shakes you for good** (the
drift) and **breaks you for a moment** (the panic) — a mourner may drop its guard and run from the
very fight that took its friend. No `Panicked` marker (anyone not freshly bereaved) → `steer_npcs` is
unchanged, so it stays bit-identical.

Death reads a bond **both ways**. The quiet mirror of grief is **vindication**: when a sworn
**Nemesis** (`affinity ≤ kBondNemesisAt`, a *latched* deep grudge) is slain, `handle_deaths` drifts
the survivor's **bravery *up*** a step (`kVindicationDrift`, the positive mirror of `kGriefDrift`) —
the tormentor that cowed it is gone, so it stands a little taller and holds its ground where it used
to flinch. There is **no acute twin**: a rival's death is a lasting relief, not a shock, so nothing
like the panic rout fires. Only a true Nemesis vindicates — a passing dislike between the two floors
stirs nothing — and a Nemesis-tier grudge can't exist at spawn (it takes sustained cruelty to sink an
edge that low), so the pre-grudge world stays bit-identical. So the same `handle_deaths` pass that
**shakes** those who loved the fallen **emboldens** those who hated them: a death moves everyone who
felt strongly about the one who died, in whichever direction they felt.

## The tradeoffs

- **Five events, a few readers.** No personality-match seeding (that's a later ring) and no
  *stored* bond stages — the ladder is the derived `bond_tier`, not a slot — just the smallest struct
  those grow into. (Ties do now **decay** and the deepest **latch**, see above.) Exactly as the
  morality seed shipped a couple of deeds and let the rest wire themselves.
- **The readers are getting less coarse.** A gentle gather, a rescue-veto, a graded rescue reach
  (a friend reached from farther), and now an active **defend** — a colonist *charges* to a bonded
  friend a creature is bearing down on (the `steer_npcs` rung just below the downed-rescue, the first
  slice of the design's *PROTECT* stance). The *remaining* deeper ones — fleeing *with* a friend,
  refusing to *fight* them, healing them *first* — are still later work.
- **Unbounded edge list.** Bounded in practice (ties form only on a rescue, a grudge, a shared kill,
  or a first lesson — a finite set of triggers), but formally open — a `ponytail:` comment names the
  `cap-N + evict-weakest` upgrade so it's tracked debt, not silent.
- **`int8`, not the ledger's `int32`.** Affinity *saturates into bands*; it doesn't
  accumulate over a life toward a gate the way `standing` does. So it takes the
  Personality-axis width, and no float or wide int enters the sim.

## Where it goes next

The immediate follow-up already landed: this seed **unblocked the last personality axis**.
`loyalty` now scales the bond-pull's radius — `kBondRadius × (1 + loyalty/200)`, the identical
trait-scaled-radius shape every other axis uses — so a loyal colonist crosses the field to stay
near a bonded ally while a fickle one follows only a friend underfoot. **All six personality
axes are now wired.**

Beyond that, the write-point is the whole point: five events already prove it out (a rescue bonds the
saved *and* wins the rescuer admiration, a cruel strike grudges, a shared kill forges camaraderie, a
first lesson forges gratitude), so each further event — a **shared meal**, a **broken promise** — is
just one more `nudge_affinity` call,
and `trust`
appends as a second `Relation` field the day its own event lands. The derived `bond_tier` above
already names the ladder those events climb, and **`decay_bonds`** already lets cold ties fade toward
neutral (a grudge cools too) — the affinity twin of the standing leak, every kBondDecayPeriod ticks
one step toward 0 — while a **latched** Partner or Nemesis (`bond_latched`) holds fast, so only the
deepest ties last. Then the social `perceive` layer reads affinity *and*
standing to choose stances (befriend / protect / exploit).

## Key files

- `engine/sim/components.hpp` — `Relation` (one directed tie) and `Relationships` (the
  lazy sparse edge list), placed beside `BehaviorLedger`/`standing`.
- `engine/sim/systems.hpp` / `systems.cpp` — `nudge_affinity` (the single write-point, the
  `record_deed` twin), `affinity_toward` (its read-side counterpart), and `allies_of` (the incoming
  count the HUD shows); the bond formed at
  `handle_deaths`' rescue branch, the victim grudge **and** the witness-grudge spread (`kWitnessGrudge`)
  at `perform_attack`'s cruel-strike branch, and `bond_witnesses` (camaraderie) at `perform_attack`'s
  AND `advance_projectiles`' killing-blow branches; the bond-pull rung in
  `steer_npcs` (below the hero-rally), the **defend** rung (charge to a bonded friend a creature
  threatens, just below the downed-rescue), the grudge-veto in both rescue paths, the
  affinity-discounted rescue reach on the `steer_npcs` rescue rung, and — at a death — the **grief**
  (a fallen Friend+ drifts a survivor's bravery down + `Panicked`) **and vindication** (a slain
  Nemesis drifts it up, `kVindicationDrift`) drifts in `handle_deaths`.
- `tests/sim/test_simulation.cpp` — the write-point (find-or-update + clamp), the bond
  wired at a rescue, the bond-pull steering toward a friend (and the range gate), the
  stale-handle guard, the grudge (a cruel strike resents the striker; a grudge-holder
  won't cross to rescue nor haul up the resented), the graded rescue reach (a bond extends the
  trek beyond the base radius, a mild dislike shortens it — isolated from the bond-pull rung), the
  defend charge (outranking hunger, isolated from bond-follow), `allies_of` (the incoming-bond
  count, floor + direction + self-exclusion), and the death-drift pair — **grief** (a fallen friend
  drifts a mourner's bravery down + panics it; a mere acquaintance is untouched) and its mirror
  **vindication** (a slain Nemesis lifts a survivor's bravery, no panic; a passing rival is untouched).

## Go deeper

- [Morality](morality.md) — `standing` and `record_deed`, the seed this one mirrors.
- [NPC behaviour](npc-behaviour.md) — the steer ladder the bond-pull joins, and the
  `Personality` axes (`loyalty` is the one this unblocks).
