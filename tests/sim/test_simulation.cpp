#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <random>

#include "engine/net/loopback.hpp"
#include "engine/net/server.hpp"
#include "engine/sim/components.hpp"
#include "engine/sim/simulation.hpp"
#include "engine/sim/systems.hpp"
#include "engine/sim/world.hpp"

// Headless tests for the simulation core. None of these open a window or touch
// the GPU — the sim is pure logic, so it's fully testable on a build server.
// This is the "test the simulation, not the rendering" split in action.

using Catch::Approx;

TEST_CASE("FixedTimestep runs one step per tick's worth of time", "[sim]") {
  eng::sim::FixedTimestep ts(1.0 / 60.0);

  REQUIRE(ts.advance(1.0 / 60.0) == 1);             // exactly one tick elapsed
  REQUIRE(ts.advance(2.5 / 60.0) == 2);             // 2.5 ticks -> run 2, keep 0.5 over
  REQUIRE(ts.alpha() == Approx(0.5).margin(1e-9));  // half a tick left to interpolate
}

TEST_CASE("FixedTimestep clamps a huge frame to avoid the spiral of death", "[sim]") {
  eng::sim::FixedTimestep ts(1.0 / 60.0, /*max_frame=*/0.25);

  // A 10-second stall (breakpoint / laptop sleep) would be 600 catch-up steps.
  // The clamp caps it at 0.25s of catch-up = 15 steps. Slow-mo, not a freeze.
  REQUIRE(ts.advance(10.0) == 15);
}

TEST_CASE("integrate_motion advances position by velocity * dt", "[sim]") {
  entt::registry reg;
  const entt::entity e = reg.create();
  reg.emplace<eng::sim::Transform>(e, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(e, eng::Vec2{60.0f, 0.0f});

  eng::sim::integrate_motion(reg, 0.5f);

  REQUIRE(reg.get<eng::sim::Transform>(e).position.x == Approx(30.0f));
}

TEST_CASE("a MovePlayer command moves the player through the funnel", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();
  const float start_x = world.registry().get<eng::sim::Transform>(player).position.x;

  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));  // push right
  world.step();

  const float now_x = world.registry().get<eng::sim::Transform>(player).position.x;
  REQUIRE(now_x > start_x);
}

TEST_CASE("a diagonal input does not exceed the player's move speed", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  // A raw diagonal (1, 1) has length 1.41. The funnel must clamp it so the
  // resulting speed equals move_speed, not move_speed * 1.41 (the classic
  // faster-on-the-diagonal bug).
  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 1.0f}));
  world.step();

  const eng::Vec2 vel = world.registry().get<eng::sim::Velocity>(player).value;
  const float speed = glm::length(vel);
  REQUIRE(speed == Approx(320.0f).margin(0.5f));  // 320 = the player's move_speed
}

TEST_CASE("regenerate_vitals heals the health vital over time, capped at max", "[sim]") {
  // Tested on regenerate_vitals directly, not the full World: the live scene now has
  // creatures that hunt and hurt the player, so "idle player heals to full" only
  // holds for the regen system in isolation.
  entt::registry reg;
  const entt::entity e = reg.create();
  reg.emplace<eng::sim::Stats>(e, eng::sim::Vital{70.0f, 100.0f, 8.0f});  // worn; heals 8/sec

  // Ten seconds: +8/sec for 10s = +80, so 70 -> 150, clamped to 100.
  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < 10 * eng::sim::kTicksPerSecond; ++i) eng::sim::regenerate_vitals(reg, dt);

  const eng::sim::Vital& health = reg.get<eng::sim::Stats>(e).health;
  REQUIRE(health.current > 70.0f);                // it healed
  REQUIRE(health.current == Approx(health.max));  // capped at 100, never overshot
}

TEST_CASE("moving drains the player's stamina", "[sim]") {
  eng::sim::World world;  // player spawns with full stamina (100)
  const entt::entity player = world.player();
  const float start = world.registry().get<eng::sim::Stats>(player).stamina.current;

  // Hold a direction for half a second of ticks. update_stamina spends stamina
  // every tick the player is moving, and doesn't recover it until they stop.
  for (int i = 0; i < eng::sim::kTicksPerSecond / 2; ++i) {
    world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
    world.step();
  }

  REQUIRE(world.registry().get<eng::sim::Stats>(player).stamina.current < start);
}

TEST_CASE("an exhausted player is slowed to a crawl", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  // Move long enough to empty the stamina bar (100 at 40/sec = 2.5s; give it 4).
  for (int i = 0; i < 4 * eng::sim::kTicksPerSecond; ++i) {
    world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
    world.step();
  }
  REQUIRE(world.registry().get<eng::sim::Stats>(player).stamina.current == Approx(0.0f));

  // One more move while exhausted: the funnel reads the empty stamina and sets a
  // reduced speed instead of the full 320 — you can still limp away, not sprint.
  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
  world.step();

  const float speed = glm::length(world.registry().get<eng::sim::Velocity>(player).value);
  REQUIRE(speed == Approx(320.0f * 0.4f).margin(0.5f));  // a crawl, not a sprint
}

