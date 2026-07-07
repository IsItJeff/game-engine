#include "engine/sim/systems.hpp"

#include <cmath>
#include <vector>

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

void steer_npcs(entt::registry& reg) {
  // How far an NPC can "see" a hazard, and how fast it flees one. Plain constants
  // until an NPC ever needs its own values — then they'd become fields on a
  // component (rule 12: write the concrete thing first, abstract on the 2nd use).
  constexpr float kSenseRadius = 120.0f;
  constexpr float kFleeSpeed = 90.0f;

  // Nested loop: every NPC against every hazard — O(n*m), fine for a handful. A
  // real crowd would query a spatial grid, the same upgrade resolve_contacts wants.
  auto npcs = reg.view<Npc, Transform, Velocity>();
  auto hazards = reg.view<Hazard, Transform>();
  for (const entt::entity n : npcs) {
    const Vec2 pos = npcs.get<Transform>(n).position;

    // Perception: find the single nearest hazard within sense range.
    float nearest = kSenseRadius;
    Vec2 threat{0.0f, 0.0f};
    bool sees_threat = false;
    for (const entt::entity h : hazards) {
      const Vec2 h_pos = hazards.get<Transform>(h).position;
      const float d = glm::distance(pos, h_pos);
      if (d < nearest) {
        nearest = d;
        threat = h_pos;
        sees_threat = true;
      }
    }

    // Action: flee straight away from the threat. With nothing in range the NPC
    // keeps whatever velocity it already had, so it just drifts on.
    if (sees_threat) {
      const Vec2 away = pos - threat;
      const float len = glm::length(away);
      if (len > 0.0f) npcs.get<Velocity>(n).value = (away / len) * kFleeSpeed;
    }
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
    // Health only ever ticks back up, so it recovers here. Stamina is different —
    // it's spent by moving — so it has its own system (update_stamina) instead of
    // a passive line here.
  }
}

void update_stamina(entt::registry& reg, float dt) {
  // Cost of moving, in stamina per second. Higher than stamina's own regen rate
  // so movement is a real drain. Tuning knob: raise it and you tire faster.
  constexpr float kDrainPerSecond = 40.0f;

  // Stats + Velocity = things that both tire and move. Motes have Velocity but no
  // Stats, so the view skips them for free — only the player pays.
  auto view = reg.view<Stats, Velocity>();
  for (const entt::entity e : view) {
    Vital& stamina = view.get<Stats>(e).stamina;
    if (glm::length(view.get<Velocity>(e).value) > 0.0f) {
      stamina.current -= kDrainPerSecond * dt;             // moving: spend it...
      if (stamina.current < 0.0f) stamina.current = 0.0f;  // ...never below empty
    } else {
      recover(stamina, dt);  // resting: recover at the vital's own regen rate
    }
  }
}

float xp_to_next(int level) {
  // Linear curve: level 1->2 costs 100, 2->3 costs 200, and so on. This single
  // constant sets the whole pace — raise it and everyone levels more slowly.
  return 100.0f * static_cast<float>(level);
}

void advance_progression(entt::registry& reg, float dt) {
  constexpr float kConditioningPerSecond = 20.0f;  // XP/sec while active — tune the pace
  constexpr float kHealthPerEndurance = 10.0f;     // how much tougher each point makes you
  constexpr float kStaminaPerEndurance = 5.0f;

  auto view = reg.view<Skills, Attributes, Stats, Velocity>();
  for (const entt::entity e : view) {
    Skills& skills = view.get<Skills>(e);

    // 1. Activity earns XP: moving trains conditioning (the same "is it moving?"
    //    signal update_stamina reads). Standing still trains nothing.
    if (glm::length(view.get<Velocity>(e).value) > 0.0f) {
      skills.conditioning.xp += kConditioningPerSecond * dt;
    }

    // 2. Spend a full XP bar on a level. A while loop, so one big grant can cross
    //    several levels at once (and it carries the remainder forward).
    while (skills.conditioning.xp >= xp_to_next(skills.conditioning.level)) {
      skills.conditioning.xp -= xp_to_next(skills.conditioning.level);
      ++skills.conditioning.level;
    }

    // 3. Skills feed attributes: endurance follows conditioning (level 1 = 0
    //    bonus, so a fresh character is unchanged).
    Attributes& attr = view.get<Attributes>(e);
    attr.endurance = skills.conditioning.level - 1;

    // 4. Attributes shape derived stats: more endurance = bigger pools, added on
    //    top of each Vital's own `base`. Only the MAX grows — a longer bar, not a
    //    free heal; regen fills the new room in.
    Stats& stats = view.get<Stats>(e);
    const float end = static_cast<float>(attr.endurance);
    stats.health.max = stats.health.base + end * kHealthPerEndurance;
    stats.stamina.max = stats.stamina.base + end * kStaminaPerEndurance;
  }
}

