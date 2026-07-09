# Morality: the behaviour ledger

## What it is

The first trace of a character's **moral history**. Personality is who a colonist
*is* (innate, and only slowly drifting — see [NPC behaviour](npc-behaviour.md) and the
[Drift](#drift-deeds-reshape-character) section below); morality is what they have *done*
(earned, directly accumulated). It starts as one component and one recorded deed:

- **`BehaviorLedger`** — a per-character tally of six deed **dimensions** (Violence,
  Honesty, Loyalty, Charity, Cruelty, Valor), each an accumulator.
- **`record_deed(actor, kind, mag)`** — the single write-point every deed funnels
  through. It lazily gives the actor a ledger the first time they act.
- **`standing(ledger)`** — one derived scalar the six dimensions collapse to:
  positive is heroic repute, negative is villainous.

**Three** deeds are wired now. Two are hero signals: completing a **rescue** credits the
rescuer with **Charity**, and landing the **killing blow on a hostile** credits the attacker
with **Valor**. The third is the first **villain** signal, and the first deed that pushes
`standing` *below zero*: a **player who cuts down a peaceful colonist** earns **Cruelty**.
The remaining dimensions exist but wait for their deeds.

## Why it matters

The design's morality is *emergent*, not assigned: nobody is flagged "hero" or
"villain" — you become one by what you repeatedly do, and the world reads that off
your `standing`. That needs three things to be right from the first line, because
they are painful to retrofit: a **single write-point** (so every future deed is one
call, never new plumbing), a **derived** standing (recomputed, never a stored number
that can drift out of sync with the deeds behind it), and a **locked schema** (the
six dimensions the design names, so later deeds append a *value*, never a *field*).

This seed does all three on a real gameplay event, and closes a loop with
personality: the **compassion** axis already decides how *fast* a colonist sprints to
a fallen ally; finishing that rescue now leaves a permanent **moral trace**.

## How it works

Each deed is one call to `record_deed`, which does a single thing:

```cpp
void record_deed(entt::registry& reg, entt::entity actor, Deed kind, std::int32_t mag) {
  reg.get_or_emplace<BehaviorLedger>(actor).dims[static_cast<std::size_t>(kind)] += mag;
}
```

Two details carry the design:

- **Lazy** — `get_or_emplace` means an entity earns a ledger only on its first deed.
  Anyone who never acts (and the whole world before this existed) has no ledger and
  replays **bit-identically**. Morality costs nothing until it happens.
- **Parity for free** — it takes a bare entity with no player/NPC branch, so a player
  and an NPC accrue deeds through the exact same path. A rescuing NPC and a rescuing
  player earn the identical Charity.

`standing` is a **pure function** of the ledger — it reads no sim state, so it is
trivially testable and can never disagree with the deeds it sums:

```cpp
standing = charity·4 + valor·5 + honesty·3 + loyalty·3 − cruelty·6 − violence·4
```

Those are the design's exact weights (`.8 / 1.0 / .6 / .6 / −1.2 / −.8`) scaled **×5**
— the smallest factor that turns every one into an integer, so **no float enters the
sim** and replay stays bit-identical. The unit is "fifths of a design-point"; a single
rescue is `+4` standing.

Three deeds are wired. The first lives in `handle_deaths`: the rescue loop already finds
the ally who reaches a downed player — it now keeps that **entity** (not just a yes/no)
and, after hauling them up, credits it `Deed::Charity`. The rescued player earns
nothing; the *rescuer* does. The second lives in `perform_attack`: the blow that takes
a hostile creature's HP across zero credits the **attacker** with `Deed::Valor` — scored
on the alive→dead *transition*, so only the one fatal strike counts, never a second
swing on the already-dead foe.

The third also lives in `perform_attack`, on the branch that used to be a plain **whiff**.
When a swing finds *no* hostile in reach, a **player** (only a player) instead strikes the
nearest peaceful colonist for the same STR-vs-VIT damage, and earns `Deed::Cruelty`. Three
gates keep it a **choice, never a slip**: only a player swings this way (NPCs never turn on
the colony — an NPC-villain AI is a later ring), hostiles are always searched *first* and win
the target (so you can reach a colonist only with nothing else to fight), and it must be in
reach. A downed body is excluded — no infamy for kicking a corpse.

!!! info "Two heroes and a villain"
    Rescue → Charity and hostile-kill → Valor are the two things a hero *does*; striking a
    peaceful colonist → Cruelty is the mirror. Each is **one `record_deed` call**, exactly
    what shipping the seam and the full schema first bought — the villain deed appended a
    *value*, never a *field*. Cruelty is weighted **×6** (dearer than any single hero deed),
    so one betrayal drops `standing` to −6: villainy is cheap to commit and expensive to wear.
    The remaining signals (unjust Violence, Honesty, Loyalty) still wait on their events.

## Drift: deeds reshape character

Morality and personality aren't two separate ledgers — a deed does *two* things at the
one `record_deed` write-point. Besides adding to the ledger, it **drifts the actor's
matching `Personality` axis** a small, bounded step: **Valor → bravery** (fighting
monsters hardens you), **Charity → compassion** (hauling up the fallen softens you). This
is the design's *"you are what you do"* / "the war changed him" — a character reshaped by
its own history, not just a fixed dial rolled at birth.

The step is deliberately small (`kDeedDriftStep`, ~50 deeds for a full ±100 swing), so a
handful of deeds shifts you visibly but never into a wholly different person. And it lands
where it can be *seen*: bravery is the **tinted** axis and the one `steer_npcs` reads
twice (flee radius + rescue commitment), so a colonist that keeps fighting warms toward
yellow and starts holding its ground — a character arc read from its deeds alone.

Two guard-rails carry it. It uses `try_get`, **never** `get_or_emplace`: an actor with no
`Personality` (the player, every creature) accrues the deed on its ledger but *stays*
Personality-free, so the bit-identical absent-Personality world is preserved. And only the
two wired deeds drift — the other four axes/deeds wire themselves the day their deeds land.

## What to expect

You can now **see** it both ways. A character's dot **grows** with its positive `standing`,
so a colonist that keeps rescuing allies and felling monsters visibly swells into a figure
of repute — a presentation-only reader (the renderer, never the sim, so determinism holds).
**Size** is the free channel: colour already carries bravery, brightness carries health. And
the HUD prints the player's `standing` *number* and `standing_title`, so a **fall** reads
too: strike your own colonists and the number goes negative and the title flips *Known →
Suspect → Notorious*. (The dot doesn't yet *shrink* below neutral — a negative-renown visual
is the villain twin of the growth cue, still to come.)

And now standing **acts**, not just shows — *both* ways. A player who crosses the *Suspect* line
becomes a **threat colonists flee**; a player who crosses the mirror *Known* line becomes a **hero
they rally to**. `steer_npcs` folds a villain into its top-priority danger rung (turn on the colony
and it recoils from you like a hazard) and a hero into its bottom-priority rally rung (idle
colonists gather around their champion). Villainy repels, heroism attracts — the two faces of one
scalar (see [NPC behaviour](npc-behaviour.md)). The *richer* readers — graded wariness, protecting
the weak, befriending — are a later ring, but the "believed standing changes behaviour" seam is now
real on both sides. Under the hood it is pinned by tests: a rescuer's ledger gains Charity, a
monster-slayer's gains Valor, a player who cuts down a colonist gains Cruelty and goes negative
(and colonists then flee that player, but not a hero or an unproven one); a bystander, an unrescued
timer-expiry respawn, a chip that doesn't kill, and a colonist shielded by a nearby hostile all
record nothing.

## The tradeoffs

- **`standing`'s gameplay readers are still coarse.** There are TWO, mirror images: colonists
  flee a villain and rally to a hero (`steer_npcs`). Both are *binary* at a single threshold — the
  design's graded *perceive* (the pull/push scaling with standing *and* the onlooker's own
  bravery/might, plus richer stances: befriend, protect the weak, exploit) is a later ring. The
  signed formula shipped in full before any of this was needed, so wiring Cruelty, the fear read,
  and the rally read were each one-line-ish additions — the payoff of locking the schema early.
- **Cruelty is player-only.** An NPC can't yet turn on the colony — `perform_attack` gates
  the cruel branch on `PlayerControlled`, because it is shared with `npc_attack` and a
  generic "hit a neighbour when no enemy is near" would make colonists brawl constantly. The
  NPC-villain path (a personality/standing-driven decision to harm) is a social-ring job.
- **Magnitudes are hardcoded.** `kRescueCharity`, `kValorKill`, `kCrueltyStrike` are
  constants; JSON-authored deed weights are a modding-milestone job. A tuning knob, not a
  design gap.
- **`int32`, not fixed-point.** The ×5 integer trick sidesteps the codebase-wide
  fixed-point migration (a later ring) while staying bit-identical.

## Where it goes next

The first **title** is already here: `standing_title(standing)` is a pure query that names
your repute — *Unproven* → *Known* → *Renowned* (and the mirror *Suspect* → *Notorious* for
when villain deeds land), shown in the HUD beside your `standing` number. That is the design's
"titles are derived queries, never stored slots" in miniature; the richer ones (*Master Smith*,
*Dragonslayer* — from build and gear as well as deeds) hang off the same idea.

The write-point is the whole point: the remaining deeds (unjust Violence, Honesty, Loyalty)
each become one `record_deed` call at their event, exactly as Cruelty just did. `standing`'s
first **gameplay** reader has already landed — colonists flee a villain — and it grows from there
into the full social `perceive` layer that turns a character's *believed* standing into how others
treat them (befriend, protect, exploit, not just fear), graded by the onlooker's own nerve rather
than a single threshold. A **leaky decay** (redemption and corruption for free, so a villain can
climb back) lands when deeds start to matter over long play.

## Key files

- `engine/sim/components.hpp` — `Deed` (the six dimensions), `BehaviorLedger` (the
  earned counterpart of `Personality`), the pure `standing` function, `renown_scale`
  (the presentation twin of `personality_tint`), and `standing_title` (the derived title).
- `engine/sim/systems.hpp` / `systems.cpp` — `record_deed` (the single write-point,
  which also **drifts** the actor's matching `Personality` axis); the Charity credit in
  `handle_deaths`' rescue branch, the Valor credit in `perform_attack`'s killing-blow
  branch, and the Cruelty credit in the same function's player-only "no hostile in reach"
  branch (`kCrueltyStrike`).
- `game/app/main.cpp` — `draw_entities` scales a dot's radius by `renown_scale(standing(...))`
  so renown reads on screen, and the debug HUD shows the player's `standing` number and
  `standing_title`.
- `tests/sim/test_simulation.cpp` — the funnel + signed formula, the wired deeds with
  player==NPC parity, the lazy no-deed-no-ledger path, and `renown_scale`.

## Go deeper

- [NPC behaviour](npc-behaviour.md) — `Personality` (who a character *is*), the innate
  twin of this earned ledger; the rescue rung that morality now records.
- [Combat](combat.md) — the Downed death beat that a rescue interrupts.
