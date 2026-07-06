#include "engine/sim/systems.hpp"

#include <cmath>

#include "engine/sim/components.hpp"

namespace eng::sim {

void snapshot_previous(entt::registry& reg) {
  // For every entity that has both a current and previous position, copy current
  // -> previous. After this, PrevTransform holds "where it was", and the systems
  // below update Transform to "where it now is" — the two the renderer blends.
  auto view = reg.view<Transform, PrevTransform>();
  for (const entt::entity e : view) {
    view.get<PrevTransform>(e).position = view.get<Transform>(e).position;
  }
}

void integrate_motion(entt::registry& reg, float dt) {
  // The classic update: new position = old position + velocity * time. Runs over
  // every entity that has both a Transform and a Velocity, and no others — that
  // automatic filtering is the ECS's core convenience.
  auto view = reg.view<Transform, Velocity>();
  for (const entt::entity e : view) {
    view.get<Transform>(e).position += view.get<Velocity>(e).value * dt;
  }
}

void wrap_bounds(entt::registry& reg, Vec2 field_size) {
  // Toroidal wrap: an entity leaving the right edge reappears on the left, etc.
  // std::fmod-style wrapping keeps positions inside [0, size) without a branch
  // per edge. Purely to keep the demo's motes on screen.
  auto view = reg.view<Transform>();
  for (const entt::entity e : view) {
    Vec2& p = view.get<Transform>(e).position;
    p.x -= std::floor(p.x / field_size.x) * field_size.x;
    p.y -= std::floor(p.y / field_size.y) * field_size.y;
  }
}

namespace {

// Move one vital toward its cap at its own rate. Splitting this out is what
// keeps regenerate_vitals a single line per stat as more vitals are added.
void recover(Vital& v, float dt) {
  v.current += v.regen_per_second * dt;
  if (v.current > v.max) v.current = v.max;  // never past the cap
}

}  // namespace

void regenerate_vitals(entt::registry& reg, float dt) {
  // view<Stats>() iterates exactly the entities that have stats — the player
  // here, not the drifting motes — so this can't touch anything without them.
  // That automatic filtering is the ECS's whole point: behaviour applies to
  // whoever has the right data, nobody else.
  auto view = reg.view<Stats>();
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);
    recover(s.health, dt);
    // When you add another vital (e.g. Stats::stamina), recover it here too.
  }
}

void handle_deaths(entt::registry& reg, Vec2 respawn_point) {
  // Only player-controlled entities are considered here; when NPCs arrive, a
  // zero-health NPC would be destroyed instead (permadeath, the game's core
  // rule). Resetting a few components — no entity creation or destruction — so
  // iterating the view while writing to it is safe.
  auto view = reg.view<Stats, PlayerControlled, Transform, Velocity>();
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);
    if (s.health.current > 0.0f) continue;            // still alive, nothing to do
    s.health.current = s.health.max;                  // respawn at full health...
    view.get<Transform>(e).position = respawn_point;  // ...at the spawn point...
    view.get<Velocity>(e).value = Vec2{0.0f, 0.0f};   // ...and standing still
  }
}

void damage_on_contact(entt::registry& reg, float dt) {
  // "In contact" = centres within this many world units. A real engine gives
  // each entity a collision shape (roadmap M4, Jolt); one distance is plenty for
  // round dots, and 15 matches the default player+mote radii (10 + 5).
  constexpr float kContactDistance = 15.0f;

  // Nested loop: every player against every hazard. Fine for a handful of each;
  // a real game with thousands would use a spatial grid to avoid the O(n*m).
  auto players = reg.view<PlayerControlled, Stats, Transform>();
  auto hazards = reg.view<Hazard, Transform>();
  for (const entt::entity p : players) {
    const Vec2 p_pos = players.get<Transform>(p).position;
    Stats& p_stats = players.get<Stats>(p);
    for (const entt::entity h : hazards) {
      if (glm::distance(p_pos, hazards.get<Transform>(h).position) >= kContactDistance) {
        continue;  // not touching this one
      }
      p_stats.health.current -= hazards.get<Hazard>(h).damage_per_second * dt;
      if (p_stats.health.current < 0.0f) p_stats.health.current = 0.0f;  // clamp at 0
    }
  }
}

}  // namespace eng::sim
