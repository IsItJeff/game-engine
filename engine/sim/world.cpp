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
  reg.emplace<Skills>(e);  // NPCs train and grow, exactly like the player
  reg.emplace<Attributes>(e);
  reg.emplace<CharacterLevel>(e);
  reg.emplace<Npc>(e);
  return e;
}

// Create one hostile creature from an archetype's numbers: HP (Stats) that attacks
// whittle down, VIT (Attributes) that softens blows, and the Enemy marker (so
// chase_prey hunts, resolve_creature_contacts hurts, handle_deaths reaps at 0 HP).
// No regen — you can wear it down; no Skills/CharacterLevel — it doesn't grow.
entt::entity make_creature(entt::registry& reg, Vec2 pos, float hp, float chase_speed,
                           float attack_damage, int defence_level, Vec3 color, float radius) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<Velocity>(e);
  reg.emplace<RenderDot>(e, color, radius);
  reg.emplace<Stats>(e, Vital{hp, hp, 0.0f});
  reg.emplace<Attributes>(e).endurance.level = defence_level;
  Enemy& enemy = reg.emplace<Enemy>(e);
  enemy.attack_damage = attack_damage;
  enemy.chase_speed = chase_speed;
  return e;
}

// The two archetypes. A BRUTE lumbers in: tough (40 HP), well-armoured, hits hard —
// wear it down and kite it. A SWARMER sprints: fragile (15 HP, ~one strike) and weak,
// but fast and it comes in numbers, so it's the one that corners you.
entt::entity make_brute(entt::registry& reg, Vec2 pos) {
  return make_creature(reg, pos, 40.0f, 70.0f, 15.0f, 3, Vec3{0.85f, 0.2f, 0.2f},
                       9.0f);  // deep red
}
entt::entity make_swarmer(entt::registry& reg, Vec2 pos) {
  const entt::entity e =
      make_creature(reg, pos, 15.0f, 130.0f, 8.0f, 1, Vec3{0.95f, 0.5f, 0.15f}, 6.0f);  // orange
  // Fast AND slippery: a swarmer's Dexterity gives it an innate ~21% chance to dodge your
  // strikes (dodge_chance(8) = 7 * 0.03), so the fragile-but-quick archetype is genuinely
  // hard to pin down — brutes (default DEX 1) never dodge. A tuning knob; creatures don't
  // grow, so this DEX stays fixed at the archetype's spawn value.
  reg.get<Attributes>(e).dexterity.level = 8;
  return e;
}

// Keep the fight alive: once the spawn timer runs out, add a creature at a field edge
// (if we're under the cap) and reset it. Deterministic — the timer is a fixed per-tick
// countdown and the position comes from the seeded rng, so every run spawns the same
// reinforcements at the same ticks (single-platform, like the rest of the sim's rng).
void spawn_creature_if_due(entt::registry& reg, float& timer, std::mt19937& rng, float dt) {
  timer -= dt;
  if (timer > 0.0f) return;
  timer = kCreatureSpawnInterval;
  if (static_cast<int>(reg.storage<Enemy>().size()) >= kMaxCreatures) return;
  // Spawn on a random field edge, so creatures arrive from outside and close in —
  // never right on top of the player (which would be a free, unavoidable first hit),
  // and the threat builds gradually as they cross the field rather than all at once.
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  const float along = unit(rng);
  Vec2 pos{};
  switch (static_cast<int>(unit(rng) * 4.0f)) {  // unit is [0,1) so this is 0..3
    case 0:
      pos = {along * kFieldWidth, 0.0f};
      break;  // top
    case 1:
      pos = {along * kFieldWidth, kFieldHeight};
      break;  // bottom
    case 2:
      pos = {0.0f, along * kFieldHeight};
      break;  // left
    default:
      pos = {kFieldWidth, along * kFieldHeight};
      break;  // right
  }
  // Roughly one brute for every two swarmers — mostly a fast fragile swarm with the
  // occasional tanky brute to anchor it. From the seeded rng, so still deterministic.
  if (unit(rng) < 0.35f) {
    make_brute(reg, pos);
  } else {
    make_swarmer(reg, pos);
  }
}

// Keep the colony alive: on its own (slower) timer, wander a fresh colonist in from a
// field edge when we're under the cap, so the NPCs creatures pick off (permadeath) are
// replaced and the field doesn't slowly empty of the very people whose skirmishes make
// the world feel alive. The mirror of spawn_creature_if_due, but drawing from its OWN
// `rng` stream so the spawner's placement/timing rolls stay off the creature stream. (The
// NPCs it spawns still influence that stream later via their combat dodge rolls — so this
// isn't full invariance to colony tuning; it just keeps the spawner's own draws separate.)
void spawn_npc_if_due(entt::registry& reg, float& timer, std::mt19937& rng, float dt) {
  timer -= dt;
  if (timer > 0.0f) return;
  timer = kNpcSpawnInterval;
  if (static_cast<int>(reg.storage<Npc>().size()) >= kMaxNpcs) return;
  // Arrive from a random field edge, like the creatures — a colonist walks in from
  // outside rather than popping into the middle of the field.
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::uniform_real_distribution<float> vel(-80.0f, 80.0f);
  const float along = unit(rng);
  Vec2 pos{};
  switch (static_cast<int>(unit(rng) * 4.0f)) {  // unit is [0,1) so this is 0..3
    case 0:
      pos = {along * kFieldWidth, 0.0f};
      break;  // top
    case 1:
      pos = {along * kFieldWidth, kFieldHeight};
      break;  // bottom
    case 2:
      pos = {0.0f, along * kFieldHeight};
      break;  // left
    default:
      pos = {kFieldWidth, along * kFieldHeight};
      break;  // right
  }
  make_npc(reg, pos, Vec2{vel(rng), vel(rng)});
}