void handle_deaths(entt::registry& reg, Vec2 respawn_point) {
  // A zero-health entity meets one of two fates, and which one is the game's core
  // rule made concrete: the PLAYER respawns; an NPC dies for good (permadeath).

  // Players: reset a few components in place. Nothing is created or destroyed, so
  // writing to the view while iterating it is safe.
  auto players = reg.view<Stats, PlayerControlled, Transform, Velocity>();
  for (const entt::entity e : players) {
    Stats& s = players.get<Stats>(e);
    if (s.health.current > 0.0f) continue;               // still alive, nothing to do
    s.health.current = s.health.max;                     // respawn at full health...
    players.get<Transform>(e).position = respawn_point;  // ...at the spawn point...
    players.get<Velocity>(e).value = Vec2{0.0f, 0.0f};   // ...and standing still
  }

  // NPCs: permadeath. Collect the dead, then destroy them AFTER the loop —
  // reg.destroy() during iteration invalidates the view (the same collect-then-
  // destroy pattern resolve_contacts uses to consume motes). This is the branch
  // the stats docs kept promising; here it finally exists.
  std::vector<entt::entity> dead;
  auto npcs = reg.view<Stats, Npc>();
  for (const entt::entity e : npcs) {
    if (npcs.get<Stats>(e).health.current <= 0.0f) dead.push_back(e);
  }
  for (const entt::entity e : dead) reg.destroy(e);
}

void resolve_contacts(entt::registry& reg) {
  // "In contact" = centres within this many world units. A real engine gives
  // each entity a collision shape (roadmap M4, Jolt); one distance is plenty for
  // round dots, and 15 matches the default player+mote radii (10 + 5).
  constexpr float kContactDistance = 15.0f;

  // Gather the hazards to destroy. We must NOT call reg.destroy() while iterating
  // the view below: destroying an entity mid-iteration invalidates the view and
  // is undefined behaviour (the classic ECS trap). So we collect first, then
  // destroy in a separate pass.
  std::vector<entt::entity> consumed;

  // Nested loop: every hazard against every target — anything with Stats, which
  // is the player AND the NPCs (motes have no Stats, so they can't hurt each
  // other). Fine for a handful of each; a real game with thousands would use a
  // spatial grid to avoid the O(n*m).
  auto targets = reg.view<Stats, Transform>();
  auto hazards = reg.view<Hazard, Transform>();
  for (const entt::entity h : hazards) {
    const Vec2 h_pos = hazards.get<Transform>(h).position;
    bool hit = false;
    for (const entt::entity t : targets) {
      if (glm::distance(targets.get<Transform>(t).position, h_pos) >= kContactDistance) {
        continue;  // this target isn't touching it
      }
      Vital& health = targets.get<Stats>(t).health;
      health.current -= hazards.get<Hazard>(h).damage;
      if (health.current < 0.0f) health.current = 0.0f;  // clamp at 0
      hit = true;
    }
    if (hit) consumed.push_back(h);  // consume it once, no matter how many it hit
  }

  for (const entt::entity e : consumed) reg.destroy(e);  // safe: iteration is done
}

}  // namespace eng::sim
