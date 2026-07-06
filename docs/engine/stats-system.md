# The stats system

## What it is

The foundation for the numbers that describe a player or an NPC — health and
stamina today, with hunger, attributes, and skills as the game grows. It is
deliberately small: two data types and a handful of small systems, built on the
engine skeleton's ECS. It is the worked example of
[extending the skeleton](skeleton/extending.md) applied to a real feature.

- **`Vital`** — a reusable "bar" stat: `current`, `max`, `regen_per_second`.
- **`Stats`** — one component per entity that holds its vitals (its character sheet).
- **`regenerate_vitals`** — a system that recovers each *passive* vital (health) toward its cap.
- **`update_stamina`** — a system that spends stamina while moving and restores it while still.
- **`DamagePlayer`** — a command that subtracts from a player's health, applied
  through the funnel (the `H` key in the demo).
- **`handle_deaths`** — a system that respawns a player whose health reaches zero.
- **`Hazard` + `resolve_contacts`** — a component marking dangerous entities and a
  system that damages a player who touches one and then destroys it (the drifting
  motes are consumed on contact).

Honest scope: `health` and `stamina` exist. Health regenerates, drops from a debug
key and from touching a hazard, and reaching zero respawns the player. Stamina is
spent by moving and recovers by resting; running it dry slows the player to a
crawl. Death is respawn, not the permadeath the game's NPCs will use.

## Why it's built this way

Two design choices are worth understanding, because they shape how you extend it.

**`Vital` is one shared type, not a struct per stat.** Health, stamina, hunger,
and mana are all "a number that fills toward a cap over time." Giving them one
type means a new vital is a field, not a copy-pasted struct plus its own system.

**`Stats` is one bundled component, not a component per stat.** An entity has a
single `Stats` holding all its vitals, so the code that manages a character —
today the debug panel, later an NPC-management screen — reads *one* place, and
`regenerate_vitals` iterates `view<Stats>()` once.

!!! info "The tradeoff, stated plainly"
    A bundled `Stats` can't be filtered by individual stat — you can't ask the
    ECS for "everything with stamina" the way you could with a separate
    `Stamina` component. For a colony sim where players and NPCs share a stat
    set, that query isn't needed, so bundling wins. If it ever is needed, split
    that stat into its own component then — not before.

## How it works

Each fixed tick, `World::step()` calls `regenerate_vitals` alongside the other
systems. It runs over exactly the entities that have a `Stats` component (the
player here, not the drifting motes) and nudges each vital toward its `max`:

```mermaid
flowchart LR
  step[World::step] --> sys[regenerate_vitals]
  sys -->|view&lt;Stats&gt;| each[each entity with Stats]
  each --> rec[recover: current += regen_per_second * dt, clamp to max]
```

The player is created in `build_scene` with `emplace<Stats>(player, Vital{70,
100, 8})` — spawned a little worn so the regeneration is visible — and the debug
panel reads it back with `try_get<Stats>` (null-safe) to draw the health bar.

Health also *decreases* through the funnel: a `DamagePlayer` command (the `H`
key) is handled in `apply_command`, which subtracts from the matching player's
health and clamps it at zero. Because that runs on the server through the funnel,
a client can't fake damage — it can only ask for it.

When health reaches zero, `handle_deaths` respawns the player at full health and
the spawn point. It runs **before** `regenerate_vitals` in `step()` on purpose:
the other order would let the same tick's regen nudge a 0-health entity back
above zero, and it would never die. The order of the system calls in `step()` is
the definition of the tick — here it is load-bearing.

Health also drops from *gameplay*, and that shows the other half of the rule.
Touching a `Hazard` (a drifting mote) hurts the player through the
`resolve_contacts` **system**, which changes health directly — no command — and
then destroys the mote. It gathers the hazards to remove and destroys them
*after* the loop: calling `registry.destroy` while iterating a view invalidates
it (a classic ECS bug), so collect-then-destroy is the safe pattern — the same
one the permadeath NPCs will use.

!!! info "Command or system? The distinction that matters"
    A **command** carries intent from *outside* the simulation — a player pressing
    `H`, later a network client — so it is validated at the funnel before it can
    do anything. A **system** is the simulation's own rule playing out each tick;
    it already runs on the authoritative server, so it acts directly. Contact
    damage is a rule of the world, so it is a system, not a command.

Stamina shows that not every vital regenerates the same way. Health only ever
ticks back up, so it lives in `regenerate_vitals`. Stamina is *spent*: the
`update_stamina` system drains it while the player is moving (any entity with
`Stats` and a non-zero `Velocity`) and lets it recover only while still. That is
why stamina earns its own system instead of a line in `regenerate_vitals` — a
one-way "always recover" rule can't express "costs something to use."

The payoff is a gameplay coupling: `MovePlayer` reads stamina when it sets the
player's velocity, so an exhausted player (empty bar) moves at 40% speed — a
crawl, not a full stop, so you can always limp to safety. This is a command whose
*effect depends on simulation state*, which is exactly why the funnel resolves it
on the server rather than trusting the client's requested speed.

## Extending it

Every one of these is a small, contained change — the system is made to grow
this way:

| To add… | You touch… |
|---|---|
| **A passive vital** (mana) | a `Vital mana;` field in `Stats`; one `recover(s.mana, dt);` line in `regenerate_vitals`; a bar in the panel |
| **A spent vital** (hunger) | a `Vital` field in `Stats` plus its own small system for *when* it drains — the shape `update_stamina` follows |
| **Attributes** (strength, agility) | new fields in `Stats`; a system that reads them where they matter (e.g. movement) |
| **Skills that level with use** | a `Skill {level, xp}` type and a set of them in `Stats`; a system that grants xp on activity |
| **A new hazard or weapon** | a component marking it (like `Hazard`) plus a system that applies its effect (like `resolve_contacts`) |

## Where it goes next

Damage now comes from both a command (the `H` key) and gameplay (touching a
hazard). Further sources are the same two shapes: a projectile or trap is another
`Hazard`-like component handled by a system, and a healing item would be its own
system nudging `current` up.

Death currently means respawn, which is right for the player but not for NPCs —
the game's core rule is **permadeath**. When NPCs arrive, `handle_deaths` grows a
branch that *destroys* a dead NPC's entity instead of resetting it, using the
exact collect-then-destroy pattern `resolve_contacts` already uses to consume the
motes.

Beyond that, the game's design calls for skills that level with activity for both
players and NPCs (see the master plan). Those slot into `Stats` as new fields with
their own systems, exactly like `Vital` did.

## Key files

- `engine/sim/components.hpp` — `Vital`, `Stats` (health + stamina), and `Hazard`.
- `engine/sim/systems.hpp` / `systems.cpp` — `regenerate_vitals`, `update_stamina`, `handle_deaths`, and `resolve_contacts`.
- `engine/sim/world.cpp` — the player's `Stats`, the motes' `Hazard`, the stamina-aware `MovePlayer`, and the lines scheduling the systems in `step()`.
- `game/app/main.cpp` — the health and stamina bars in the debug panel.
- `tests/sim/test_simulation.cpp` — the heal, damage, death, contact, and stamina tests.

## Go deeper

- [Entities and components](skeleton/ecs.md) — why stats are data, not a subclass.
- [The tick and the systems](skeleton/tick-and-systems.md) — how `regenerate_vitals` is scheduled.
- [The command funnel](skeleton/command-funnel.md) — the path a damage command would take.
- [Extending the skeleton](skeleton/extending.md) — the general recipes this system is built from.
