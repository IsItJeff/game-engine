# Morality: the behaviour ledger

## What it is

The first trace of a character's **moral history**. Personality is who a colonist
*is* (fixed, innate — see [NPC behaviour](npc-behaviour.md)); morality is what they
have *done* (earned, mutable). It starts as one component and one recorded deed:

- **`BehaviorLedger`** — a per-character tally of six deed **dimensions** (Violence,
  Honesty, Loyalty, Charity, Cruelty, Valor), each an accumulator.
- **`record_deed(actor, kind, mag)`** — the single write-point every deed funnels
  through. It lazily gives the actor a ledger the first time they act.
- **`standing(ledger)`** — one derived scalar the six dimensions collapse to:
  positive is heroic repute, negative is villainous.

So far **two** deeds are wired — the two hero signals, both from events the game
already has: completing a **rescue** credits the rescuer with **Charity**, and landing
the **killing blow on a hostile** credits the attacker with **Valor**. The other four
dimensions exist but wait for their deeds.

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

Two deeds are wired. The first lives in `handle_deaths`: the rescue loop already finds
the ally who reaches a downed player — it now keeps that **entity** (not just a yes/no)
and, after hauling them up, credits it `Deed::Charity`. The rescued player earns
nothing; the *rescuer* does. The second lives in `perform_attack`: the blow that takes
a hostile creature's HP across zero credits the **attacker** with `Deed::Valor` — scored
on the alive→dead *transition*, so only the one fatal strike counts, never a second
swing on the already-dead foe.

!!! info "Two deeds, both heroic"
    Rescue → Charity and hostile-kill → Valor are the two things a hero *does*, and each
    is **one `record_deed` call** at an event the game already had — exactly what shipping
    the seam and the full schema first bought. The villain signals wait on their events:
    cutting down a *peaceful* colonist (Cruelty / unjust Violence) needs both a reason to
    harm an ally and the "justness" rule, both deferred.

## What to expect

You can now **see** it: a character's dot **grows** with its positive `standing`, so a
colonist that keeps rescuing allies and felling monsters visibly swells into a figure
of repute. That is the first *reader* of `standing` — a presentation-only one (the
renderer, never the sim, so determinism holds). **Size** is the free channel: colour
already carries bravery, brightness carries health. Renown only rises so far (both
deeds are positive); negative standing gets its own cue when villain deeds land. The
*deeper* reader — NPCs that **act** on a character's standing (befriend, protect, fear)
— is a later ring. Under the hood it is pinned by tests: a rescuer's ledger gains
Charity, a monster-slayer's gains Valor; a bystander, an unrescued timer-expiry
respawn, and a chip that doesn't kill all record nothing.

## The tradeoffs

- **`standing` has no *gameplay* reader yet** — only a presentation one (the renderer
  sizes the dot by it). The signed formula still ships in full because it is the design's
  load-bearing claim (ledger → one derived scalar) and locking it now makes the next,
  oppositely-signed deed purely additive; the tests pin its negative-weight arithmetic
  before any negative deed exists.
- **Magnitudes are hardcoded.** `kRescueCharity` is a constant; JSON-authored deed
  weights are a modding-milestone job. A tuning knob, not a design gap.
- **`int32`, not fixed-point.** The ×5 integer trick sidesteps the codebase-wide
  fixed-point migration (a later ring) while staying bit-identical.

## Where it goes next

The write-point is the whole point: more deeds (Cruelty, Violence, Honesty, Loyalty)
each become one `record_deed` call at their event, exactly as Valor just did. Then
`standing` grows a **reader** — the social `perceive` layer that turns a character's *believed* standing
into how others treat them (befriend, protect, fear, exploit). On top of that sit
**titles** ("the Butcher", "Dragonslayer") and hero/villain labels: derived queries
over the same ledger, never stored slots. A **leaky decay** (redemption and corruption
for free) lands when deeds start to matter over long play.

## Key files

- `engine/sim/components.hpp` — `Deed` (the six dimensions), `BehaviorLedger` (the
  earned counterpart of `Personality`), the pure `standing` function, and `renown_scale`
  (the presentation twin of `personality_tint`).
- `engine/sim/systems.hpp` / `systems.cpp` — `record_deed` (the single write-point);
  the Charity credit in `handle_deaths`' rescue branch, and the Valor credit in
  `perform_attack`'s killing-blow branch.
- `game/app/main.cpp` — `draw_entities` scales a dot's radius by
  `renown_scale(standing(...))`, so renown reads on screen.
- `tests/sim/test_simulation.cpp` — the funnel + signed formula, the wired deeds with
  player==NPC parity, the lazy no-deed-no-ledger path, and `renown_scale`.

## Go deeper

- [NPC behaviour](npc-behaviour.md) — `Personality` (who a character *is*), the innate
  twin of this earned ledger; the rescue rung that morality now records.
- [Combat](combat.md) — the Downed death beat that a rescue interrupts.
