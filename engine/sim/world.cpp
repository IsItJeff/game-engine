#include "engine/sim/world.hpp"

#include "engine/sim/components.hpp"
#include "engine/sim/systems.hpp"
#include "engine/sim/types.hpp"

namespace eng::sim {

namespace {

// Create one drifting "mote": an entity with a position, a velocity, and a
// colour to draw with. Used both for the opening scene and the SpawnMote command.
entt::entity make_mote(entt::registry& reg, Vec2 pos, Vec2 vel, Vec3 color) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<Velocity>(e, vel);
  reg.emplace<RenderDot>(e, color, 5.0f);
  return e;
}

// Build the opening scene: a controllable player in the centre plus a dozen
// ambient motes drifting in deterministic directions. Returns the player entity.
entt::entity build_scene(entt::registry& reg, std::mt19937& rng) {
  const Vec2 center{kFieldWidth * 0.5f, kFieldHeight * 0.5f};

  const entt::entity player = reg.create();
  reg.emplace<Transform>(player, center);
  reg.emplace<PrevTransform>(player, center);
  reg.emplace<Velocity>(player);
  reg.emplace<PlayerControlled>(player, kLocalPlayer, 320.0f);
  reg.emplace<RenderDot>(player, Vec3{0.3f, 0.8f, 1.0f}, 10.0f);  // bright blue
  reg.emplace<Stats>(player, Vital{70.0f, 100.0f, 8.0f});         // spawn worn; heals 8/sec

  // Deterministic directions from the seeded PRNG so every run starts identically.
  std::uniform_real_distribution<float> vel(-80.0f, 80.0f);
  for (int i = 0; i < 12; ++i) {
    const Vec2 pos{static_cast<float>((i + 1) * 90 % static_cast<int>(kFieldWidth)),
                   static_cast<float>((i + 1) * 60 % static_cast<int>(kFieldHeight))};
    make_mote(reg, pos, Vec2{vel(rng), vel(rng)}, Vec3{0.8f, 0.7f, 0.3f});
  }
  return player;
}

}  // namespace

World::World() {
  player_ = build_scene(registry_, rng_);
}

void World::submit(const Command& cmd) {
  pending_.push_back(cmd);
}

void World::step() {
  // 1. Remember where everything was, so the renderer can interpolate.
  snapshot_previous(registry_);

  // 2. Drain the command funnel: apply every queued intent, in order. This is
  //    the only place the world mutates in response to outside input.
  for (const Command& cmd : pending_) {
    apply_command(cmd);
  }
  pending_.clear();

  // 3. Run the systems, in this fixed, readable order. Adding behaviour means
  //    writing a system and adding a line here.
  const float dt = static_cast<float>(kSecondsPerTick);
  integrate_motion(registry_, dt);
  wrap_bounds(registry_, Vec2{kFieldWidth, kFieldHeight});
  regenerate_vitals(registry_, dt);

  // 4. One tick done.
  ++tick_;
}

void World::apply_command(const Command& cmd) {
  switch (cmd.kind) {
    case CommandKind::MovePlayer: {
      // Clamp the input direction to length <= 1 HERE, at the funnel — this is
      // exactly why a single choke point is worth having. The keyboard builds a
      // diagonal like (1, 1) whose length is 1.41, which would move the player
      // 41% faster diagonally than straight. Clamping once covers every sender
      // (keyboard now, the network later) instead of trusting each to normalize.
      Vec2 dir = cmd.move_dir;
      const float magnitude = glm::length(dir);
      if (magnitude > 1.0f) dir /= magnitude;

      // Find every player-controlled entity belonging to this player and set its
      // velocity from the input direction. (In single-player that's one entity;
      // the loop is what makes it correct once there are several players.)
      auto view = registry_.view<PlayerControlled, Velocity>();
      for (const entt::entity e : view) {
        const PlayerControlled& pc = view.get<PlayerControlled>(e);
        if (pc.player != cmd.player) continue;
        view.get<Velocity>(e).value = dir * pc.move_speed;
      }
      break;
    }
    case CommandKind::SpawnMote: {
      std::uniform_real_distribution<float> vel(-120.0f, 120.0f);
      make_mote(registry_, cmd.spawn_pos, Vec2{vel(rng_), vel(rng_)},
                Vec3{0.9f, 0.4f, 0.4f});  // reddish, so spawned motes stand out
      break;
    }
    case CommandKind::DamagePlayer: {
      // Subtract from the matching player's health, clamped at 0 (no negative
      // health). Only entities that are player-controlled AND have Stats can be
      // hit — the view filters the rest out for free. Because this runs on the
      // server through the funnel, a client can't fake damage: it can only ask.
      auto view = registry_.view<PlayerControlled, Stats>();
      for (const entt::entity e : view) {
        if (view.get<PlayerControlled>(e).player != cmd.player) continue;
        Vital& health = view.get<Stats>(e).health;
        health.current -= cmd.amount;
        if (health.current < 0.0f) health.current = 0.0f;
      }
      break;
    }
  }
}

}  // namespace eng::sim
