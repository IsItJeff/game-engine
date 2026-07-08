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

// Steer NPCs: each NPC looks for the nearest Hazard within its senses and, if it
// finds one, sets its Velocity to flee directly away from it (otherwise it keeps
// drifting). The first taste of NPC behaviour — perception (find the threat) then
// action (set the velocity). MUST run before integrate_motion, which is what
// turns the velocity it sets into actual movement this tick.
void steer_npcs(entt::registry& reg);

// Steer creatures: each Enemy sets its Velocity to home straight in on the player —
// the hostile mirror of steer_npcs (which flees). Like steer_npcs, MUST run before
// integrate_motion so the chosen velocity turns into movement this tick.
void chase_player(entt::registry& reg);

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

// Resolve creature contact: an Enemy overlapping a player, once its attack cooldown
// is up, deals a `attack_damage` blow softened by the player's VIT (ratio mitigation)
// and trains the player's Toughness (via train_on_damage). Unlike a mote it is NOT
// consumed — it keeps chasing and swinging on its cooldown. `dt` advances the
// cooldown. A SYSTEM, not a command (collision is the sim's own rule).
void resolve_creature_contacts(entt::registry& reg, float dt);

// Train Toughness on a hit: surviving `damage` grows the victim's Toughness skill
// and its main attribute Endurance (a VIT skill — you toughen by enduring hardship),
// which advance_progression turns into a bigger HP pool. The single place damage
// feeds progression, so every damage source — contact now, weapons later — trains
// it the same way just by calling this. A no-op for entities without Skills.
void train_on_damage(entt::registry& reg, entt::entity victim, float damage);

// Resolve one melee swing for `attacker`: find the nearest attackable target (a
// Hazard mote OR a hostile Enemy) within reach (reach grows with Strength), train
// Striking -> Strength for a connecting strike, and act by target kind:
//   - a MOTE is fragile: it's returned for the caller to destroy (instant kill).
//   - an ENEMY takes STR-vs-VIT damage to its HP (base + Strength, softened by the
//     enemy's VIT via ratio mitigation); it is NOT returned — it dies later through
//     handle_deaths when HP hits 0, so it survives weak hits and takes several.
// Returns the mote to destroy, or entt::null (missed, or hit an enemy that lived).
// Callers collect-then-destroy so no view is invalidated mid-iteration. Shared by the
// player's Attack command and npc_attack. A no-op without Transform+Attributes+Skills.
entt::entity perform_attack(entt::registry& reg, entt::entity attacker);

// NPCs fight back: every NPC with a hazard in reach strikes it (via perform_attack),
// training Striking -> Strength just as the player does — so NPCs build Strength too,
// not only Endurance. Complements steer_npcs (flee): a threat that closes to reach
// gets struck rather than merely dodged. MUST run after integrate_motion (positions
// current) and before resolve_contacts (so a struck mote can't also land its hit).
void npc_attack(entt::registry& reg);

// Advance progression, the whole "learn by doing" chain in one pass over every
// entity with Skills + Attributes + Stats + Velocity + CharacterLevel: activity
// earns XP for the skill it trains, that skill's main attribute, AND a fraction to
// the global Character Level; full XP bars level each up; and derived stats (max
// health & stamina) grow from the attribute's level, scaled a little by the
// Character Level's veteran multiplier. Runs on the player and NPCs alike. No `dt`
// — the timestep is fixed, so XP is a constant per-tick amount. (The XP curve
// `xp_to_next` and the effect curve `power` live in progression/curve.hpp.)
void advance_progression(entt::registry& reg);

// React to death: a player at 0 health respawns at `respawn_point` with full health;
// an NPC or creature at 0 health is destroyed — permadeath, the game's core rule. A
// slain CREATURE also drops a health Pickup where it fell (loot for the win). MUST
// run before regenerate_vitals, or a just-killed entity gets healed back above 0 the
// same tick and never dies.
void handle_deaths(entt::registry& reg, Vec2 respawn_point);

// Collect loot and age it: each Pickup's `lifetime` counts down by `dt`, and one a
// player overlaps restores its `heal` health (capped) AND permanently raises max HP by
// its `bonus_max_hp`, then is consumed; an orb whose lifetime runs out fades away
// uncollected (so drops from far-off kills don't pile up forever). Collect-then-destroy.
void collect_pickups(entt::registry& reg, float dt);

}  // namespace eng::sim
