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

So far exactly **one** deed is wired: completing a **rescue** credits the rescuer
with **Charity**. The other five dimensions exist but wait for their deeds.

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

The one wired deed lives in `handle_deaths`: the rescue loop already finds the ally
who reaches a downed player — it now keeps that **entity** (not just a yes/no) and,
after hauling them up, credits it `Deed::Charity`. The rescued player earns nothing;
the *rescuer* does.

!!! info "One deed, on purpose"
    Only rescue → Charity is wired. Killing a hostile (Valor) and cutting down a
    peaceful colonist (Cruelty / unjust Violence) are the obvious next deeds — each is
    **one more `record_deed` call** into the same write-point and an already-defined
    dimension. Shipping the seam and the full schema first is what makes them one-liners.

## What to expect

Nothing visible yet — `standing` has no reader in the sim (the social layer that will
*react* to it is a later ring). Today it is exercised by tests: a rescuer's ledger
gains Charity; a bystander and an unrescued timer-expiry respawn record nothing. This
is groundwork you feel later, when NPCs start treating a hero and a butcher differently.

## The tradeoffs

- **`standing` has no gameplay reader this slice.** It ships anyway because it is the
  design's load-bearing claim (ledger → one derived scalar) and locking the *signed*
  formula now makes the next, oppositely-signed deed purely additive. It is not dead:
  the tests read it, pinning the negative-weight arithmetic before any negative deed.
- **Magnitudes are hardcoded.** `kRescueCharity` is a constant; JSON-authored deed
  weights are a modding-milestone job. A tuning knob, not a design gap.
- **`int32`, not fixed-point.** The ×5 integer trick sidesteps the codebase-wide
  fixed-point migration (a later ring) while staying bit-identical.

## Where it goes next

The write-point is the whole point: more deeds (Valor, Cruelty, Violence, Honesty,
Loyalty) each become one `record_deed` call at their event. Then `standing` grows a
**reader** — the social `perceive` layer that turns a character's *believed* standing
into how others treat them (befriend, protect, fear, exploit). On top of that sit
**titles** ("the Butcher", "Dragonslayer") and hero/villain labels: derived queries
over the same ledger, never stored slots. A **leaky decay** (redemption and corruption
for free) lands when deeds start to matter over long play.

## Key files

- `engine/sim/components.hpp` — `Deed` (the six dimensions), `BehaviorLedger` (the
  earned counterpart of `Personality`), and the pure `standing` function.
- `engine/sim/systems.hpp` / `systems.cpp` — `record_deed` (the single write-point);
  the Charity credit in `handle_deaths`' rescue branch.
- `tests/sim/test_simulation.cpp` — the funnel + signed formula, the wired rescue deed
  with player==NPC parity, and the lazy no-deed-no-ledger path.

## Go deeper

- [NPC behaviour](npc-behaviour.md) — `Personality` (who a character *is*), the innate
  twin of this earned ledger; the rescue rung that morality now records.
- [Combat](combat.md) — the Downed death beat that a rescue interrupts.
