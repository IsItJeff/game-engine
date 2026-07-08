#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "engine/core/fixed.hpp"
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
  // The un-bonused pool size. `max` is recomputed each tick as base + attribute
  // bonuses (see advance_progression), so an entity that should be tankier just
  // spawns with a bigger base — no hardcoded constant to trip over. Defaults to
  // 100 so every Vital{current, max, regen} literal keeps today's numbers.
  float base = 100.0f;
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

// One trainable skill. XP accrues from doing the related activity and crossing a
// threshold raises the level. `xp` is a Fixed so it can gain a *fraction* each
// 60 Hz tick (20 XP/sec is 0.333/tick) and still be deterministic — an int would
// round every tick's gain to zero.
struct Skill {
  int level = 1;
  Fixed xp{};
};

// Identifies a skill. An enum (not a string) so lookups are cheap and the wire/
// save formats stay small. New skills append here.
enum class SkillId : std::uint16_t {
  Conditioning,  // trained by moving; main attribute Endurance
  Toughness,     // trained by taking damage; main attribute Endurance (a VIT skill)
  Striking,      // trained by attacking; main attribute Strength
  Recovery,      // trained by resting to recover spent stamina; main attribute Endurance
};

// The skills an entity is training — a KEYED collection, so a character can hold a
// whole branching tree of skills rather than one struct field each. A small
// append-ordered vector: deterministic iteration (a hash map's order is not) and
// no per-entity heap churn until a character actually learns many. Everyone starts
// knowing Conditioning (trained by staying active).
struct Skills {
  std::vector<std::pair<SkillId, Skill>> owned{{SkillId::Conditioning, Skill{}}};

  // Find a learned skill, or nullptr.
  const Skill* find(SkillId id) const {
    for (const auto& [key, skill] : owned) {
      if (key == id) return &skill;
    }
    return nullptr;
  }
  // Get a skill to train, learning it at level 1 if it's new.
  Skill& train(SkillId id) {
    for (auto& [key, skill] : owned) {
      if (key == id) return skill;
    }
    owned.emplace_back(id, Skill{});
    return owned.back().second;
  }
};

// A broad character attribute — now with its OWN level + XP (revised). The skills
// that use it feed it: a skill grants its MAIN attribute a lot of XP and each
// contributor a little, so an attribute levels in parallel with the skills that
// train it, rather than being recomputed from one skill. Endurance is the first;
// its level shapes the health & stamina pools (see advance_progression).
struct Attribute {
  int level = 1;
  Fixed xp{};
};

struct Attributes {
  Attribute endurance;  // fed by Conditioning + Toughness; each level past 1 grows the pools
  Attribute strength;   // fed by Striking; each level past 1 lengthens attack reach
};

// A single global "how experienced overall" level, fed by a fraction of ALL
// activity (not one skill). Its level is a gentle multiplier on earned stats via
// the same POWER curve, so a long-lived character is generally a bit better across
// the board — the "veteran" layer on top of specific skills and attributes. Starts
// at level 1 = POWER(0) = no head start, exactly like the other {level, xp} pairs.
struct CharacterLevel {
  int level = 1;
  Fixed xp{};
};

}  // namespace eng::sim