TEST_CASE("staying active trains conditioning, which raises endurance and max health", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();
  const float base_max = world.registry().get<eng::sim::Stats>(player).health.max;  // 100 at start

  // Move continuously for several seconds: activity trains the conditioning skill,
  // which (once it levels) feeds Endurance, which grows the health pool.
  for (int i = 0; i < 6 * eng::sim::kTicksPerSecond; ++i) {
    world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
    world.step();
  }

  const eng::sim::Skills& skills = world.registry().get<eng::sim::Skills>(player);
  const eng::sim::Attributes& attr = world.registry().get<eng::sim::Attributes>(player);
  const eng::sim::Stats& stats = world.registry().get<eng::sim::Stats>(player);
  REQUIRE(skills.find(eng::sim::SkillId::Conditioning)->level >= 2);  // the skill leveled from use
  REQUIRE(attr.endurance.level >= 2);    // the attribute leveled in parallel (bonus >= 1)
  REQUIRE(stats.health.max > base_max);  // the attribute grew the pool
}

TEST_CASE("an idle character does not train conditioning", "[sim]") {
  // advance_progression on a lone idle entity (not the full World, where a creature
  // would eventually reach the idle player and train Toughness on it via its blows).
  entt::registry reg;
  const entt::entity e = reg.create();
  reg.emplace<eng::sim::Skills>(e);
  reg.emplace<eng::sim::Attributes>(e);
  reg.emplace<eng::sim::Stats>(e);     // spawns full stamina — nothing to recover
  reg.emplace<eng::sim::Velocity>(e);  // zero velocity — genuinely idle
  reg.emplace<eng::sim::CharacterLevel>(e);

  // Stand still for several seconds' worth of ticks.
  for (int i = 0; i < 6 * eng::sim::kTicksPerSecond; ++i) eng::sim::advance_progression(reg);

  const eng::sim::Skills& skills = reg.get<eng::sim::Skills>(e);
  const eng::sim::Skill* cond = skills.find(eng::sim::SkillId::Conditioning);
  REQUIRE(cond->level == 1);                                       // no activity, no training...
  REQUIRE(cond->xp == eng::Fixed{});                               // ...not even a sliver of XP
  REQUIRE(reg.get<eng::sim::Attributes>(e).endurance.level == 1);  // bonus 0
}

TEST_CASE("resting to recover spent stamina trains Recovery", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  // Spend stamina by moving for a second (spawns full at 100, drains 40/sec).
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) {
    world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
    world.step();
  }
  const float spent = world.registry().get<eng::sim::Stats>(player).stamina.current;
  REQUIRE(spent < 100.0f);  // stamina was actually spent

  // Now rest: a zero-move each tick stops the player (velocity persists otherwise),
  // so stamina recovers — and recovering spent stamina trains Recovery.
  for (int i = 0; i < 2 * eng::sim::kTicksPerSecond; ++i) {
    world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {0.0f, 0.0f}));
    world.step();
  }

  const eng::sim::Skill* recovery =
      world.registry().get<eng::sim::Skills>(player).find(eng::sim::SkillId::Recovery);
  REQUIRE(recovery != nullptr);          // resting-to-recover taught it
  REQUIRE(recovery->xp > eng::Fixed{});  // ...with XP
  REQUIRE(world.registry().get<eng::sim::Stats>(player).stamina.current >
          spent);  // and it recovered
}

TEST_CASE("sustained activity raises the character level, which compounds the pools", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  // Move for a good while. The Character Level earns only a quarter-share of the
  // activity, so it climbs slower than the skill — long enough here to cross into
  // level 2, where its POWER(level - 1) multiplier first bites.
  for (int i = 0; i < 25 * eng::sim::kTicksPerSecond; ++i) {
    world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
    world.step();
  }

  const eng::sim::CharacterLevel& character =
      world.registry().get<eng::sim::CharacterLevel>(player);
  const eng::sim::Attributes& attr = world.registry().get<eng::sim::Attributes>(player);
  const eng::sim::Stats& stats = world.registry().get<eng::sim::Stats>(player);

  REQUIRE(character.level >= 2);  // accrued from activity and leveled
  // The veteran multiplier scales the EARNED endurance bonus, so the pool is
  // strictly bigger than endurance alone (base 100 + bonus x 10/point) would give.
  const float endurance_only = 100.0f + static_cast<float>(attr.endurance.level - 1) * 10.0f;
  REQUIRE(stats.health.max > endurance_only);
}

