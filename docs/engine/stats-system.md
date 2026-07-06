# The stats system

## What it is

The foundation for the numbers that describe a player or an NPC — health today,
and stamina, hunger, attributes, and skills as the game grows. It is deliberately
small: two data types and one system, built on the engine skeleton's ECS. It is
the worked example of [extending the skeleton](skeleton/extending.md) applied to
a real feature.

- **`Vital`** — a reusable "bar" stat: `current`, `max`, `regen_per_second`.
- **`Stats`** — one component per entity that holds its vitals (its character sheet).
- **`regenerate_vitals`** — a system that recovers each vital toward its cap.
- **`DamagePlayer`** — a command that subtracts from a player's health, applied
  through the funnel (the `H` key in the demo).
- **`handle_deaths`** — a system that respawns a player whose health reaches zero.
- **`Hazard` + `damage_on_contact`** — a component marking dangerous entities and
  a system that hurts players who touch them (the drifting motes).

Honest scope: only `health` exists so far. It regenerates, it drops both when a
debug key fires a damage command and when a drifting hazard touches the player,
and reaching zero respawns the player. Death is respawn, not the permadeath the
game's NPCs will use.

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
`damage_on_contact` **system**, which changes health directly — no command.

!!! info "Command or system? The distinction that matters"
    A **command** carries intent from *outside* the simulation — a player pressing
    `H`, later a network client — so it is validated at the funnel before it can
    do anything. A **system** is the simulation's own rule playing out each tick;
    it already runs on the authoritative server, so it acts directly. Contact
    damage is a rule of the world, so it is a system, not a command.

## Extending it

Every one of these is a small, contained change — the system is made to grow
this way:

| To add… | You touch… |
|---|---|
| **A stamina vital** | a `Vital stamina;` field in `Stats`; one `recover(s.stamina, dt);` line in `regenerate_vitals`; a bar in the panel |
| **Attributes** (strength, agility) | new fields in `Stats`; a system that reads them where they matter (e.g. movement) |
| **Skills that level with use** | a `Skill {level, xp}` type and a set of them in `Stats`; a system that grants xp on activity |
| **A new hazard or weapon** | a component marking it (like `Hazard`) plus a system that applies its effect (like `damage_on_contact`) |

## Where it goes next

Damage now comes from both a command (the `H` key) and gameplay (touching a
hazard). Further sources are the same two shapes: a projectile or trap is another
`Hazard`-like component handled by a system, and a healing item would be its own
system nudging `current` up.

Death currently means respawn, which is right for the player but not for NPCs —
the game's core rule is **permadeath**. When NPCs arrive, `handle_deaths` grows a
branch that *destroys* a dead NPC's entity instead of resetting it (the ECS's
`registry.destroy`, done carefully outside the iteration loop).

Beyond that, the game's design calls for skills that level with activity for both
players and NPCs (see the master plan). Those slot into `Stats` as new fields with
their own systems, exactly like `Vital` did.

## Key files

- `engine/sim/components.hpp` — `Vital`, `Stats`, and `Hazard`.
- `engine/sim/systems.hpp` / `systems.cpp` — `regenerate_vitals`, `handle_deaths`, and `damage_on_contact`.
- `engine/sim/world.cpp` — the player's `Stats`, the motes' `Hazard`, and the lines scheduling the systems in `step()`.
- `game/app/main.cpp` — the health bar in the debug panel.
- `tests/sim/test_simulation.cpp` — the heal, damage, death, and contact tests.

## Go deeper

- [Entities and components](skeleton/ecs.md) — why stats are data, not a subclass.
- [The tick and the systems](skeleton/tick-and-systems.md) — how `regenerate_vitals` is scheduled.
- [The command funnel](skeleton/command-funnel.md) — the path a damage command would take.
- [Extending the skeleton](skeleton/extending.md) — the general recipes this system is built from.
