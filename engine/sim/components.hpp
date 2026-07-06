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
  // How quickly this entity accelerates toward the input direction (units/s^2).
  float move_speed = 300.0f;
};

// Presentation-only: how the debug renderer should draw this entity. The
// simulation never reads this — it exists so the client has something to show.
// Colours are 0..1 RGB.
struct RenderDot {
  Vec3 color{0.9f, 0.9f, 0.9f};
  float radius = 6.0f;
};

}  // namespace eng::sim