TEST_CASE("taking damage trains Toughness, which feeds Endurance", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  // The player starts knowing only Conditioning; enduring hits should teach Toughness.
  REQUIRE(world.registry().get<eng::sim::Skills>(player).find(eng::sim::SkillId::Toughness) ==
          nullptr);

  // Take a few non-lethal hits through the funnel (spawns at 70/100 and heals, so
  // 3 x 10 damage won't down them). The player never moves, so any progression XP
  // here can only have come from the damage.
  for (int i = 0; i < 3; ++i) {
    world.submit(eng::sim::damage_player(eng::sim::kLocalPlayer, 10.0f));
    world.step();
  }

  const eng::sim::Skill* toughness =
      world.registry().get<eng::sim::Skills>(player).find(eng::sim::SkillId::Toughness);
  REQUIRE(toughness != nullptr);          // enduring hits taught it
  REQUIRE(toughness->xp > eng::Fixed{});  // ...and it is accruing XP
  REQUIRE(world.registry().get<eng::sim::Attributes>(player).endurance.xp > eng::Fixed{});
}

TEST_CASE("train_on_damage ignores non-finite and non-positive damage", "[sim]") {
  entt::registry reg;
  const entt::entity e = reg.create();
  reg.emplace<eng::sim::Skills>(e);
  reg.emplace<eng::sim::Attributes>(e);

  // train_on_damage is the funnel every damage source flows through, so garbage in
  // must be a no-op — never reaching the float->int cast (UB on NaN/Inf).
  eng::sim::train_on_damage(reg, e, std::numeric_limits<float>::quiet_NaN());
  eng::sim::train_on_damage(reg, e, std::numeric_limits<float>::infinity());
  eng::sim::train_on_damage(reg, e, -5.0f);
  eng::sim::train_on_damage(reg, e, 0.0f);
  REQUIRE(reg.get<eng::sim::Skills>(e).find(eng::sim::SkillId::Toughness) == nullptr);
  REQUIRE(reg.get<eng::sim::Attributes>(e).endurance.xp == eng::Fixed{});

  // ...but a real hit still trains it (the guard rejects only the bad input).
  eng::sim::train_on_damage(reg, e, 10.0f);
  REQUIRE(reg.get<eng::sim::Skills>(e).find(eng::sim::SkillId::Toughness) != nullptr);
}

TEST_CASE("attacking the nearest mote in reach destroys it and trains Striking -> Strength",
          "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();
  const eng::Vec2 pos = world.registry().get<eng::sim::Transform>(player).position;

  // Put a mote in reach — past contact range (15) but inside base reach (45) — and
  // let it settle for a tick so it isn't consumed by contact first.
  world.submit(eng::sim::spawn_mote(eng::Vec2{pos.x + 25.0f, pos.y}));
  world.step();
  const int motes_before = static_cast<int>(world.registry().storage<eng::sim::Hazard>().size());

  // Swing. Striking is granted ONLY by a connecting Attack, so XP on it proves the
  // strike found a target; Strength (its main attribute) climbs alongside.
  world.submit(eng::sim::attack(eng::sim::kLocalPlayer));
  world.step();

  const int motes_after = static_cast<int>(world.registry().storage<eng::sim::Hazard>().size());
  REQUIRE(motes_after < motes_before);  // the strike destroyed a mote

  const eng::sim::Skill* striking =
      world.registry().get<eng::sim::Skills>(player).find(eng::sim::SkillId::Striking);
  REQUIRE(striking != nullptr);
  REQUIRE(striking->xp > eng::Fixed{});
  REQUIRE(world.registry().get<eng::sim::Attributes>(player).strength.xp > eng::Fixed{});
}

TEST_CASE("an NPC strikes a hazard in reach and trains Striking -> Strength", "[sim]") {
  eng::sim::World world;

  // Grab any NPC and its spot. front() gives the first entity in the view (or null
  // if none) — avoids a for/break loop, which MSVC /W4 flags as unreachable code.
  const entt::entity npc = world.registry().view<eng::sim::Npc, eng::sim::Transform>().front();
  REQUIRE(world.registry().valid(npc));  // we actually found an NPC
  const eng::Vec2 npc_pos = world.registry().get<eng::sim::Transform>(npc).position;

  // Put a mote in that NPC's reach and step once: npc_attack strikes it. The NPC
  // may flee a step first, but 20 units is well inside reach (45), so it connects.
  world.submit(eng::sim::spawn_mote(eng::Vec2{npc_pos.x + 20.0f, npc_pos.y}));
  world.step();

  // Striking is granted ONLY by a connecting swing, so its XP proves the NPC fought
  // — closing the player-only gap: NPCs build Strength now, not just Endurance.
  const eng::sim::Skill* striking =
      world.registry().get<eng::sim::Skills>(npc).find(eng::sim::SkillId::Striking);
  REQUIRE(striking != nullptr);
  REQUIRE(world.registry().get<eng::sim::Attributes>(npc).strength.xp > eng::Fixed{});
}

TEST_CASE("a DamagePlayer command reduces health through the funnel", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();
  const float before = world.registry().get<eng::sim::Stats>(player).health.current;

  world.submit(eng::sim::damage_player(eng::sim::kLocalPlayer, 25.0f));
  world.step();  // applies the command, THEN regen runs the same tick

  // ~25 removed; the margin absorbs the sliver the regen system heals back in
  // the same step (8/sec over one 1/60 tick = 0.13).
  const float after = world.registry().get<eng::sim::Stats>(player).health.current;
  REQUIRE(after == Approx(before - 25.0f).margin(0.5f));
}

