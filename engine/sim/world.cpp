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

// Create a fixed WaterSource — a pond a thirsty colonist walks to and drinks from (the `drink`
// system). Transform + PrevTransform so the renderer draws it (a still, dark-blue disc — it never
// moves), RenderDot sized to its drink `radius` so the visible pool IS the reach, and WaterSource
// itself. No Stats/Velocity: it's scenery, not a person.
entt::entity make_water_source(entt::registry& reg, Vec2 pos, float radius) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.15f, 0.4f, 0.65f},
                         radius);  // deep-blue pool, drawn at its reach
  reg.emplace<WaterSource>(e, WaterSource{radius});
  return e;
}

// Create a fixed FoodSource — a berry patch / garden a hungry colonist grazes (the `graze` system).
// Like the pond it's Transform+PrevTransform+RenderDot (a green plot, drawn at its reach), but it's
// FINITE: `stock` depletes as colonists eat and regrows over time, so it's the renewable-crop seed,
// not an infinite buffet. Scenery — no Stats/Velocity.
entt::entity make_food_source(entt::registry& reg, Vec2 pos, float radius) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.25f, 0.55f, 0.2f},
                         radius);  // leafy green plot, drawn at its reach
  FoodSource fs{};
  fs.radius = radius;
  reg.emplace<FoodSource>(e, fs);
  return e;
}

// Create one NPC: a wandering non-player character. It has Stats (so it takes
// contact damage and could regenerate) and the Npc marker (so handle_deaths
// destroys it on death rather than respawning it — permadeath). It is otherwise a
// drifting dot, like a mote, but it is a *person* the world owns, not a hazard.
entt::entity make_npc(entt::registry& reg, Vec2 pos, Vec2 vel, int bravery = 0, int greed = 0,
                      int compassion = 0, int industry = 0) {
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
  // Its personality (P7 seed) — steer_npcs reads bravery to shape when it flees. Cast to the
  // int8 field explicitly (the caller passes a plain int for convenience).
  reg.emplace<Personality>(
      e, Personality{static_cast<std::int8_t>(bravery), static_cast<std::int8_t>(greed),
                     static_cast<std::int8_t>(compassion), static_cast<std::int8_t>(industry)});
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
  const entt::entity e = make_creature(reg, pos, 40.0f, 70.0f, 15.0f, 3, Vec3{0.85f, 0.2f, 0.2f},
                                       9.0f);  // deep red
  reg.get<Enemy>(e).drop = DropKind::Weapon;   // the hard kill pays out a weapon
  return e;
}
// A SENTINEL: the slow, heavily-plated tank — highest HP (60) and defence (VIT 5), but it
// lumbers (chase 50, slower than a brute), and on death it drops its ARMOUR (the defensive
// counterpart of the brute's weapon), giving armour a renewable battlefield source. ponytail:
// its HP / defence / speed and its spawn share (below) are tuning knobs, not load-bearing.
entt::entity make_sentinel(entt::registry& reg, Vec2 pos) {
  const entt::entity e = make_creature(reg, pos, 60.0f, 50.0f, 12.0f, 5, Vec3{0.45f, 0.5f, 0.62f},
                                       10.0f);  // slate blue, big
  reg.get<Enemy>(e).drop = DropKind::Armour;
  return e;
}
entt::entity make_swarmer(entt::registry& reg, Vec2 pos) {
  const entt::entity e =
      make_creature(reg, pos, 15.0f, 130.0f, 8.0f, 1, Vec3{0.95f, 0.5f, 0.15f}, 6.0f);  // orange
  // Fast AND slippery: a swarmer's Dexterity gives it an innate ~21% chance to dodge your
  // strikes (dodge_chance(8) = 7 * 0.03), so the fragile-but-quick archetype is genuinely
  // hard to pin down — brutes (default DEX 1) never dodge. A tuning knob; creatures don't
  // grow, so this DEX stays fixed at the archetype's spawn value.
  reg.get<Attributes>(e).dexterity.level = 8;
  // And VENOMOUS: a swarmer's bite leaves a lingering poison (tick_poison), so the fast swarm
  // punishes you even after you break away — the brute hits harder up front, the swarmer's threat
  // trails you. A knob (health/sec of venom); brutes/sentinels leave it 0 (not venomous).
  reg.get<Enemy>(e).poison_per_second = 9.0f;
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
  // The archetype mix, from ONE seeded draw (kept a single draw so the shared stream stays
  // aligned): a rare heavily-plated sentinel (10%), the occasional tanky brute (25%, its band
  // carved to make room for the sentinel), and mostly the fast fragile swarm (the rest). Still
  // deterministic. ponytail: the 0.10 / 0.35 bands are balance knobs.
  const float which = unit(rng);
  if (which < 0.10f) {
    make_sentinel(reg, pos);
  } else if (which < 0.35f) {
    make_brute(reg, pos);
  } else {
    make_swarmer(reg, pos);
  }
}