// Build the opening scene: a controllable player in the centre, a few wandering
// NPCs, a couple of hunting creatures, and a dozen ambient motes drifting in
// deterministic directions. Returns the player entity.
entt::entity build_scene(entt::registry& reg, std::mt19937& rng) {
  const Vec2 center{kFieldWidth * 0.5f, kFieldHeight * 0.5f};

  const entt::entity player = reg.create();
  reg.emplace<Transform>(player, center);
  reg.emplace<PrevTransform>(player, center);
  reg.emplace<Velocity>(player);
  reg.emplace<PlayerControlled>(player, kLocalPlayer, 320.0f);
  reg.emplace<RenderDot>(player, Vec3{0.3f, 0.8f, 1.0f}, 10.0f);  // bright blue
  reg.emplace<Stats>(player, Vital{70.0f, 100.0f, 8.0f});         // spawn worn; heals 8/sec
  reg.emplace<Skills>(player);  // trains with activity; feeds Attributes (see advance_progression)
  reg.emplace<Attributes>(player);
  reg.emplace<CharacterLevel>(player);

  // Deterministic directions from the seeded PRNG so every run starts identically.
  std::uniform_real_distribution<float> vel(-80.0f, 80.0f);
  for (int i = 0; i < 12; ++i) {
    const Vec2 pos{static_cast<float>((i + 1) * 90 % static_cast<int>(kFieldWidth)),
                   static_cast<float>((i + 1) * 60 % static_cast<int>(kFieldHeight))};
    make_mote(reg, pos, Vec2{vel(rng), vel(rng)}, Vec3{0.8f, 0.7f, 0.3f});
  }

  // A handful of NPCs wandering among the motes. They flee hazards they sense
  // (steer_npcs), but can still get cornered, take damage, and — being NPCs, not
  // the player — die for good. Deterministic spawn spots, same as the motes.
  for (int i = 0; i < 4; ++i) {
    const Vec2 pos{static_cast<float>((i + 1) * 200 % static_cast<int>(kFieldWidth)),
                   static_cast<float>((i + 1) * 140 % static_cast<int>(kFieldHeight))};
    make_npc(reg, pos, Vec2{vel(rng), vel(rng)});
  }

  // Two hostile creatures at opposite corners that hunt the nearest person (you or an
  // NPC) — one of each kind so both archetypes show from the start. Strike them (J) to
  // wear their HP down; a stronger Strength kills faster. The spawner keeps a mix coming.
  make_brute(reg, Vec2{kFieldWidth * 0.2f, kFieldHeight * 0.2f});
  make_swarmer(reg, Vec2{kFieldWidth * 0.8f, kFieldHeight * 0.8f});
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
  steer_npcs(registry_);  // NPCs decide where to flee (may set their velocity)
  chase_prey(registry_);  // creatures decide to home in on the nearest person (player or NPC)
  integrate_motion(registry_, dt);
  npc_attack(registry_, rng_);     // NPCs strike any hazard now in reach (positions are current)
  update_stamina(registry_, dt);   // moving costs stamina; resting restores it
  drain_hunger(registry_, dt);     // people get hungry; starving (0) chips health before deaths
  advance_progression(registry_);  // activity -> skill+attribute XP -> level -> bigger pools
  wrap_bounds(registry_, Vec2{kFieldWidth, kFieldHeight});
  // Collision runs after movement (positions are current), then death is checked
  // from any damage it dealt, then survivors regenerate. This order is the
  // definition of the tick — collision before death before heal.
  resolve_contacts(registry_);                     // motes shatter on contact
  resolve_creature_contacts(registry_, dt, rng_);  // creatures swing; player may dodge (DEX)
  handle_deaths(registry_, Vec2{kFieldWidth * 0.5f, kFieldHeight * 0.5f}, dt);
  collect_pickups(registry_, dt);  // grab health orbs the slain creatures dropped; fade old ones
  regenerate_vitals(registry_, dt);

  // Reinforcements: after deaths are resolved, top the creature population back up
  // on a timer so the fight keeps coming.
  spawn_creature_if_due(registry_, creature_spawn_timer_, rng_, dt);
  spawn_npc_if_due(registry_, npc_spawn_timer_, npc_spawn_rng_, dt);  // colony reinforcements

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
        if (registry_.all_of<Downed>(e)) continue;  // downed = helpless, input does nothing
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
        train_on_damage(registry_, e, cmd.amount);  // toughen on a hit, same as contact damage
      }
      break;
    }
    case CommandKind::Attack: {
      // The player's swing runs the shared perform_attack resolver (systems.cpp) —
      // the exact one NPCs use — so a player and an NPC hit identically. The view
      // requires the attacker to be player-controlled AND carry the progression pair,
      // and we match by player id so a command can't swing for someone else (the same
      // anti-cheat the funnel exists for). Collect-then-destroy with a valid() guard,
      // in case a future co-op setup gives one PlayerId several attacking units.
      std::vector<entt::entity> struck;
      auto attackers = registry_.view<PlayerControlled, Transform, Attributes, Skills>();
      for (const entt::entity a : attackers) {
        if (attackers.get<PlayerControlled>(a).player != cmd.player) continue;
        const entt::entity t = perform_attack(registry_, a, rng_);
        if (t != entt::null) struck.push_back(t);
      }
      for (const entt::entity t : struck) {
        if (registry_.valid(t)) registry_.destroy(t);
      }
      break;
    }
  }
}

}  // namespace eng::sim