TEST_CASE("a lethal hit respawns the player at full health and the spawn point", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  // Move the player off-centre first, so the respawn visibly moves it back.
  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
  for (int i = 0; i < 30; ++i) world.step();
  REQUIRE(world.registry().get<eng::sim::Transform>(player).position.x >
          eng::sim::kFieldWidth * 0.5f);  // drifted right of centre

  world.submit(eng::sim::damage_player(eng::sim::kLocalPlayer, 9999.0f));  // lethal
  world.step();  // damage -> health hits 0 -> handle_deaths respawns before regen

  const eng::sim::Stats& stats = world.registry().get<eng::sim::Stats>(player);
  const eng::sim::Transform& tf = world.registry().get<eng::sim::Transform>(player);
  REQUIRE(stats.health.current == Approx(stats.health.max));       // back to full
  REQUIRE(tf.position.x == Approx(eng::sim::kFieldWidth * 0.5f));  // back to spawn
  REQUIRE(tf.position.y == Approx(eng::sim::kFieldHeight * 0.5f));
}

TEST_CASE("touching a hazard damages the player and consumes the hazard", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();
  entt::registry& reg = world.registry();
  const eng::Vec2 player_pos = reg.get<eng::sim::Transform>(player).position;

  // A stationary hazard sitting right on the player (no Velocity, so it stays).
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Transform>(hazard, player_pos);
  reg.emplace<eng::sim::Hazard>(hazard, 20.0f);  // 20 per hit — non-lethal (player has 70)

  const float before = reg.get<eng::sim::Stats>(player).health.current;
  world.step();

  REQUIRE(reg.get<eng::sim::Stats>(player).health.current < before);  // took the hit
  REQUIRE_FALSE(reg.valid(hazard));  // and the hazard was destroyed (consumed)
}

TEST_CASE("a hazard out of range does no damage and is not consumed", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();
  entt::registry& reg = world.registry();
  const eng::Vec2 player_pos = reg.get<eng::sim::Transform>(player).position;

  // A stationary hazard well out of range.
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Transform>(hazard, player_pos + eng::Vec2{500.0f, 0.0f});
  reg.emplace<eng::sim::Hazard>(hazard, 20.0f);

  const float before = reg.get<eng::sim::Stats>(player).health.current;
  world.step();

  // No contact: no damage (regen only nudges up), and the hazard survives.
  REQUIRE(reg.get<eng::sim::Stats>(player).health.current >= before);
  REQUIRE(reg.valid(hazard));
}

TEST_CASE("a dead NPC is destroyed (permadeath), not respawned", "[sim]") {
  eng::sim::World world;
  entt::registry& reg = world.registry();

  // A lone NPC with just enough health to die to a single hazard hit, off in a
  // corner away from the scene's own motes and NPCs.
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{10.0f, 10.0f});
  reg.emplace<eng::sim::Stats>(npc, eng::sim::Vital{10.0f, 100.0f, 0.0f});
  reg.emplace<eng::sim::Npc>(npc);

  // A stationary hazard sitting right on it deals 20 — lethal for a 10-health NPC.
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{10.0f, 10.0f});
  reg.emplace<eng::sim::Hazard>(hazard, 20.0f);

  world.step();  // contact drops the NPC to 0 -> handle_deaths destroys it

  // Unlike the player (who respawns on a lethal hit), the NPC is gone for good —
  // and the hazard that killed it was consumed.
  REQUIRE_FALSE(reg.valid(npc));
  REQUIRE_FALSE(reg.valid(hazard));
}

TEST_CASE("an NPC steers away from a nearby hazard", "[sim]") {
  entt::registry reg;

  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::Velocity>(npc);  // starts still

  // A hazard 50 units to the NPC's right — inside its 120-unit senses.
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Hazard>(hazard);
  reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{150.0f, 100.0f});

  eng::sim::steer_npcs(reg);

  // It should now be moving left — directly away from the hazard on its right.
  REQUIRE(reg.get<eng::sim::Velocity>(npc).value.x < 0.0f);
}

TEST_CASE("an NPC ignores a hazard beyond its senses", "[sim]") {
  entt::registry reg;

  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::Velocity>(npc, eng::Vec2{55.0f, 0.0f});  // already drifting right

  // A hazard 300 units away — well outside the 120-unit sense radius.
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Hazard>(hazard);
  reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{400.0f, 100.0f});

  eng::sim::steer_npcs(reg);

  // Out of range: no threat sensed, so the NPC's existing velocity is left
  // untouched — it drifts on. Asserting the exact prior value (not just "nonzero")
  // pins the "leave velocity alone" contract: a version that zeroed or otherwise
  // changed it here would fail, which a start-from-still test could never catch.
  const eng::Vec2 vel = reg.get<eng::sim::Velocity>(npc).value;
  REQUIRE(vel.x == 55.0f);
  REQUIRE(vel.y == 0.0f);
}