// The design's "NPCs roll an ARCHETYPE + jitter": a reinforcement colonist picks one of these
// coherent personality presets rather than independent random axes, so each is a recognizable
// character (a dependable Stalwart, a self-serving Rogue) instead of a random stat-blob. Values
// span the four WIRED axes (bravery, greed, compassion, industry) in [-100,100]; the design's other
// archetypes (Schemer, Zealot, Loner, Firebrand) append here once loyalty/sociability are wired to
// tell them apart. The opening four keep their hand-authored showcase spread (build_scene) — this
// is only for the ongoing reinforcements. ponytail: the numbers are tuning knobs.
constexpr std::array<Personality, 4> kArchetypes{{
    {70, -40, 50, 60},  // Stalwart: brave, generous, kind, industrious — the dependable backbone
    {-50, 80, -50, -30},  // Rogue: cowardly, greedy, callous, idle — out for itself
    {30, -60, 85, 20},    // Kindler: brave-ish, very generous, deeply compassionate — the carer
    {-10, 20, -15, 80},  // Drudge: timid and a touch selfish, but tireless — the heads-down worker
}};

// How far each axis wobbles off its archetype so two colonists of the same kind aren't clones —
// small enough that the archetype stays recognizable. A tuning knob.
constexpr int kArchetypeJitter = 15;

// Nudge one axis by ±kArchetypeJitter, clamped to the [-100, 100] axis range. Draws once from the
// spawner's own stream via the shared `unit` distribution.
int jitter(std::int8_t base, std::mt19937& rng, std::uniform_real_distribution<float>& unit) {
  const int delta =
      static_cast<int>(unit(rng) * static_cast<float>(2 * kArchetypeJitter + 1)) - kArchetypeJitter;
  int v = static_cast<int>(base) + delta;
  if (v > 100) v = 100;
  if (v < -100) v = -100;
  return v;
}

// Roll a reinforcement's personality: pick an archetype, then jitter each axis. Draws are SEQUENCED
// (the index first, then the four jitters in field order) so the result is deterministic same-build
// — the same discipline the bravery draw used. Uses the spawner's OWN stream via `unit`/`rng`.
Personality roll_archetype(std::mt19937& rng, std::uniform_real_distribution<float>& unit) {
  const std::size_t which =
      static_cast<std::size_t>(unit(rng) * static_cast<float>(kArchetypes.size()));
  const Personality base = kArchetypes[which < kArchetypes.size() ? which : kArchetypes.size() - 1];
  const int b = jitter(base.bravery, rng, unit);
  const int g = jitter(base.greed, rng, unit);
  const int c = jitter(base.compassion, rng, unit);
  const int in = jitter(base.industry, rng, unit);
  return Personality{static_cast<std::int8_t>(b), static_cast<std::int8_t>(g),
                     static_cast<std::int8_t>(c), static_cast<std::int8_t>(in)};
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
  // Each reinforcement rolls a coherent ARCHETYPE + jitter (the design's model), so the ongoing
  // colony stays as varied as the opening four instead of drifting toward neutral as they die.
  // Drawn from this spawner's OWN isolated stream, never the combat/creature rng. SEQUENCE the
  // draws into named locals before the call: the Vec2 braced-init fixes the two vel draws
  // left-to-right, then roll_archetype's draws follow in a separate statement. Passing draws as
  // bare function arguments would leave their order vs the vel draws unspecified — a cross-compiler
  // determinism hole.
  const Vec2 wander{vel(rng), vel(rng)};
  const Personality p = roll_archetype(rng, unit);
  make_npc(reg, pos, wander, p.bravery, p.greed, p.compassion, p.industry);
}

