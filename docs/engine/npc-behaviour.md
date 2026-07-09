# NPC behaviour: the first steering

## What it is

The first thing an NPC does on its own. Until now NPCs were data that drifted;
the `steer_npcs` system gives each one a *decision* every tick. It started as one
choice — flee the nearest hazard — and has grown into a small **priority ladder** of
wants, still perception-then-action, still hard-coded leaves:

1. **Flee** the nearest `Hazard` (fear beats everything).
2. **Rescue** — run to the nearest **Downed** ally to haul them up (the first want about
   another *person*, added with the [Downed death beat](combat.md)).
3. **Forage** — if hungry, head for the nearest food orb (a `Pickup`).
4. **Arm up** — if unarmed, head for the nearest dropped `Weapon` (`npc_equip` wields it on
   reach), so colonists loot the battlefield and fight harder — the player==NPC gear parity.
5. Otherwise **drift**.

Every steer speed is scaled by `(1 - move_penalty)` when the NPC is **armed**, so a wielded
weapon's heft slows an NPC exactly as it slows the player — the item's bane bites both.

It is the seed of the engine's NPC AI (the master plan's sensors, blackboard, and
behaviour trees) — each rung is exactly the kind of leaf a behaviour tree will one day
select among.

- **`steer_npcs`** — a system that, for each NPC, senses the nearest target of each kind
  in priority order (hazard → downed ally → food) and sets its velocity accordingly.

## Why it matters

"NPCs are people, not units" is the game's first pillar. A unit follows a fixed
script; a person *reacts* to its world. This is the smallest honest version of
reacting: an NPC that notices danger and moves away from it. Everything richer —
seeking food, following orders, fighting — is the same shape with a different
decision at the centre.

## How it works

Each tick, **before anything moves**, `steer_npcs` runs over every NPC:

```mermaid
flowchart TD
  npc[each NPC] --> haz{nearest Hazard<br/>in range?}
  haz -->|yes| flee[flee: velocity straight away]
  haz -->|no| dwn{nearest Downed<br/>ally in range?}
  dwn -->|yes| rescue[rescue: velocity toward them]
  dwn -->|no| hun{hungry AND<br/>food orb in range?}
  hun -->|yes| forage[forage: velocity toward the orb]
  hun -->|no| arm{unarmed AND<br/>weapon in range?}
  arm -->|yes| seek[arm up: velocity toward the weapon]
  arm -->|no| drift[drift: leave velocity alone]
```