TEST_CASE("SpawnMote adds an entity to the world", "[sim]") {
  eng::sim::World world;

  // Spawn at (100, 100): a far corner, >100 units from every NPC (they sit at
  // 200,140 and beyond), so it's out of NPC attack reach and survives the tick.
  // (An absolute entity count is no longer stable — NPCs now strike motes that
  // drift into reach, so the mote population changes on its own; see npc_attack.)
  world.submit(eng::sim::spawn_mote({100.0f, 100.0f}));
  world.step();

  bool found = false;
  for (const entt::entity h : world.registry().view<eng::sim::Hazard, eng::sim::Transform>()) {
    if (glm::distance(world.registry().get<eng::sim::Transform>(h).position,
                      eng::Vec2{100.0f, 100.0f}) < 5.0f) {
      found = true;
      break;
    }
  }
  REQUIRE(found);  // the spawned mote exists where we put it
}

TEST_CASE("player input reaches the world through the transport seam", "[sim]") {
  eng::net::LoopbackTransport transport;
  eng::net::Server server(transport);
  const entt::entity player = server.world().player();
  const float start_x = server.world().registry().get<eng::sim::Transform>(player).position.x;

  // The client's side: send an input message. The server picks it up next tick —
  // exactly the path real multiplayer will take, just over a queue instead of UDP.
  transport.send(eng::net::Message{eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f})});
  server.tick();

  const float now_x = server.world().registry().get<eng::sim::Transform>(player).position.x;
  REQUIRE(now_x > start_x);
}

TEST_CASE("attacking a creature whittles its HP by Strength vs its VIT", "[sim]") {
  entt::registry reg;
  // A player-like attacker at the origin (Strength level 1 to start).
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  // A creature 20 units away (inside reach), 40 HP, some VIT for defence.
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe).endurance.level = 3;
  reg.emplace<eng::sim::Enemy>(foe);

  // One swing at base Strength: chips HP but does NOT one-shot it (a real fight).
  std::mt19937 rng{1234};  // foe is Dexterity 1 -> never dodges, so every strike lands
  eng::sim::perform_attack(reg, atk, rng);
  const float hp_after_one = reg.get<eng::sim::Stats>(foe).health.current;
  REQUIRE(hp_after_one < 40.0f);  // it took damage...
  REQUIRE(hp_after_one > 0.0f);   // ...but survived — multi-hit, not instant like a mote
  REQUIRE(reg.get<eng::sim::Skills>(atk).find(eng::sim::SkillId::Striking) != nullptr);
  REQUIRE(reg.get<eng::sim::Attributes>(atk).strength.xp >
          eng::Fixed{});  // the swing trained Strength

  // A much stronger attacker hits far harder against the same VIT.
  const float weak_hit = 40.0f - hp_after_one;
  reg.get<eng::sim::Attributes>(atk).strength.level = 10;
  const float before_strong = reg.get<eng::sim::Stats>(foe).health.current;
  eng::sim::perform_attack(reg, atk, rng);
  const float strong_hit = before_strong - reg.get<eng::sim::Stats>(foe).health.current;
  REQUIRE(strong_hit > weak_hit);  // Strength matters: the stronger swing deals more
}

TEST_CASE("a slippery high-Dexterity creature dodges some of your strikes", "[sim]") {
  // The offensive mirror of the player's dodge: a creature's DEX lets it slip some of
  // your strikes (capped at 50%), but a stream of hits still lands. Deterministic seed.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});  // inside base reach (45)
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe).dexterity.level = 18;  // past the 50% dodge cap
  reg.emplace<eng::sim::Enemy>(foe);

  std::mt19937 rng{1234};
  int dodges = 0;
  int hits = 0;
  for (int i = 0; i < 40; ++i) {
    // Reset HP each swing so the 0-floor never masks a hit as a dodge.
    reg.get<eng::sim::Stats>(foe).health.current = 40.0f;
    eng::sim::perform_attack(reg, atk, rng);
    if (reg.get<eng::sim::Stats>(foe).health.current == Approx(40.0f)) {
      ++dodges;
    } else {
      ++hits;
    }
  }
  REQUIRE(dodges > 0);  // it slipped some strikes...
  REQUIRE(hits > 0);    // ...but couldn't dodge them all
}

TEST_CASE("a lucky attacker crits for extra damage", "[sim]") {
  // Two identical attackers vs identical unkillable dummies over the same seeded stream:
  // one has high Luck (crits), one has none. Neither levels Strength here (no
  // advance_progression), so the base hit is the same 12 every swing — the lucky one only
  // differs by landing some 2x crits, so it deals strictly more damage overall.
  const auto total_damage = [](int luck_level) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk).luck.level = luck_level;
    reg.emplace<eng::sim::Skills>(atk);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});             // inside reach
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{1.0e6f, 1.0e6f, 0.0f});  // never dies
    reg.emplace<eng::sim::Attributes>(foe);  // DEX 1 -> never dodges, so every strike lands
    reg.emplace<eng::sim::Enemy>(foe);
    std::mt19937 rng{1234};
    const float before = reg.get<eng::sim::Stats>(foe).health.current;
    for (int i = 0; i < 40; ++i) eng::sim::perform_attack(reg, atk, rng);
    return before - reg.get<eng::sim::Stats>(foe).health.current;
  };
  REQUIRE(total_damage(18) > total_damage(1));  // Luck crits -> strictly more damage dealt
  // ...and an untrained attacker (LCK 1) never crits — its total is the flat base (40 * 12).
  REQUIRE(total_damage(1) == Approx(40.0f * 12.0f));
}

