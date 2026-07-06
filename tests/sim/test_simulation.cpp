#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
  eng::sim::World world;  // the player spawns at 70/100 and heals 8/sec
  const entt::entity player = world.player();
  const float start = world.registry().get<eng::sim::Stats>(player).health.current;

  // Ten seconds of ticks: +8/sec for 10s = +80, so 70 -> 150, clamped to 100.
  for (int i = 0; i < 10 * eng::sim::kTicksPerSecond; ++i) world.step();

  const eng::sim::Vital& health = world.registry().get<eng::sim::Stats>(player).health;
  REQUIRE(health.current > start);                // it healed
  REQUIRE(health.current == Approx(health.max));  // and never overshot the cap
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

TEST_CASE("SpawnMote adds an entity to the world", "[sim]") {
  eng::sim::World world;
  const auto before = world.registry().storage<eng::sim::Transform>().size();

  world.submit(eng::sim::spawn_mote({100.0f, 100.0f}));
  world.step();

  const auto after = world.registry().storage<eng::sim::Transform>().size();
  REQUIRE(after == before + 1);
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