The first matching want wins and the NPC commits to it that tick (a `continue`), so a
fleeing NPC never also forages, and a rescuer drops its meal to save someone. One
load-bearing detail in the rescue rung: an NPC *already* within revive range holds
position rather than steering, so it doesn't nudge itself back out before
`handle_deaths` (later the same tick) hauls the ally up. And every steer speed is scaled
down while the NPC is armed — the weapon's heft slows it, so the buff it wields is paid
for exactly as the player pays (the item's bane bites both).

Two details carry the whole idea:

It is a **system, not a command** — the NPC's own behaviour, so it changes
velocity directly. The command funnel is only for intent from *outside* the sim
(the player's keys, later the network); an NPC's choices are the sim's own rules.

It **must run before `integrate_motion`**, because it sets the velocity that
integration turns into movement *this same tick*. As with death-before-heal, the
order of the calls in `step()` is load-bearing.

The ECS filter does the targeting for free: `view<Npc, Transform, Velocity>` skips
the player (no `Npc`) and the motes (no `Npc`) without a single `if`.

### Personality: bravery shapes the flee distance (the P7 seed)

The flee rung no longer treats every colonist alike. A new **`Personality`** component
carries the design's first **P7** axis — **`bravery`** (an `int8` in `[-100, +100]`) — and
`steer_npcs` reads it to scale the danger sense radius: `kSenseRadius × (1 − bravery/200)`. A
**coward** (−100) senses a hazard from 1.5× as far and **bolts early**; a **brave** colonist
(+100) shrinks its radius to half and **holds** until the hazard is nearly on top of it.
Neutral `0` — or no `Personality` at all — is the base radius exactly, so this is bit-identical
for anyone without a leaning (the player has none, and stays neutral). The opening four NPCs get
a fixed spread (two cowards, two brave) so you can watch the difference from the first frame;
reinforcements roll a random bravery from the spawner's *own* isolated RNG stream.

Bravery reads a **second time** on the rescue rung, which is where it becomes a real character
trait rather than a flee tweak: it scales how far an NPC will *commit* to saving a downed ally.
A **brave** colonist crosses the field to reach one (`kRescueRadius × (1 + bravery/200)` grows);
a **coward** won't make the risky trek and only helps an ally close by (the radius shrinks). The
sign is deliberately **opposite** the flee radius — braver *shrinks* the flee radius (holds
ground) but *grows* the rescue radius (commits further) — so on both rungs "braver" is the
courageous choice. That two-behaviour payoff is what makes `Personality` earn its keep on its
second use, not a one-off.

A **second axis, `greed`**, proves `Personality` bends to more than one *shape* of decision. It
reads the **forage** rung — not a radius but a **need threshold**: the effective "am I hungry?"
fraction is `kHungerSeekFraction × (1 + greed/200)`, so a **greedy** colonist (+greed) breaks off
to hoard an orb while still well-fed, and a **selfless** one (−greed) leaves food for others and
only forages when genuinely hungry. Because it scales a threshold rather than a distance, it
isn't a bravery reskin — it demonstrates the axes generalise across different mechanisms. The
opening four NPCs get a fixed bravery/greed spread (each a distinct combo — a cowardly hoarder, a
brave altruist), so the personalities read from frame one. (Reinforcements jitter bravery only
for now; a greed jitter is a follow-up.)

This is the smallest honest seed of the master plan's **personality/morality** layer: axes that
real behaviours *read* and that change visible motion. The remaining four (compassion, industry,
loyalty, sociability) append to the same struct as more behaviours grow to read them.

And you can now *see* it: the renderer tints each colonist's dot by its **bravery** — the brave
warm toward yellow, the cowardly cool toward teal, green left untouched so a tinted NPC stays
green-dominant (never confused for an enemy or the player). It's a pure `personality_tint`
helper (the twin of `wounded_brightness`), a colour multiplier the sim never reads — so the
brave/coward spread `build_scene` seeds reads at a glance. (The warm/cool palette is a tuning
knob, eyeballed in the live renderer; greed is left untinted for now — a second-axis cue wants a
channel this one doesn't already use.)

!!! info "Greedy and memoryless — on purpose"
    It flees the *single nearest* threat, with no memory. An NPC can dodge one
    mote straight into another. That is fine: real steering behaviours (Reynolds)
    blend many influences, and this is deliberately the one-decision version. Write
    the concrete thing first; add the blend on the second real need.

## What to expect

Spawn a mote (`Space`) near the green dots in the demo and watch them scatter — fleeing
still buys time, not immortality (some get cornered and die, permadeath). But there's more
life to watch now: a hungry colonist peels off to grab a dropped health orb; if *you* go
down, a nearby colonist breaks off and **runs to revive you** before your respawn timer
fires; and a slain **brute's dropped weapon** gets snapped up by whichever unarmed colonist
(or you) reaches it first — after which that NPC hits harder but moves a little slower. Four
wants, one ladder, chosen fresh each tick.

## The tradeoffs

- **O(NPCs × hazards) per tick.** Fine for a handful; a crowd needs a spatial
  grid — the same upgrade `resolve_contacts` wants, done once for both.
- **Mostly constants, not components.** Flee speed is still `constexpr`; the sense
  radius has now taken the first step off that — it's personalised per-NPC by
  `Personality::bravery` (see above), the pattern a scout-that-sees-farther would extend.
  The rest stay constants until a behaviour actually needs to vary them.

## Where it goes next

This is one hard-coded decision. The game needs many, chosen by situation — the
master plan's **behaviour trees** (C++ structural nodes, Luau leaves). `steer_npcs`
becomes one *leaf* ("flee threat") among many ("gather", "build", "guard"),
selected by a tree the NPC walks each tick. The perception half grows into a
**blackboard** — what an NPC knows — fed by sensors. The shape you see here, look
then act, is what stays.

## Key files

- `engine/sim/systems.hpp` / `systems.cpp` — `steer_npcs` (the flee / rescue / forage / arm-up ladder, speeds scaled by the equip bane; `Personality::bravery` scales the flee AND rescue radii, `Personality::greed` scales the forage threshold); `handle_deaths` does the revive at `kReviveDistance`; `npc_equip` + the shared `equip_nearest_gear` do the wield-on-reach.
- `engine/sim/components.hpp` — `Personality` (the P7 seed; `bravery` + `greed` axes); `engine/sim/world.cpp` — `make_npc` sets it (fixed spread in `build_scene`, bravery jittered for reinforcements).
- `engine/sim/world.cpp` — the `steer_npcs` line in `step()` (before `integrate_motion`) and `npc_equip` (after it).
- `tests/sim/test_simulation.cpp` — flee / forage / rescue / revive-in-place, and steer-to-weapon / NPC-arms-itself / armed-NPC-flees-slower (the equip bane parity).

## Go deeper

- [The tick and the systems](skeleton/tick-and-systems.md) — how `steer_npcs` is scheduled and why order matters.
- [Entities and components](skeleton/ecs.md) — why an NPC is a component set, and how the view targets them.
- [The stats system](stats-system.md) — the permadeath that fleeing tries to postpone.
