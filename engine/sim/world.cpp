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
  reg.emplace<Hazard>(e);  // motes damage the player on contact, then are consumed
  return e;
}

// Create one NPC: a wandering non-player character. It has Stats (so it takes
// contact damage and could regenerate) and the Npc marker (so handle_deaths
// destroys it on death rather than respawning it — permadeath). It is otherwise a
// drifting dot, like a mote, but it is a *person* the world owns, not a hazard.
entt::entity make_npc(entt::registry& reg, Vec2 pos, Vec2 vel) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<Velocity>(e, vel);
  reg.emplace<RenderDot>(e, Vec3{0.4f, 0.85f, 0.4f},
                         8.0f);                       // green, a touch smaller than the player
  reg.emplace<Stats>(e, Vital{60.0f, 100.0f, 4.0f});  // frailer than the player; heals slowly
  reg.emplace<Npc>(e);
  return e;
}

// Build the opening scene: a controllable player in the centre, a few wandering
// NPCs, and a dozen ambient motes drifting in deterministic directions. Returns
// the player entity.
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

  // A handful of NPCs wandering among the motes. They drift into hazards, take
  // damage, and — being NPCs, not the player — die for good when their health
  // runs out. Deterministic spawn spots, same as the motes.
  for (int i = 0; i < 4; ++i) {
    const Vec2 pos{static_cast<float>((i + 1) * 200 % static_cast<int>(kFieldWidth)),
                   static_cast<float>((i + 1) * 140 % static_cast<int>(kFieldHeight))};
    make_npc(reg, pos, Vec2{vel(rng), vel(rng)});
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
  update_stamina(registry_, dt);  // moving costs stamina; resting restores it
  wrap_bounds(registry_, Vec2{kFieldWidth, kFieldHeight});
  // Collision runs after movement (positions are current), then death is checked
  // from any damage it dealt, then survivors regenerate. This order is the
  // definition of the tick — collision before death before heal.
  resolve_contacts(registry_);
  handle_deaths(registry_, Vec2{kFieldWidth * 0.5f, kFieldHeight * 0.5f});
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
      // Requiring Stats too lets the command read stamina: an exhausted player
      // (0 stamina) is slowed to a crawl rather than stopped dead, so they can
      // always limp to safety while stamina recovers.
      auto view = registry_.view<PlayerControlled, Velocity, Stats>();
      for (const entt::entity e : view) {
        const PlayerControlled& pc = view.get<PlayerControlled>(e);
        if (pc.player != cmd.player) continue;
        const float speed =
            view.get<Stats>(e).stamina.current > 0.0f ? pc.move_speed : pc.move_speed * 0.4f;
        view.get<Velocity>(e).value = dir * speed;
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
