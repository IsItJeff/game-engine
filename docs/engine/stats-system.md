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

Honest scope: only `health` exists so far. It regenerates over time, and it can
be damaged through a command — but nothing in *gameplay* (a weapon, a trap)
produces that damage yet; only a debug keypress does.

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

## Extending it

Every one of these is a small, contained change — the system is made to grow
this way:

| To add… | You touch… |
|---|---|
| **A stamina vital** | a `Vital stamina;` field in `Stats`; one `recover(s.stamina, dt);` line in `regenerate_vitals`; a bar in the panel |
| **Attributes** (strength, agility) | new fields in `Stats`; a system that reads them where they matter (e.g. movement) |
| **Skills that level with use** | a `Skill {level, xp}` type and a set of them in `Stats`; a system that grants xp on activity |
| **Damage from actual gameplay** | a weapon, trap, or collision that *emits* a damage command instead of a keypress |

## Where it goes next

Damage exists, but only from a debug key. The next real step is **damage from
gameplay** — a collision, a projectile, or a trap that emits a `DamagePlayer`
command the same way the `H` key does. Nothing about `Stats` changes; a new bit
of game logic just becomes another sender into the funnel.

Beyond that, the game's design calls for skills that level with activity for both
players and NPCs (see the master plan). Those slot into `Stats` as new fields with
their own systems, exactly like `Vital` did — and a health that can reach zero is
what a future "death" system will watch for.

## Key files

- `engine/sim/components.hpp` — `Vital` and `Stats`.
- `engine/sim/systems.hpp` / `systems.cpp` — `regenerate_vitals` and its `recover` helper.
- `engine/sim/world.cpp` — the player's `Stats`, and the one line scheduling the system in `step()`.
- `game/app/main.cpp` — the health bar in the debug panel.
- `tests/sim/test_simulation.cpp` — the heal-and-cap test.

## Go deeper

- [Entities and components](skeleton/ecs.md) — why stats are data, not a subclass.
- [The tick and the systems](skeleton/tick-and-systems.md) — how `regenerate_vitals` is scheduled.
- [The command funnel](skeleton/command-funnel.md) — the path a damage command would take.
- [Extending the skeleton](skeleton/extending.md) — the general recipes this system is built from.