TEST_CASE("collecting loot trains Scavenging and Luck", "[sim]") {
  // The build loop: grabbing an orb trains Scavenging -> Luck, which (via perform_attack)
  // is what earns crits. Foraging the field is itself an offensive investment.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Stats>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::Attributes>(player);
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{0.0f, 0.0f});  // right on the player
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::collect_pickups(reg, 1.0f / 60.0f);

  const eng::sim::Skill* scav =
      reg.get<eng::sim::Skills>(player).find(eng::sim::SkillId::Scavenging);
  REQUIRE(scav != nullptr);
  REQUIRE(scav->xp.to_double() > 0.0);                                       // Scavenging trained
  REQUIRE(reg.get<eng::sim::Attributes>(player).luck.xp.to_double() > 0.0);  // ...and its Luck
  REQUIRE(!reg.valid(orb));                                                  // the orb was consumed
}

TEST_CASE("a creature's blow is softened by the player's VIT", "[sim]") {
  entt::registry reg;
  // A player in contact with a creature, with some VIT (Endurance) for defence.
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Stats>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::Attributes>(player).endurance.level = 3;
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe);  // attack_damage 15, ready to swing

  const float before = reg.get<eng::sim::Stats>(player).health.current;
  std::mt19937 rng{1234};  // default Dexterity (level 1) never dodges, so the blow lands
  eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);
  const float dealt = before - reg.get<eng::sim::Stats>(player).health.current;
  REQUIRE(dealt > 0.0f);   // the creature landed a blow
  REQUIRE(dealt < 15.0f);  // ...but VIT softened it below the raw 15
}

TEST_CASE("facing a creature's swing trains Evasion and Dexterity, even when it lands", "[sim]") {
  // The bootstrap: at Dexterity 1 you can't dodge yet, so the blow lands — but facing
  // it still trains Evasion and its Dexterity, which is what eventually lets you dodge.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Stats>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::Attributes>(player);  // default Dexterity level 1 -> 0% dodge
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe);

  std::mt19937 rng{1234};
  eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);

  REQUIRE(reg.get<eng::sim::Stats>(player).health.current < 100.0f);  // it landed (no dodge yet)
  const eng::sim::Skill* evasion =
      reg.get<eng::sim::Skills>(player).find(eng::sim::SkillId::Evasion);
  REQUIRE(evasion != nullptr);
  REQUIRE(evasion->xp.to_double() > 0.0);                                         // Evasion trained
  REQUIRE(reg.get<eng::sim::Attributes>(player).dexterity.xp.to_double() > 0.0);  // ...and its DEX
}

TEST_CASE("a high-Dexterity player dodges some blows but not all", "[sim]") {
  // Evasion softens the incoming stream but never negates it (capped at 50%): over many
  // swings a trained dodger slips some and eats others. Deterministic from the seed.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Stats>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::Attributes>(player).dexterity.level = 18;  // past the 50% dodge cap
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe);

  std::mt19937 rng{1234};
  int dodges = 0;
  int hits = 0;
  for (int i = 0; i < 40; ++i) {
    // Reset health each swing so the 0-floor never masks a hit as a dodge; dt = 1s
    // clears the 0.8s cooldown so every call is a fresh committed swing.
    reg.get<eng::sim::Stats>(player).health.current = 100.0f;
    eng::sim::resolve_creature_contacts(reg, 1.0f, rng);
    if (reg.get<eng::sim::Stats>(player).health.current == Approx(100.0f)) {
      ++dodges;
    } else {
      ++hits;
    }
  }
  REQUIRE(dodges > 0);  // some blows slipped...
  REQUIRE(hits > 0);    // ...but a stream of hits still lands — never invulnerable
}

TEST_CASE("motes drift through creatures without hurting them", "[sim]") {
  entt::registry reg;
  // A creature and a mote sitting on the same spot (well within contact range).
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe);
  const entt::entity mote = reg.create();
  reg.emplace<eng::sim::Transform>(mote, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Hazard>(mote);

  eng::sim::resolve_contacts(reg);

  // A creature is fought with strikes (STR-vs-VIT), not chipped by ambient motes: it
  // takes no damage, and the mote isn't consumed — it drifts on.
  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current == Approx(40.0f));
  REQUIRE(reg.valid(mote));
}

