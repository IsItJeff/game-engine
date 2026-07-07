#include "engine/sim/systems.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/sim/components.hpp"
#include "engine/sim/progression/curve.hpp"

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

namespace {

// Spend full XP bars to raise a {level, xp} pair, carrying the remainder — shared
// by skills and attributes (both are an int level + Fixed xp). The threshold is
// xp_to_next(level) from the curve (the cost half of the one law). A while loop, so
// one big grant can cross several levels at once.
void apply_levels(int& level, Fixed& xp) {
  // ponytail: threshold is held in Q16.16, which saturates at 32767.99998, so
  // xp_to_next (100·level) stops growing once it reaches 32768 — i.e. past level
  // ~327. Beyond there leveling diverges from the true curve. That's ~5.3M XP / ~74h
  // of one-skill grind, well past the 255-entry POWER table, so it's a
  // known ceiling, not a bug to fix now: widen xp/threshold past Q16.16 (or an
  // int64 XP path) when levels can actually reach it.
  Fixed threshold = Fixed::from_int(static_cast<std::int32_t>(xp_to_next(level)));
  while (xp >= threshold) {
    xp -= threshold;
    ++level;
    threshold = Fixed::from_int(static_cast<std::int32_t>(xp_to_next(level)));
  }
}

}  // namespace

void advance_progression(entt::registry& reg) {
  constexpr float kHealthPerEndurance = 10.0f;  // how much tougher each point makes you
  constexpr float kStaminaPerEndurance = 5.0f;
  // XP earned per tick of activity. The timestep is fixed (1/60 s), so a
  // per-second rate is a constant per-tick Fixed amount — deterministic, no float
  // in the loop (20 XP/sec ÷ 60 ticks).
  const Fixed kConditioningPerTick = Fixed::from_ratio(20, 60);
  // The Character Level earns a FRACTION of the same activity — a quarter here — so
  // it climbs slower than the skill it comes from and stays the gentle "veteran"
  // layer, not a second fast track. A balance knob, not a law (design: tune at
  // playtest).
  const Fixed kCharLevelShare = Fixed::from_ratio(1, 4);

  // NB: CharacterLevel is required here, so any new progression-capable entity must
  // be spawned WITH it (see world.cpp: player + NPC) — miss it and that entity
  // silently never grows, no error. Keep the progression components together.
  auto view = reg.view<Skills, Attributes, Stats, Velocity, CharacterLevel>();
  for (const entt::entity e : view) {
    Skill& conditioning = view.get<Skills>(e).train(SkillId::Conditioning);
    Attribute& endurance = view.get<Attributes>(e).endurance;
    CharacterLevel& character = view.get<CharacterLevel>(e);

    // 1. Activity earns XP for the SKILL and its MAIN attribute, plus a fraction to
    //    the global Character Level. Conditioning's main attribute is Endurance, so
    //    it takes the full share for now (skills with contributors will split it
    //    later). Standing still trains nothing.
    if (glm::length(view.get<Velocity>(e).value) > 0.0f) {
      conditioning.xp += kConditioningPerTick;
      endurance.xp += kConditioningPerTick;
      character.xp += kConditioningPerTick * kCharLevelShare;
    }

    // 2. Turn full XP bars into levels — EVERY trained skill, the attribute, and the
    //    character level each climb on their own {level, Fixed xp}. Iterating all
    //    owned skills means a skill trained elsewhere (Toughness, fed by
    //    train_on_damage) levels here too, without this system knowing the source.
    for (auto& entry : view.get<Skills>(e).owned) apply_levels(entry.second.level, entry.second.xp);
    apply_levels(endurance.level, endurance.xp);
    apply_levels(character.level, character.xp);

    // 3. Attributes shape derived stats: each Endurance level past the first adds to
    //    the pools, on top of each Vital's own `base`. The Character Level then
    //    scales that EARNED bonus (not the base) by POWER(char_level - 1) — level 1
    //    is POWER(0) = 1.0, so a fresh character is unchanged and a veteran's earned
    //    toughness compounds a little. Only the MAX grows — a longer bar, not a free
    //    heal; regen fills the new room in.
    const float bonus = static_cast<float>(endurance.level - 1);
    const float veteran = static_cast<float>(power(character.level - 1).to_double());
    Stats& stats = view.get<Stats>(e);
    stats.health.max = stats.health.base + bonus * kHealthPerEndurance * veteran;
    stats.stamina.max = stats.stamina.base + bonus * kStaminaPerEndurance * veteran;
  }
}

void train_on_damage(entt::registry& reg, entt::entity victim, float damage) {
  // Only a progression entity (Skills + Attributes) toughens; a bare-Stats target,
  // or a non-damaging call, trains nothing.
  Skills* skills = reg.try_get<Skills>(victim);
  Attributes* attrs = reg.try_get<Attributes>(victim);
  if (skills == nullptr || attrs == nullptr || damage <= 0.0f) return;

  // 1 XP per point of damage survived — a tunable, so ~100 damage endured earns a
  // Toughness level. Snap the float pool damage to an int BEFORE the Fixed XP path
  // so the deterministic accumulator never sees a float. Toughness's main attribute
  // is Endurance, so (like Conditioning) it feeds the whole share there.
  const Fixed gained = Fixed::from_int(static_cast<std::int32_t>(damage));
  skills->train(SkillId::Toughness).xp += gained;
  attrs->endurance.xp += gained;
  // ponytail: the Character Level is fed by movement only for now; routing every
  // damage source into "fraction of all activity" waits until combat gives more
  // sources to balance against. advance_progression levels Toughness next tick.
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
      const float dmg = hazards.get<Hazard>(h).damage;
      Vital& health = targets.get<Stats>(t).health;
      health.current -= dmg;
      if (health.current < 0.0f) health.current = 0.0f;  // clamp at 0
      train_on_damage(reg, t, dmg);                      // enduring the hit toughens the survivor
      hit = true;
    }
    if (hit) consumed.push_back(h);  // consume it once, no matter how many it hit
  }

  for (const entt::entity e : consumed) reg.destroy(e);  // safe: iteration is done
}

}  // namespace eng::sim
