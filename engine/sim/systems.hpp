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

// React to death: a player-controlled entity whose health hit 0 respawns at
// `respawn_point` with full health. MUST run before regenerate_vitals, or a
// just-killed entity gets healed back above 0 the same tick and never dies.
// (NPCs will instead be destroyed — permadeath — which is a later step.)
void handle_deaths(entt::registry& reg, Vec2 respawn_point);

}  // namespace eng::sim
