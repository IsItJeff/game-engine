#pragma once

#include <entt/entity/registry.hpp>

#include "engine/core/math.hpp"

// Systems: the BEHAVIOUR of the simulation.
//
// A system is just a free function that runs over every entity holding a given
// set of components (code-design-rules rule 11: "systems are plain functions in
// one explicit ordered schedule — no ISystem base class"). There is no clever
// framework here on purpose: World::step() calls these in a fixed, readable
// order, and that call order IS the definition of how a tick behaves. To add
// behaviour, you write a new free function and add one line to step().
//
// Every system takes the fixed per-tick timestep as `dt` — never a variable
// frame time. That is what "fixed timestep" buys: the same inputs always
// produce the same motion, on every machine and every frame rate.

namespace eng::sim {

// Copy each renderable entity's current Transform into its PrevTransform, so the
// renderer can smoothly interpolate between the last two ticks (see the fixed-
// timestep explanation in simulation.hpp). Runs first, before anything moves.
void snapshot_previous(entt::registry& reg);

// Move every entity with a Transform and Velocity: position += velocity * dt.
// This is Euler integration — the simplest way to turn a velocity into motion.
void integrate_motion(entt::registry& reg, float dt);

// Wrap positions around the field edges (toroidal space) so nothing drifts out
// of view. Keeps the demo self-contained; a real game uses a camera instead.
void wrap_bounds(entt::registry& reg, Vec2 field_size);

// Recover every entity's vitals toward their max at each vital's regen rate.
// Runs over exactly the entities that have a Stats component (players, NPCs).
void regenerate_vitals(entt::registry& reg, float dt);

// Update stamina from movement: any entity with Stats and a non-zero Velocity
// spends stamina; one standing still recovers it. (Motes have Velocity but no
// Stats, so they're untouched.) This makes movement cost something — the
// MovePlayer funnel reads the result and slows an exhausted player to a crawl.
void update_stamina(entt::registry& reg, float dt);

// Resolve contact damage: any entity with Stats (the player or an NPC) that
// overlaps a Hazard takes its `damage`, and the hazard is then consumed
// (destroyed). A SYSTEM, not a command — collision is the sim's own rule, so it
// changes state directly (the funnel is only for input from outside the sim).
// Note it destroys entities only AFTER iterating; destroying during iteration
// invalidates the view (a classic bug).
void resolve_contacts(entt::registry& reg);

// React to death, two ways: a player at 0 health respawns at `respawn_point` with
// full health; an NPC at 0 health is destroyed instead — permadeath, the game's
// core rule. MUST run before regenerate_vitals, or a just-killed entity gets
// healed back above 0 the same tick and never dies.
void handle_deaths(entt::registry& reg, Vec2 respawn_point);

}  // namespace eng::sim