TEST_CASE("the world respawns creatures after they're cleared, up to a cap", "[sim]") {
  eng::sim::World world;
  entt::registry& reg = world.registry();

  // Wipe every creature to empty the fight. NPCs fight creatures too (shared
  // perform_attack — a nice emergent ally) AND the colony now refills its own NPCs,
  // so to isolate the CREATURE spawner we keep NPCs cleared every tick below; otherwise
  // reinforcing NPCs would cull the spawns and confound the count.
  std::vector<entt::entity> doomed;
  for (const entt::entity e : reg.view<eng::sim::Enemy>()) doomed.push_back(e);
  for (const entt::entity e : reg.view<eng::sim::Npc>()) doomed.push_back(e);
  for (const entt::entity e : doomed) reg.destroy(e);
  REQUIRE(reg.storage<eng::sim::Enemy>().size() == 0);  // the fight is empty

  // Run well past several spawn intervals: the spawner refills the fight and then
  // holds at the cap — reinforcements, not an unbounded flood. Clear NPCs each tick so
  // the creature spawner is measured in isolation (see the colony-refill test for NPCs).
  for (int i = 0; i < 60 * eng::sim::kTicksPerSecond; ++i) {
    std::vector<entt::entity> npcs;
    for (const entt::entity e : reg.view<eng::sim::Npc>()) npcs.push_back(e);
    for (const entt::entity e : npcs) reg.destroy(e);
    world.step();
  }

  REQUIRE(static_cast<int>(reg.storage<eng::sim::Enemy>().size()) == eng::sim::kMaxCreatures);
}

TEST_CASE("the colony refills its NPCs after they're lost, up to a cap", "[sim]") {
  eng::sim::World world;
  entt::registry& reg = world.registry();

  // Wipe every colonist (all lost). We keep the field non-lethal below — no creatures,
  // no hazardous motes — so the refilled NPCs can't be culled, isolating the NPC spawner
  // (the mirror of the creature-cap test). The colony should refill on its own timer.
  std::vector<entt::entity> lost;
  for (const entt::entity e : reg.view<eng::sim::Npc>()) lost.push_back(e);
  for (const entt::entity e : lost) reg.destroy(e);
  REQUIRE(reg.storage<eng::sim::Npc>().size() == 0);

  // Run well past several NPC-spawn intervals. Each tick, clear creatures and motes so
  // nothing kills the recovering colony (the creature spawner keeps adding creatures,
  // so this must be every tick, not once).
  for (int i = 0; i < 90 * eng::sim::kTicksPerSecond; ++i) {
    std::vector<entt::entity> threats;
    for (const entt::entity e : reg.view<eng::sim::Enemy>()) threats.push_back(e);
    for (const entt::entity e : reg.view<eng::sim::Hazard>()) threats.push_back(e);
    for (const entt::entity e : threats) reg.destroy(e);
    world.step();
  }

  // Refilled to the cap and held there — reinforcements, not an unbounded flood.
  REQUIRE(static_cast<int>(reg.storage<eng::sim::Npc>().size()) == eng::sim::kMaxNpcs);
}

TEST_CASE("a slain creature drops a health pickup", "[sim]") {
  entt::registry reg;
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{0.0f, 40.0f, 0.0f});  // already dead (0 HP)
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::handle_deaths(reg, eng::Vec2{0.0f, 0.0f});

  REQUIRE(!reg.valid(foe));                              // reaped for good (permadeath)
  REQUIRE(reg.storage<eng::sim::Pickup>().size() == 1);  // ...and it left loot
}

TEST_CASE("a player collects a pickup: heals and grows max HP", "[sim]") {
  entt::registry reg;
  const entt::entity p = reg.create();
  reg.emplace<eng::sim::Transform>(p, eng::Vec2{50.0f, 50.0f});
  reg.emplace<eng::sim::PlayerControlled>(p);
  reg.emplace<eng::sim::Stats>(p, eng::sim::Vital{40.0f, 100.0f, 0.0f});  // hurt: 40/100
  const entt::entity item = reg.create();
  reg.emplace<eng::sim::Transform>(item, eng::Vec2{50.0f, 50.0f});  // right on the player
  reg.emplace<eng::sim::Pickup>(item);                              // heals 25, +2 max HP

  eng::sim::collect_pickups(reg, 1.0f / 60.0f);

  const eng::sim::Vital& health = reg.get<eng::sim::Stats>(p).health;
  REQUIRE(health.current == Approx(65.0f));  // 40 + 25 healed
  REQUIRE(health.max == Approx(102.0f));     // ...and a permanent +2 max-HP bump
  REQUIRE(!reg.valid(item));                 // consumed
}

TEST_CASE("an uncollected pickup fades after its lifetime", "[sim]") {
  entt::registry reg;
  // A pickup with no player anywhere near — it should time out and vanish rather
  // than pile up forever.
  const entt::entity item = reg.create();
  reg.emplace<eng::sim::Transform>(item, eng::Vec2{500.0f, 500.0f});
  reg.emplace<eng::sim::Pickup>(item);  // lifetime 20s

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < 25 * eng::sim::kTicksPerSecond; ++i) eng::sim::collect_pickups(reg, dt);

  REQUIRE(!reg.valid(item));  // it faded
  REQUIRE(reg.storage<eng::sim::Pickup>().size() == 0);
}

