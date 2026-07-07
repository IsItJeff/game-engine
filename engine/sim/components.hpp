#pragma once

#include "engine/core/math.hpp"
#include "engine/sim/types.hpp"

// Components: the DATA that entities are made of.
//
// The engine uses an ECS (Entity Component System, via the EnTT library). The
// core idea, and the reason we do NOT use classes like `class Enemy : public
// Actor` (code-design-rules rule 1: "an entity is an ID"):
//
//   - An ENTITY is just an id (a number). It has no data or behaviour itself.
//   - A COMPONENT is a plain struct of data (below). An entity "has" a
//     component if the world stores one for that id.
//   - A SYSTEM (see systems.hpp) is a free function that runs over every entity
//     that has a given set of components.
//
// You build a "kind of thing" by giving an entity a COMBINATION of components,
// not by subclassing. A player is an entity with Transform + Velocity +
// PlayerControlled + RenderDot. A drifting mote is the same minus
// PlayerControlled. Adding a new capability later = a new component + a system,
// touching nothing that already exists. That composability is why engines use
// this pattern (handbook: "Composition over inheritance").
//
// Keep components small and copyable — they get iterated tightly and, later,
// serialized for network/saves.

namespace eng::sim {

// Where an entity is, in world units.
struct Transform {
  Vec2 position{0.0f, 0.0f};
};

// The entity's position as of the PREVIOUS tick. The simulation steps at a fixed
// rate but the screen refreshes faster, so the renderer draws somewhere between
// the last two ticks — interpolating PrevTransform -> Transform — to look smooth
// (see simulation.hpp). Updated at the start of every step.
struct PrevTransform {
  Vec2 position{0.0f, 0.0f};
};

// How fast it is moving, in world units per second.
struct Velocity {
  Vec2 value{0.0f, 0.0f};
};

// Marks an entity as driven by a player's input. The player id lets the command
// funnel route "player 1 pushed left" to the right entity — trivial in
// single-player, essential once several players share a world.
struct PlayerControlled {
  PlayerId player = kLocalPlayer;
  // Top speed while an input is held, in world units per second. The command
  // funnel sets velocity = clamped_input_direction * move_speed directly — there
  // is no acceleration or inertia yet, so the entity starts and stops instantly.
  float move_speed = 300.0f;
};

// Marks an entity as a non-player character. Empty for now — its whole job is to
// answer "is this a person the world runs, rather than the player?" so systems
// can treat the two differently (most importantly: an NPC that dies is destroyed,
// not respawned — permadeath, the game's core rule). This is the seed of the
// tiered NPC identity from the master plan: later, "Named" NPCs gain more
// components (a name, relationships, skills) while this marker stays the floor.
struct Npc {};

// Presentation-only: how the debug renderer should draw this entity. The
// simulation never reads this — it exists so the client has something to show.
// Colours are 0..1 RGB.
struct RenderDot {
  Vec3 color{0.9f, 0.9f, 0.9f};
  float radius = 6.0f;
};

// Marks an entity as dangerous to touch. An entity with a Hazard deals `damage`
// to any player it overlaps and is then consumed — destroyed (see the
// resolve_contacts system). The drifting motes have this: touch one, take a hit,
// and it's gone.
struct Hazard {
  float damage = 20.0f;
};

// --- Stats system ---
//
// The foundation for player and NPC stats. Deliberately small: a reusable Vital
// building block and a Stats "character sheet" that holds them. This is where
// the game's stats and skills grow (see docs/engine/skeleton/extending.md), but
// only as they're actually needed — no speculative RPG scaffolding.

// A depleting, regenerating resource with a cap. Health today; stamina, hunger,
// and mana will be the same shape, so they share this one type. Floats (not
// ints) so recovery can accrue smoothly across the fixed 60 Hz ticks.
struct Vital {
  float current = 100.0f;
  float max = 100.0f;
  float regen_per_second = 0.0f;  // 0 = doesn't recover on its own
};

// An entity's stat sheet — one component holding everything about its condition,
// so a player or an NPC-management screen reads a single place. Give an entity a
// Stats and it participates in the stats systems; leave it off and it doesn't.
//
// To grow the system you add fields HERE — another Vital, later a set of
// attributes or skills — and teach a system to read them. `stamina` was added
// exactly that way: one field here plus a small update_stamina system (it's spent
// by moving, not passively regained, so it earns its own system rather than a
// line in regenerate_vitals). Nothing else in the engine had to change.
struct Stats {
  Vital health{100.0f, 100.0f, 5.0f};    // full, and slowly self-heals
  Vital stamina{100.0f, 100.0f, 20.0f};  // spent by moving; recovers when resting
};

// --- Progression: skills feed attributes ---
//
// The game grows characters the "learn by doing" way, and in three layers:
//
//   activity  ->  a SKILL levels up  ->  an ATTRIBUTE rises  ->  a derived stat grows
//
// A Skill improves with the activity that uses it; skills roll up into broad
// Attributes; attributes shape the Vitals you feel in play. The player and NPCs
// share this exact machinery — anyone carrying these components levels the same
// way, which is the whole point of doing it in the ECS.

// One trainable skill: XP accrues from doing the related activity, and crossing a
// threshold raises the level (see the advance_progression system). Levels are what
// attributes read; xp is the progress toward the next one.
struct Skill {
  int level = 1;
  float xp = 0.0f;
};

// The skills an entity can train, bundled like Stats. One so far — conditioning,
// built by staying active, which feeds Endurance. A new skill is a new field here
// plus the activity that trains it.
struct Skills {
  Skill conditioning;
};

// Broad character attributes, DERIVED from skills every tick (skills feed
// attributes). They are the dial between "what you practise" and "how strong you
// are". Endurance is the first: it makes you tougher — a bigger health and stamina
// pool. Recomputed by advance_progression, so nothing sets these by hand.
struct Attributes {
  int endurance = 0;  // from conditioning; raises max health & stamina
};

}  // namespace eng::sim