// Build the opening scene: a controllable player in the centre, a few wandering
// NPCs, a couple of hunting creatures, and a dozen ambient motes drifting in
// deterministic directions. Returns the player entity.
entt::entity build_scene(entt::registry& reg, std::mt19937& rng) {
  const Vec2 center{kFieldWidth * 0.5f, kFieldHeight * 0.5f};

  // A pond in the lower field and a berry patch in the upper — the colony's water and food. Both
  // created FIRST so they draw UNDER everyone standing on them, and off-centre so they're landmarks
  // to walk to. Watch thirsty colonists peel off to the pond and hungry ones to the garden; the
  // garden is finite, so a well-fed crowd picks it bare and it must regrow before it feeds again.
  make_water_source(reg, Vec2{center.x, kFieldHeight * 0.8f}, 60.0f);
  make_food_source(reg, Vec2{center.x, kFieldHeight * 0.2f}, 55.0f);

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
    // A fixed personality spread so the demo shows the range from the first frame: bravery
    // -90/-30/+30/+90, greed REVERSED (+90/+30/-30/-90), compassion ALTERNATING (-75/+75/-75/+75),
    // and industry GROUPED (-80/-80/+80/+80, the first pair idle, the second keen), so each
    // colonist is a distinct FOUR-axis combo (a cowardly-greedy-callous-idle one, a brave-selfless-
    // compassionate-keen one, ...), not four clones on one dial. Pure index expressions, NO rng
    // draw, so the seeded streams stay bit-aligned. This hand-authored spread is the OPENING
    // showcase; ongoing reinforcements instead roll a coherent archetype + jitter (roll_archetype).
    make_npc(reg, pos, Vec2{vel(rng), vel(rng)}, (i * 2 - 3) * 30, (3 - i * 2) * 30,
             ((i % 2) * 2 - 1) * 75, ((i / 2) * 2 - 1) * 80);
  }

  // Two hostile creatures at opposite corners that hunt the nearest person (you or an
  // NPC) — one of each kind so both archetypes show from the start. Strike them (J) to
  // wear their HP down; a stronger Strength kills faster. The spawner keeps a mix coming.
  make_brute(reg, Vec2{kFieldWidth * 0.2f, kFieldHeight * 0.2f});
  make_swarmer(reg, Vec2{kFieldWidth * 0.8f, kFieldHeight * 0.8f});

  // A couple of armour pieces on the field (dull-bronze dots) — walk onto one and press E to
  // don it for +defence at the cost of a slower second wind, the defensive twin of a weapon.
  // A colonist may beat you to one (npc_equip), the same as with weapons.
  spawn_armour(reg, Vec2{center.x + 90.0f, center.y - 40.0f});
  spawn_armour(reg, Vec2{kFieldWidth * 0.6f, kFieldHeight * 0.35f});
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
  drain_water(registry_, dt);      // ...and thirsty; dehydrating (0) chips health the same way
  advance_progression(registry_);  // activity -> skill+attribute XP -> level -> bigger pools
  wrap_bounds(registry_, Vec2{kFieldWidth, kFieldHeight});
  // Collision runs after movement (positions are current), then death is checked
  // from any damage it dealt, then survivors regenerate. This order is the
  // definition of the tick — collision before death before heal.
  resolve_contacts(registry_);                     // motes shatter on contact
  resolve_creature_contacts(registry_, dt, rng_);  // creatures swing; player may dodge (DEX)
  tick_poison(registry_, dt);                      // venom from a swarmer's bite chips health...
  handle_deaths(registry_, Vec2{kFieldWidth * 0.5f, kFieldHeight * 0.5f},
                dt);               // ...then 0-HP reaped
  collect_pickups(registry_, dt);  // grab health orbs the slain creatures dropped; fade old ones
  drink(registry_, dt);            // anyone standing in a water source refills their canteen
  graze(registry_, dt);  // ...and in a food plot refills hunger (the plot regrows/depletes)
  npc_equip(registry_);  // unarmed NPCs wield a dropped weapon they've reached
  regenerate_vitals(registry_, dt);
  decay_flashes(registry_, dt);  // age the hit-flashes left by this tick's blows (presentation)

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
        float speed =
            view.get<Stats>(e).stamina.current > 0.0f ? pc.move_speed : pc.move_speed * 0.4f;
        // A wielded weapon's heft slows you — the equip tradeoff, felt on every step, and it
        // stacks with the exhaustion crawl (so a tired, heavily-armed player really trudges).
        if (const Equipped* gear = registry_.try_get<Equipped>(e); gear != nullptr) {
          speed *= 1.0f - gear->move_penalty;
        }
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
    case CommandKind::Equip: {
      // Wear the nearest dropped gear in reach (weapon or armour) — the target is computed
      // server-side via the shared equip_nearest_gear (the same fold NPCs use, so gear can't
      // diverge). Match the commanding player, skip a downed one, and collect-then-destroy so a
      // co-op pair can't invalidate the view mid-loop.
      std::vector<entt::entity> consumed;
      auto players = registry_.view<PlayerControlled, Transform>();
      for (const entt::entity p : players) {
        if (players.get<PlayerControlled>(p).player != cmd.player) continue;
        if (registry_.all_of<Downed>(p)) continue;  // helpless — can't equip
        const entt::entity w = equip_nearest_gear(registry_, p);
        if (w != entt::null) consumed.push_back(w);
      }
      for (const entt::entity w : consumed) {
        if (registry_.valid(w)) registry_.destroy(w);
      }
      break;
    }
    case CommandKind::Drop: {
      // Ditch the wielded WEAPON at your feet — the inverse of Equip, turning the heft bane you
      // took on into a choice you can undo (drop to sprint clear of a swarm, re-grab later). It
      // is SLOT-AWARE, the symmetric twin of the non-clobbering equip: it drops only the weapon
      // and zeroes only the weapon field-pair, leaving any worn armour untouched — a blanket
      // remove<Equipped> here would silently strip your armour. A player with no weapon (armour
      // only, or bare) is a no-op: no phantom weapon appears. Skip a downed player. Collect-
      // then-act: spawn_weapon creates a new entity (emplacing Transform), which could realloc
      // the Transform pool this view walks, so spawn + mutate AFTER the walk.
      std::vector<entt::entity> droppers;
      auto players = registry_.view<PlayerControlled, Transform, Equipped>();
      for (const entt::entity p : players) {
        if (players.get<PlayerControlled>(p).player != cmd.player) continue;
        if (registry_.all_of<Downed>(p)) continue;  // helpless — can't drop
        Equipped& eq = players.get<Equipped>(p);
        if (eq.strength_bonus == 0 && eq.move_penalty == 0.0f) continue;  // no weapon to shed
        droppers.push_back(p);
      }
      for (const entt::entity p : droppers) {
        spawn_weapon(registry_, registry_.get<Transform>(p).position);  // a weapon where you stand
        Equipped& eq = registry_.get<Equipped>(p);
        eq.strength_bonus = 0;  // clear ONLY the weapon slot; the heft is shed...
        eq.move_penalty = 0.0f;
        // ...and if nothing's left worn (no armour either), drop the now-empty cache entirely
        // so the "wielding" HUD clears and MovePlayer/perform_attack see a bare character.
        if (eq.defence_bonus == 0.0f && eq.stamina_regen_penalty == 0.0f) {
          registry_.remove<Equipped>(p);
        }
      }
      break;
    }
  }
}

}  // namespace eng::sim