TEST_CASE("creatures chase at their own speed (brute vs swarmer)", "[sim]") {
  entt::registry reg;
  const entt::entity p = reg.create();
  reg.emplace<eng::sim::Transform>(p, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(p);
  reg.emplace<eng::sim::Stats>(p);  // prey = things with a Stats sheet (see chase_prey)
  // Two creatures the same distance out, with different chase speeds.
  const entt::entity slow = reg.create();
  reg.emplace<eng::sim::Transform>(slow, eng::Vec2{100.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(slow);
  reg.emplace<eng::sim::Enemy>(slow).chase_speed = 70.0f;
  const entt::entity fast = reg.create();
  reg.emplace<eng::sim::Transform>(fast, eng::Vec2{100.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(fast);
  reg.emplace<eng::sim::Enemy>(fast).chase_speed = 140.0f;

  eng::sim::chase_prey(reg);

  // Each homes in at its own chase_speed — the swarmer closes twice as fast.
  REQUIRE(glm::length(reg.get<eng::sim::Velocity>(slow).value) == Approx(70.0f));
  REQUIRE(glm::length(reg.get<eng::sim::Velocity>(fast).value) == Approx(140.0f));
}

TEST_CASE("a creature chases the nearest person, player or NPC", "[sim]") {
  entt::registry reg;
  // A player far to the right; an NPC close above; a creature at the origin.
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{1000.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Stats>(player);
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 20.0f});
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc);
  const entt::entity creature = reg.create();
  reg.emplace<eng::sim::Transform>(creature, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(creature);
  reg.emplace<eng::sim::Enemy>(creature);  // default chase_speed 70

  eng::sim::chase_prey(reg);

  // The NPC (20 units up) is far nearer than the player (1000 right), so the creature
  // steers straight UP toward the NPC — not sideways toward the player.
  const eng::Vec2 v = reg.get<eng::sim::Velocity>(creature).value;
  REQUIRE(v.y == Approx(70.0f));  // homing on the NPC, at chase_speed
  REQUIRE(v.x == Approx(0.0f));   // not a whisker toward the far-off player
}

TEST_CASE("a creature's blow harms an NPC too, not just the player", "[sim]") {
  // player == NPC parity: an NPC caught by a creature takes the same VIT-softened blow
  // and trains the same way, because both flow through the one prey path in
  // resolve_creature_contacts.
  entt::registry reg;
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc);
  reg.emplace<eng::sim::Skills>(npc);
  reg.emplace<eng::sim::Attributes>(npc);  // default Dexterity 1 -> no dodge, so the blow lands
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe);

  const float before = reg.get<eng::sim::Stats>(npc).health.current;
  std::mt19937 rng{1234};
  eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);

  REQUIRE(reg.get<eng::sim::Stats>(npc).health.current < before);  // the NPC took the hit
  // ...and facing the swing trained its Evasion -> Dexterity, exactly as for the player.
  const eng::sim::Skill* evasion = reg.get<eng::sim::Skills>(npc).find(eng::sim::SkillId::Evasion);
  REQUIRE(evasion != nullptr);
  REQUIRE(evasion->xp.to_double() > 0.0);
}

TEST_CASE("the two archetypes spawn with their own numbers (brute vs swarmer)", "[sim]") {
  // make_brute/make_swarmer are file-local, but a fresh World seeds exactly one of
  // each. Pin their HP/speed/damage here: make_creature takes three adjacent float
  // params (hp, chase_speed, attack_damage) that a future archetype could transpose
  // silently — this is the guard that would catch it.
  eng::sim::World world;
  entt::registry& reg = world.registry();

  bool saw_brute = false;
  bool saw_swarmer = false;
  for (const entt::entity e : reg.view<eng::sim::Enemy>()) {
    const float hp = reg.get<eng::sim::Stats>(e).health.max;
    const eng::sim::Enemy& en = reg.get<eng::sim::Enemy>(e);
    const int dex = reg.get<eng::sim::Attributes>(e).dexterity.level;
    if (hp == Approx(40.0f)) {  // brute: tanky, slow, hits hard — but never dodges
      saw_brute = true;
      REQUIRE(en.chase_speed == Approx(70.0f));
      REQUIRE(en.attack_damage == Approx(15.0f));
      REQUIRE(dex == 1);  // default Dexterity -> dodge_chance 0
    } else {              // swarmer: fragile, fast, weak — and slippery
      saw_swarmer = true;
      REQUIRE(hp == Approx(15.0f));
      REQUIRE(en.chase_speed == Approx(130.0f));
      REQUIRE(en.attack_damage == Approx(8.0f));
      REQUIRE(dex == 8);  // innate Dexterity -> ~21% chance to dodge a strike
    }
  }
  REQUIRE(saw_brute);
  REQUIRE(saw_swarmer);
}
