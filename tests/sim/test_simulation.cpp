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

TEST_CASE("hunger drains over time and never self-recovers", "[sim]") {
  entt::registry reg;
  const entt::entity e = reg.create();
  reg.emplace<eng::sim::Stats>(e);  // hunger starts full (100)

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < 10 * eng::sim::kTicksPerSecond; ++i) eng::sim::drain_hunger(reg, dt);

  const float hunger = reg.get<eng::sim::Stats>(e).hunger.current;
  REQUIRE(hunger < 100.0f);  // it fell (no regen ever adds it back)...
  REQUIRE(hunger > 0.0f);    // ...but a gentle drain hasn't emptied it in 10s
}

TEST_CASE("hunger drains faster while moving than at rest", "[sim]") {
  entt::registry reg;
  const entt::entity rester = reg.create();
  reg.emplace<eng::sim::Stats>(rester);
  reg.emplace<eng::sim::Velocity>(rester);  // zero velocity — at rest
  const entt::entity mover = reg.create();
  reg.emplace<eng::sim::Stats>(mover);
  reg.emplace<eng::sim::Velocity>(mover, eng::Vec2{50.0f, 0.0f});  // moving = exerting

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < 10 * eng::sim::kTicksPerSecond; ++i) eng::sim::drain_hunger(reg, dt);

  REQUIRE(reg.get<eng::sim::Stats>(mover).hunger.current <
          reg.get<eng::sim::Stats>(rester).hunger.current);  // exertion costs extra hunger
}

TEST_CASE("an empty stomach starves health", "[sim]") {
  entt::registry reg;
  const entt::entity e = reg.create();
  auto& stats = reg.emplace<eng::sim::Stats>(e);
  stats.hunger.current = 0.0f;  // already starving

  const float before = stats.health.current;  // full (100)
  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::drain_hunger(reg, dt);  // 1 second

  // Starvation chips health (through the normal path — handle_deaths reaps it at 0).
  REQUIRE(reg.get<eng::sim::Stats>(e).health.current < before);
}

TEST_CASE("a starving character cannot heal, so starvation always nets health down", "[sim]") {
  // The invariant the starve-gate hardens: even a high-Endurance character whose BOOSTED
  // regen (13.6/s) would out-pace starvation (12/s) still loses health while starving,
  // because regenerate_vitals skips healing at hunger 0. Without the gate this entity would
  // net upward (+1.6/s) and never die — the gate makes starvation lethal at any regen rate.
  entt::registry reg;
  const entt::entity e = reg.create();
  auto& s = reg.emplace<eng::sim::Stats>(e);
  s.health = eng::sim::Vital{50.0f, 100.0f, 8.0f};           // wounded, heals 8/s base
  s.hunger.current = 0.0f;                                   // starving
  reg.emplace<eng::sim::Attributes>(e).endurance.level = 8;  // boost 1.7 -> 13.6/s, would beat 12/s

  const float before = s.health.current;
  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) {  // one second, the real tick order
    eng::sim::drain_hunger(reg, dt);                     // chips health while at 0 hunger...
    eng::sim::regenerate_vitals(reg, dt);                // ...and heal is gated off, so no clawback
  }
  REQUIRE(reg.get<eng::sim::Stats>(e).health.current < before);  // strictly down despite high VIT
}

TEST_CASE("health regen scales with Endurance (VIT) when fed", "[sim]") {
  // The other half: a FED tougher character heals faster — VIT now speeds HP regen, mirroring
  // how it already speeds stamina recovery. Two wounded, fed entities differing only in Endurance.
  entt::registry reg;
  const entt::entity tough = reg.create();
  reg.emplace<eng::sim::Stats>(tough).health = eng::sim::Vital{50.0f, 100.0f, 8.0f};
  reg.emplace<eng::sim::Attributes>(tough).endurance.level = 8;  // hardy -> faster heal
  const entt::entity frail = reg.create();
  reg.emplace<eng::sim::Stats>(frail).health = eng::sim::Vital{50.0f, 100.0f, 8.0f};
  reg.emplace<eng::sim::Attributes>(frail);  // Endurance 1 -> base rate (boost 1.0)
  // (Both spawn with full default hunger, so the starve-gate never fires.)

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::regenerate_vitals(reg, dt);  // 1s

  const float tough_hp = reg.get<eng::sim::Stats>(tough).health.current;
  const float frail_hp = reg.get<eng::sim::Stats>(frail).health.current;
  REQUIRE(frail_hp > 50.0f);     // the frail one still healed at the base rate...
  REQUIRE(tough_hp > frail_hp);  // ...but the hardy one healed strictly more
  REQUIRE(tough_hp < 100.0f);    // and neither hit the cap (so the comparison is real)
}

TEST_CASE("eating a loot orb refills hunger", "[sim]") {
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  auto& stats = reg.emplace<eng::sim::Stats>(player);
  stats.hunger.current = 20.0f;  // peckish
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{0.0f, 0.0f});  // right on the player
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::collect_pickups(reg, 1.0f / 60.0f);

  REQUIRE(reg.get<eng::sim::Stats>(player).hunger.current > 20.0f);  // the orb fed you
  REQUIRE(!reg.valid(orb));                                          // and was consumed
}

TEST_CASE("a hungry NPC eats a food orb too, like the player", "[sim]") {
  entt::registry reg;
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Npc>(npc);
  auto& stats = reg.emplace<eng::sim::Stats>(npc);
  stats.hunger.current = 20.0f;  // hungry
  stats.health.current = 50.0f;  // and hurt, so the orb's heal is visible
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{0.0f, 0.0f});  // right on the NPC
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::collect_pickups(reg, 1.0f / 60.0f);

  REQUIRE(reg.get<eng::sim::Stats>(npc).hunger.current > 20.0f);  // fed (parity with the player)...
  REQUIRE(reg.get<eng::sim::Stats>(npc).health.current > 50.0f);  // ...and healed
  REQUIRE(!reg.valid(orb));                                       // consumed
}

TEST_CASE("a hungry NPC steers toward a nearby food orb", "[sim]") {
  entt::registry reg;
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(npc);
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc).hunger.current = 10.0f;  // below the seek threshold
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{100.0f, 0.0f});  // to the right, in forage range
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::steer_npcs(reg);

  const eng::Vec2 v = reg.get<eng::sim::Velocity>(npc).value;
  REQUIRE(v.x > 0.0f);           // heading toward the orb (its first want-driven motion)...
  REQUIRE(v.y == Approx(0.0f));  // ...straight at it
}

TEST_CASE("fear beats hunger: a threatened NPC flees rather than forage", "[sim]") {
  entt::registry reg;
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(npc);
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc).hunger.current = 10.0f;  // hungry...
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Transform>(hazard,
                                   eng::Vec2{50.0f, 0.0f});  // ...but a threat is close, right...
  reg.emplace<eng::sim::Hazard>(hazard);
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{100.0f, 0.0f});  // ...and food is that way too
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::steer_npcs(reg);

  // It flees LEFT (away from the hazard), not right toward the food — fear wins.
  REQUIRE(reg.get<eng::sim::Velocity>(npc).value.x < 0.0f);
}

TEST_CASE("bravery shapes how close a hazard gets before an NPC flees", "[sim]") {
  // The first personality axis, read by steer_npcs' flee radius. Three NPCs all 100 units from
  // one hazard (base sense radius is 120): a brave one (+100 -> radius 60) HOLDS (100 > 60), a
  // neutral one (no Personality -> base 120) flees (100 < 120), a coward (-100 -> radius 180)
  // flees too. Same distance, opposite reactions — and bravery 0/absent is the unchanged base.
  entt::registry reg;
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Hazard>(hazard);

  const entt::entity brave = reg.create();
  reg.emplace<eng::sim::Transform>(brave, eng::Vec2{100.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(brave);  // starts at rest
  reg.emplace<eng::sim::Npc>(brave);
  reg.emplace<eng::sim::Personality>(brave, eng::sim::Personality{100});  // brave -> radius 60

  const entt::entity neutral = reg.create();
  reg.emplace<eng::sim::Transform>(neutral, eng::Vec2{0.0f, 100.0f});
  reg.emplace<eng::sim::Velocity>(neutral);
  reg.emplace<eng::sim::Npc>(neutral);  // NO Personality -> base radius 120

  const entt::entity coward = reg.create();
  reg.emplace<eng::sim::Transform>(coward, eng::Vec2{-100.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(coward);
  reg.emplace<eng::sim::Npc>(coward);
  reg.emplace<eng::sim::Personality>(coward, eng::sim::Personality{-100});  // coward -> radius 180

  eng::sim::steer_npcs(reg);

  REQUIRE(reg.get<eng::sim::Velocity>(brave).value.x == Approx(0.0f));  // never sensed it...
  REQUIRE(reg.get<eng::sim::Velocity>(brave).value.y == Approx(0.0f));  // ...still at rest
  REQUIRE(reg.get<eng::sim::Velocity>(neutral).value.y > 0.0f);  // neutral fled (base radius), away
  REQUIRE(reg.get<eng::sim::Velocity>(coward).value.x < 0.0f);   // and the coward fled early too
}

TEST_CASE("bravery shapes how far an NPC will commit to a rescue", "[sim]") {
  // Bravery's SECOND read, exercising BOTH directions against the base rescue radius (300):
  //  - GROW: a brave NPC (+100 -> radius 450) at 350 rescues, where a NEUTRAL one (300) could not;
  //  - SHRINK: at 250 a neutral NPC rescues (250 < 300) but a coward (-100 -> radius 150) does NOT.
  // So each personality is tested at a distance that DISTINGUISHES it from neutral — the coward
  // stays put precisely because the shrink pulled its radius below 250. Sign is opposite the flee
  // radius: brave COMMITS further here, but HOLDS (shorter flee radius) against a hazard.
  entt::registry reg;
  const entt::entity fallen = reg.create();
  reg.emplace<eng::sim::Transform>(fallen, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Downed>(fallen);  // a helpless ally to be rescued

  const entt::entity brave = reg.create();
  reg.emplace<eng::sim::Transform>(brave, eng::Vec2{350.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(brave);  // starts at rest
  reg.emplace<eng::sim::Npc>(brave);
  reg.emplace<eng::sim::Personality>(brave,
                                     eng::sim::Personality{100});  // radius 450 > 350 -> goes

  const entt::entity neutral = reg.create();
  reg.emplace<eng::sim::Transform>(neutral, eng::Vec2{0.0f, 250.0f});
  reg.emplace<eng::sim::Velocity>(neutral);
  reg.emplace<eng::sim::Npc>(neutral);  // NO Personality -> base radius 300 > 250 -> goes

  const entt::entity coward = reg.create();
  reg.emplace<eng::sim::Transform>(coward, eng::Vec2{0.0f, -250.0f});
  reg.emplace<eng::sim::Velocity>(coward);
  reg.emplace<eng::sim::Npc>(coward);
  reg.emplace<eng::sim::Personality>(coward,
                                     eng::sim::Personality{-100});  // radius 150 < 250 -> won't

  eng::sim::steer_npcs(reg);

  REQUIRE(reg.get<eng::sim::Velocity>(brave).value.x <
          0.0f);  // brave crosses to the ally (west)...
  REQUIRE(reg.get<eng::sim::Velocity>(neutral).value.y <
          0.0f);  // ...neutral reaches it too (base)...
  REQUIRE(reg.get<eng::sim::Velocity>(coward).value.x ==
          Approx(0.0f));  // ...but the coward won't go
  REQUIRE(reg.get<eng::sim::Velocity>(coward).value.y == Approx(0.0f));
}

TEST_CASE("greed shapes how hungry an NPC must get before it forages", "[sim]") {
  // Personality's SECOND axis, reading a differently-shaped knob than bravery (a NEED THRESHOLD,
  // not a radius). Each personality sits at a hunger that DISTINGUISHES it from the base 0.6:
  //  - GROW: a greedy NPC (+100 -> 0.9) at 75% full forages, where the base (0.6) would NOT;
  //  - SHRINK: at 45% full a neutral forages, but a selfless one (-100 -> 0.3) does NOT.
  // The neutral-at-45 forager is the control proving the base reaches 45 — so the selfless NPC
  // staying put is genuinely the shrink, not a distance both thresholds fail.
  entt::registry reg;
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{100.0f, 0.0f});
  reg.emplace<eng::sim::Pickup>(orb);  // in forage range (260)

  const entt::entity greedy = reg.create();
  reg.emplace<eng::sim::Transform>(greedy, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(greedy);
  reg.emplace<eng::sim::Npc>(greedy);
  reg.emplace<eng::sim::Stats>(greedy).hunger.current =
      75.0f;  // 75% full: the base wouldn't forage
  reg.emplace<eng::sim::Personality>(greedy, eng::sim::Personality{0, 100});  // greed +100 -> 0.9

  const entt::entity neutral = reg.create();
  reg.emplace<eng::sim::Transform>(neutral, eng::Vec2{0.0f, 50.0f});
  reg.emplace<eng::sim::Velocity>(neutral);
  reg.emplace<eng::sim::Npc>(neutral);
  reg.emplace<eng::sim::Stats>(neutral).hunger.current = 45.0f;  // 45%, NO Personality -> base 0.6

  const entt::entity selfless = reg.create();
  reg.emplace<eng::sim::Transform>(selfless, eng::Vec2{0.0f, -50.0f});
  reg.emplace<eng::sim::Velocity>(selfless);
  reg.emplace<eng::sim::Npc>(selfless);
  reg.emplace<eng::sim::Stats>(selfless).hunger.current = 45.0f;  // same 45%...
  reg.emplace<eng::sim::Personality>(selfless,
                                     eng::sim::Personality{0, -100});  // ...but greed -100 -> 0.3

  eng::sim::steer_npcs(reg);

  REQUIRE(reg.get<eng::sim::Velocity>(greedy).value.x > 0.0f);   // greedy hoards at 75% (grow)...
  REQUIRE(reg.get<eng::sim::Velocity>(neutral).value.x > 0.0f);  // ...the base forages at 45%...
  REQUIRE(reg.get<eng::sim::Velocity>(selfless).value.x ==
          Approx(0.0f));  // ...but the selfless won't
  REQUIRE(reg.get<eng::sim::Velocity>(selfless).value.y ==
          Approx(0.0f));  // (shrink: it leaves the orb)
}

TEST_CASE("compassion shapes how fast an NPC rushes to a rescue", "[sim]") {
  // The third axis, reading a THIRD knob-shape: rescue SPEED (bravery sets the rescue RADIUS,
  // greed a need threshold). A compassionate colonist SPRINTS to a fallen ally; a neutral one
  // trudges at the base rescue speed. Both commit (well inside the base radius, outside revive
  // range) — only the speed differs, which is what a distance both reach cannot show alone.
  entt::registry reg;
  const entt::entity fallen = reg.create();
  reg.emplace<eng::sim::Transform>(fallen, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Downed>(fallen);  // a helpless ally to rush

  const entt::entity kind = reg.create();
  reg.emplace<eng::sim::Transform>(kind, eng::Vec2{200.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(kind);
  reg.emplace<eng::sim::Npc>(kind);
  reg.emplace<eng::sim::Personality>(kind, eng::sim::Personality{0, 0, 100});  // compassion +100

  const entt::entity plain = reg.create();
  reg.emplace<eng::sim::Transform>(plain, eng::Vec2{150.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(plain);
  reg.emplace<eng::sim::Npc>(plain);  // NO Personality -> base rescue speed (unchanged)

  const entt::entity cold = reg.create();
  reg.emplace<eng::sim::Transform>(cold, eng::Vec2{100.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(cold);
  reg.emplace<eng::sim::Npc>(cold);
  reg.emplace<eng::sim::Personality>(cold, eng::sim::Personality{0, 0, -100});  // compassion -100

  eng::sim::steer_npcs(reg);

  // All three commit and steer toward the ally (leftward, -x); only the SPEED differs, ranked by
  // compassion (more negative x = faster). Speed is independent of their distance.
  REQUIRE(reg.get<eng::sim::Velocity>(plain).value.x < 0.0f);  // the neutral one is on its way...
  REQUIRE(reg.get<eng::sim::Velocity>(kind).value.x <
          reg.get<eng::sim::Velocity>(plain).value.x);  // ...the compassionate one rushes faster...
  REQUIRE(reg.get<eng::sim::Velocity>(cold).value.x >
          reg.get<eng::sim::Velocity>(plain).value.x);  // ...and the callous one trudges slower...
  REQUIRE(reg.get<eng::sim::Velocity>(cold).value.x < 0.0f);  // (but is still, grudgingly, going)
}

TEST_CASE("an unarmed colonist steers toward a dropped weapon to arm up", "[sim]") {
  entt::registry reg;
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(npc);
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc);  // full hunger -> not foraging, so it falls through to arming
  const entt::entity weapon = reg.create();
  reg.emplace<eng::sim::Transform>(weapon,
                                   eng::Vec2{100.0f, 0.0f});  // in seek range, past grab reach
  reg.emplace<eng::sim::Weapon>(weapon);

  eng::sim::steer_npcs(reg);

  const eng::Vec2 v = reg.get<eng::sim::Velocity>(npc).value;
  REQUIRE(v.x > 0.0f);           // heading toward the blade...
  REQUIRE(v.y == Approx(0.0f));  // ...straight at it
}

TEST_CASE("an unarmed NPC arms itself from a dropped weapon in reach", "[sim]") {
  entt::registry reg;
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Npc>(npc);
  const entt::entity weapon = reg.create();
  reg.emplace<eng::sim::Transform>(weapon, eng::Vec2{5.0f, 0.0f});  // within the grab reach (30)
  reg.emplace<eng::sim::Weapon>(weapon);

  eng::sim::npc_equip(reg);

  REQUIRE(reg.all_of<eng::sim::Equipped>(npc));                   // now armed (parity)...
  REQUIRE(reg.get<eng::sim::Equipped>(npc).strength_bonus == 4);  // ...its mods...
  REQUIRE_FALSE(reg.valid(weapon));                               // ...and consumed
}

TEST_CASE("an armed NPC flees slower: the heft bane bites NPCs too", "[sim]") {
  // The parity crux: a weapon buffs an NPC's swing (shared perform_attack), so its bane MUST
  // slow the NPC too — else armed NPCs would be pure-upside. steer_npcs scales its speeds.
  const auto flee_speed = [](bool armed) {
    entt::registry reg;
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);
    if (armed) reg.emplace<eng::sim::Equipped>(npc, eng::sim::Equipped{4, 0.25f});
    const entt::entity hazard = reg.create();
    reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{50.0f, 0.0f});  // a close threat to flee
    reg.emplace<eng::sim::Hazard>(hazard);
    eng::sim::steer_npcs(reg);
    return glm::length(reg.get<eng::sim::Velocity>(npc).value);
  };
  REQUIRE(flee_speed(true) < flee_speed(false));  // armed = slower, exactly like the player
}

TEST_CASE("a creature does not eat a health orb", "[sim]") {
  // Guards the load-bearing exclude<Enemy> on the eater view: creatures carry Stats, but
  // must NOT heal or grow off the very orbs they drop, or the fight breaks.
  entt::registry reg;
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe);
  reg.emplace<eng::sim::Stats>(foe).health.current = 40.0f;  // hurt, so a wrongful heal would show
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{0.0f, 0.0f});  // right on the creature
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::collect_pickups(reg, 1.0f / 60.0f);

  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current == Approx(40.0f));  // no heal
  REQUIRE(reg.valid(orb));                                                 // orb untouched
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

TEST_CASE("enduring damage feeds the character level, not just movement", "[sim]") {
  // The Character Level is the "veteran" layer fed by a fraction of ALL activity, not
  // just walking. Enduring a blow is activity: it trains Toughness (via train_on_damage),
  // which now also feeds the character level through the one shared grant funnel — so a
  // fighter who only ever stands and takes hits still becomes a veteran. Before the funnel
  // routed it, combat was a dead path to the character level and this XP stayed flat at 0.
  entt::registry reg;
  const entt::entity e = reg.create();
  reg.emplace<eng::sim::Skills>(e);
  reg.emplace<eng::sim::Attributes>(e);
  reg.emplace<eng::sim::Stats>(e);
  reg.emplace<eng::sim::CharacterLevel>(e);

  REQUIRE(reg.get<eng::sim::CharacterLevel>(e).xp == eng::Fixed{});  // no activity yet

  // Endure several blows — no movement and no advance_progression, so the damage is the
  // ONLY possible XP source. The char XP moving off 0 proves combat now feeds it.
  for (int i = 0; i < 5; ++i) eng::sim::train_on_damage(reg, e, 20.0f);

  REQUIRE(reg.get<eng::sim::CharacterLevel>(e).xp > eng::Fixed{});  // combat made it a veteran
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

TEST_CASE("a lethal hit downs the player in place, not an instant respawn", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  // Move the player well off-centre first.
  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
  for (int i = 0; i < 30; ++i) world.step();
  const float fell_x = world.registry().get<eng::sim::Transform>(player).position.x;
  REQUIRE(fell_x > eng::sim::kFieldWidth * 0.5f);  // drifted right of centre

  world.submit(eng::sim::damage_player(eng::sim::kLocalPlayer, 9999.0f));  // lethal
  world.step();  // damage -> 0 HP -> handle_deaths DOWNS the player (no teleport, no heal)

  // Downed, not respawned: still at 0 HP, still out where they fell (right of centre) —
  // the old free teleport-to-safety is gone. (Rescue isn't checked the tick you go down,
  // so this is stable.) Position isn't exactly fell_x because residual velocity nudges the
  // player a little further before handle_deaths zeroes it — the point is it's NOT centre.
  REQUIRE(world.registry().all_of<eng::sim::Downed>(player));
  const eng::sim::Stats& stats = world.registry().get<eng::sim::Stats>(player);
  REQUIRE(stats.health.current == Approx(0.0f));  // helpless, not healed
  REQUIRE(world.registry().get<eng::sim::Transform>(player).position.x >
          eng::sim::kFieldWidth * 0.5f);  // still where they fell, not teleported to centre
}

TEST_CASE("an unrescued downed player respawns whole at the centre on expiry", "[sim]") {
  // With no ally in reach, the Downed timer runs out and the player respawns at the spawn
  // point — restored WHOLE (a starved/exhausted player mustn't come back still empty).
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{10.0f, 10.0f});  // far from the centre
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Velocity>(player);
  auto& stats = reg.emplace<eng::sim::Stats>(player);
  stats.health.current = 0.0f;   // dead...
  stats.hunger.current = 0.0f;   // ...of starvation...
  stats.stamina.current = 0.0f;  // ...and exhausted

  const eng::Vec2 centre{640.0f, 360.0f};
  eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // 1st call: goes Downed (no ally near)
  REQUIRE(reg.all_of<eng::sim::Downed>(player));
  eng::sim::handle_deaths(reg, centre, 6.0f);  // a fat dt expires the 5s timer -> respawn

  const eng::sim::Stats& after = reg.get<eng::sim::Stats>(player);
  REQUIRE_FALSE(reg.all_of<eng::sim::Downed>(player));          // back up...
  REQUIRE(after.health.current == Approx(after.health.max));    // ...at full health...
  REQUIRE(after.hunger.current == Approx(after.hunger.max));    // ...fed (not re-starving)...
  REQUIRE(after.stamina.current == Approx(after.stamina.max));  // ...and rested...
  REQUIRE(reg.get<eng::sim::Transform>(player).position.x == Approx(centre.x));  // ...at the centre
}

TEST_CASE("a living ally revives a downed player in place", "[sim]") {
  // A rescuer within reach hauls the player up WHERE they fell (not teleported to centre) —
  // the co-op payoff that beats waiting out the respawn timer.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Velocity>(player);
  reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // down
  // A living NPC ally standing right next to them (within the revive distance).
  const entt::entity ally = reg.create();
  reg.emplace<eng::sim::Transform>(ally, eng::Vec2{110.0f, 100.0f});
  reg.emplace<eng::sim::Npc>(ally);
  reg.emplace<eng::sim::Stats>(ally);  // full health — alive

  const eng::Vec2 centre{640.0f, 360.0f};
  eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // 1st call: goes Downed
  REQUIRE(reg.all_of<eng::sim::Downed>(player));
  eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // 2nd: ally in reach -> revived early

  REQUIRE_FALSE(reg.all_of<eng::sim::Downed>(player));  // back up...
  const eng::sim::Stats& after = reg.get<eng::sim::Stats>(player);
  REQUIRE(after.health.current == Approx(after.health.max));  // ...healed...
  REQUIRE(reg.get<eng::sim::Transform>(player).position.x ==
          Approx(100.0f));  // ...IN PLACE, not centre
  REQUIRE(reg.get<eng::sim::Transform>(player).position.y == Approx(100.0f));
}

TEST_CASE("a downed player does not eat an orb it fell on", "[sim]") {
  // Helpless means helpless: a downed player must not heal off a loot orb they happen to be
  // lying on (mirrors regenerate_vitals excluding Downed) — only a rescue or respawn revives.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // down
  reg.emplace<eng::sim::Downed>(player);
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{0.0f, 0.0f});  // right on the downed body
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::collect_pickups(reg, 1.0f / 60.0f);

  REQUIRE(reg.get<eng::sim::Stats>(player).health.current ==
          Approx(0.0f));    // no heal off the floor
  REQUIRE(reg.valid(orb));  // orb untouched
}

TEST_CASE("a downed player ignores movement input", "[sim]") {
  // The command funnel drops MovePlayer for a downed player, so pressing a direction while
  // helpless does nothing — you lie where you fell.
  eng::sim::World world;
  const entt::entity player = world.player();
  world.registry().emplace<eng::sim::Downed>(player);  // force the live player down...
  world.registry().get<eng::sim::Stats>(player).health.current = 0.0f;

  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
  world.step();

  REQUIRE(glm::length(world.registry().get<eng::sim::Velocity>(player).value) == Approx(0.0f));
}

TEST_CASE("a colonist steers toward a downed ally to rescue it", "[sim]") {
  entt::registry reg;
  const entt::entity fallen = reg.create();
  reg.emplace<eng::sim::Transform>(fallen, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Downed>(fallen);  // a player crumpled here
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(
      npc, eng::Vec2{100.0f, 0.0f});  // in rescue range, outside revive range
  reg.emplace<eng::sim::Velocity>(npc);
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc);  // a living colonist

  eng::sim::steer_npcs(reg);

  const eng::Vec2 v = reg.get<eng::sim::Velocity>(npc).value;
  REQUIRE(v.x < 0.0f);           // running LEFT toward the fallen ally at the origin...
  REQUIRE(v.y == Approx(0.0f));  // ...straight at them
}

TEST_CASE("rescuing a downed ally outranks foraging", "[sim]") {
  entt::registry reg;
  const entt::entity fallen = reg.create();
  reg.emplace<eng::sim::Transform>(fallen, eng::Vec2{-100.0f, 0.0f});  // ally to the LEFT
  reg.emplace<eng::sim::Downed>(fallen);
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(npc);
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc).hunger.current = 5.0f;  // starving...
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{100.0f, 0.0f});  // ...with food to the RIGHT
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::steer_npcs(reg);

  // Even starving, it runs to the ally (left), not the food (right) — a life beats a meal.
  REQUIRE(reg.get<eng::sim::Velocity>(npc).value.x < 0.0f);
}

TEST_CASE("a colonist runs in and revives a downed ally, beating the timer", "[sim]") {
  // End to end: an NPC sees a fallen player, closes the gap, and hauls them up IN PLACE well
  // before the ~5s timer would have respawned them at the centre.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Velocity>(player);
  reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // goes down on the first tick
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc,
                                   eng::Vec2{250.0f, 100.0f});  // 150 units off — in rescue range
  reg.emplace<eng::sim::Velocity>(npc);
  reg.emplace<eng::sim::Npc>(npc);
  reg.emplace<eng::sim::Stats>(npc);

  // Drive the systems in step() order. 120 ticks (2s) is far short of the 5s timer, so a
  // revive here is the NPC's doing, not an expiry respawn.
  const eng::Vec2 centre{640.0f, 360.0f};
  const float dt = 1.0f / 60.0f;
  for (int i = 0; i < 120; ++i) {
    eng::sim::steer_npcs(reg);
    eng::sim::integrate_motion(reg, dt);
    eng::sim::handle_deaths(reg, centre, dt);
    if (!reg.all_of<eng::sim::Downed>(player)) break;
  }

  REQUIRE_FALSE(reg.all_of<eng::sim::Downed>(player));  // revived by the colonist...
  REQUIRE(reg.get<eng::sim::Transform>(player).position.x == Approx(100.0f));  // ...IN PLACE...
  REQUIRE(reg.get<eng::sim::Transform>(player).position.y == Approx(100.0f));  // ...not at centre
}

TEST_CASE("creatures ignore a downed body and hunt whoever's still standing", "[sim]") {
  // A Downed body is inert to the fight: chase_prey drops it from the prey set, so a creature
  // re-targets the living instead of camping a corpse.
  entt::registry reg;
  const entt::entity creature = reg.create();
  reg.emplace<eng::sim::Transform>(creature, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(creature);
  reg.emplace<eng::sim::Enemy>(creature);
  // A downed body right next to it (would be the nearest prey), and a standing NPC further out.
  const entt::entity fallen = reg.create();
  reg.emplace<eng::sim::Transform>(fallen, eng::Vec2{10.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(fallen).health.current = 0.0f;
  reg.emplace<eng::sim::Downed>(fallen);
  const entt::entity standing = reg.create();
  reg.emplace<eng::sim::Transform>(standing, eng::Vec2{0.0f, 100.0f});
  reg.emplace<eng::sim::Stats>(standing);
  reg.emplace<eng::sim::Npc>(standing);

  eng::sim::chase_prey(reg);

  // It homes on the STANDING NPC (up, +y) — NOT the adjacent downed body (which would be +x).
  REQUIRE(reg.get<eng::sim::Velocity>(creature).value.y > 0.0f);
  REQUIRE(reg.get<eng::sim::Velocity>(creature).value.x == Approx(0.0f));
}

TEST_CASE("a downed body takes no hits: no free grind from creatures or motes", "[sim]") {
  // Closing the risk-free-progression exploit: neither a creature swing nor a mote may train a
  // helpless body's skills or stamp a flash on it — it is not a valid target while down.
  entt::registry reg;
  const entt::entity fallen = reg.create();
  reg.emplace<eng::sim::Transform>(fallen, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(fallen).health.current = 0.0f;
  reg.emplace<eng::sim::Skills>(fallen);
  reg.emplace<eng::sim::Attributes>(fallen).dexterity.level = 8;  // would dodge+train if attacked
  reg.emplace<eng::sim::Downed>(fallen);
  const entt::entity creature = reg.create();
  reg.emplace<eng::sim::Transform>(creature, eng::Vec2{0.0f, 0.0f});  // right on top of it
  reg.emplace<eng::sim::Enemy>(creature);
  const entt::entity mote = reg.create();
  reg.emplace<eng::sim::Transform>(mote, eng::Vec2{0.0f, 0.0f});  // also right on top of it
  reg.emplace<eng::sim::Hazard>(mote);

  std::mt19937 rng{1234};
  eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);
  eng::sim::resolve_contacts(reg);

  // Trained NOTHING (no Evasion from the creature, no Toughness from the mote) and no hit-flash.
  REQUIRE(reg.get<eng::sim::Skills>(fallen).find(eng::sim::SkillId::Evasion) == nullptr);
  REQUIRE(reg.get<eng::sim::Skills>(fallen).find(eng::sim::SkillId::Toughness) == nullptr);
  REQUIRE_FALSE(reg.all_of<eng::sim::HitFlash>(fallen));
  // ...and the mote drifts through rather than being consumed (no hit occurred).
  REQUIRE(reg.valid(mote));
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

TEST_CASE("a veteran attacker hits harder than a fresh one at equal Strength", "[sim]") {
  // The Character Level is a global "veteran" multiplier. It already scaled the earned HP
  // pool; now it scales the earned Strength delta on a swing's damage too (POWER(level - 1)),
  // so the combat XP that all activity now feeds into it finally PAYS OFF in combat. Two
  // attackers with IDENTICAL Strength differ only in character level — the veteran hits harder.
  entt::registry reg;

  // A fresh attacker (character level 1 -> POWER(0) = 1.0) and its foe, near the origin.
  const entt::entity fresh = reg.create();
  reg.emplace<eng::sim::Transform>(fresh, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(fresh).strength.level = 5;
  reg.emplace<eng::sim::Skills>(fresh);
  reg.emplace<eng::sim::CharacterLevel>(fresh);  // default level 1
  const entt::entity fresh_foe = reg.create();
  reg.emplace<eng::sim::Transform>(fresh_foe, eng::Vec2{20.0f, 0.0f});  // inside reach
  reg.emplace<eng::sim::Stats>(fresh_foe,
                               eng::sim::Vital{200.0f, 200.0f, 0.0f});  // won't die in one
  reg.emplace<eng::sim::Attributes>(fresh_foe).endurance.level = 3;     // identical VIT defence
  reg.emplace<eng::sim::Enemy>(fresh_foe);

  // A veteran attacker (character level 20), identical Strength, placed far away so its reach
  // can't touch the fresh foe — with its own identical foe. Only the character level differs.
  const entt::entity vet = reg.create();
  reg.emplace<eng::sim::Transform>(vet, eng::Vec2{1000.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(vet).strength.level = 5;
  reg.emplace<eng::sim::Skills>(vet);
  reg.emplace<eng::sim::CharacterLevel>(vet).level = 20;
  const entt::entity vet_foe = reg.create();
  reg.emplace<eng::sim::Transform>(vet_foe, eng::Vec2{1020.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(vet_foe, eng::sim::Vital{200.0f, 200.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(vet_foe).endurance.level = 3;
  reg.emplace<eng::sim::Enemy>(vet_foe);

  std::mt19937 rng{1234};  // foes DEX 1 -> never dodge; attackers LCK 1 -> never crit (no draw)
  eng::sim::perform_attack(reg, fresh, rng);
  eng::sim::perform_attack(reg, vet, rng);

  const float fresh_dmg = 200.0f - reg.get<eng::sim::Stats>(fresh_foe).health.current;
  const float vet_dmg = 200.0f - reg.get<eng::sim::Stats>(vet_foe).health.current;
  REQUIRE(fresh_dmg > 0.0f);     // the fresh attacker connected
  REQUIRE(vet_dmg > fresh_dmg);  // the veteran's earned Strength compounds into a harder hit
}

TEST_CASE("a weapon's granted Strength does not compound with character level", "[sim]") {
  // The veteran multiplier compounds only what you EARNED by grinding, never gear. Two wielders
  // of the SAME weapon at the SAME (base) trained Strength deal the same blow whatever their
  // character level — otherwise a fixed weapon bonus would silently scale with unrelated
  // progression, breaking "you are what you do". (Both have trained Strength 1, so the earned
  // delta is 0 and the ONLY Strength contribution is the +4 gear, which must stay flat.)
  entt::registry reg;

  const entt::entity fresh = reg.create();
  reg.emplace<eng::sim::Transform>(fresh, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(fresh);  // trained Strength level 1
  reg.emplace<eng::sim::Skills>(fresh);
  reg.emplace<eng::sim::CharacterLevel>(fresh);                          // level 1
  reg.emplace<eng::sim::Equipped>(fresh, eng::sim::Equipped{4, 0.25f});  // +4 STR weapon
  const entt::entity fresh_foe = reg.create();
  reg.emplace<eng::sim::Transform>(fresh_foe, eng::Vec2{20.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(fresh_foe, eng::sim::Vital{200.0f, 200.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(fresh_foe).endurance.level = 3;
  reg.emplace<eng::sim::Enemy>(fresh_foe);

  const entt::entity vet = reg.create();
  reg.emplace<eng::sim::Transform>(vet, eng::Vec2{1000.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(vet);  // identical trained Strength
  reg.emplace<eng::sim::Skills>(vet);
  reg.emplace<eng::sim::CharacterLevel>(vet).level = 20;               // seasoned...
  reg.emplace<eng::sim::Equipped>(vet, eng::sim::Equipped{4, 0.25f});  // ...same weapon
  const entt::entity vet_foe = reg.create();
  reg.emplace<eng::sim::Transform>(vet_foe, eng::Vec2{1020.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(vet_foe, eng::sim::Vital{200.0f, 200.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(vet_foe).endurance.level = 3;
  reg.emplace<eng::sim::Enemy>(vet_foe);

  std::mt19937 rng{1234};  // foes DEX 1 (never dodge); attackers LCK 1 (never crit, no draw)
  eng::sim::perform_attack(reg, fresh, rng);
  eng::sim::perform_attack(reg, vet, rng);

  const float fresh_dmg = 200.0f - reg.get<eng::sim::Stats>(fresh_foe).health.current;
  const float vet_dmg = 200.0f - reg.get<eng::sim::Stats>(vet_foe).health.current;
  REQUIRE(fresh_dmg > 0.0f);              // both connected
  REQUIRE(vet_dmg == Approx(fresh_dmg));  // gear STR is flat: the veteran gains nothing from it
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

TEST_CASE("a strike trains Strength fully and Dexterity a little (skill contributions)", "[sim]") {
  // P2 main+contributions: Striking's main is Strength (full share) with Dexterity as a
  // contributor (a quarter). So a pure striker slowly picks up a little footwork.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});             // inside reach
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{1.0e6f, 1.0e6f, 0.0f});  // never dies
  reg.emplace<eng::sim::Attributes>(foe);  // DEX 1 -> never dodges, so every strike lands
  reg.emplace<eng::sim::Enemy>(foe);

  std::mt19937 rng{1234};
  for (int i = 0; i < 8; ++i) eng::sim::perform_attack(reg, atk, rng);  // 10 XP each

  const eng::sim::Attributes& a = reg.get<eng::sim::Attributes>(atk);
  REQUIRE(a.strength.xp.to_double() == Approx(80.0));   // main: the full 8 * 10...
  REQUIRE(a.dexterity.xp.to_double() == Approx(20.0));  // contributor: a quarter of it
}

TEST_CASE("a main-only skill feeds only its own attribute", "[sim]") {
  // Guards the routing table: a skill with no contributors (Scavenging -> Luck) must leak
  // XP to NOTHING else — a mis-mapped row would show up as a stray attribute gaining XP.
  entt::registry reg;
  const entt::entity p = reg.create();
  reg.emplace<eng::sim::Transform>(p, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(p);
  reg.emplace<eng::sim::Stats>(p);
  reg.emplace<eng::sim::Skills>(p);
  reg.emplace<eng::sim::Attributes>(p);
  const entt::entity orb = reg.create();
  reg.emplace<eng::sim::Transform>(orb, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Pickup>(orb);

  eng::sim::collect_pickups(reg, 1.0f / 60.0f);

  const eng::sim::Attributes& a = reg.get<eng::sim::Attributes>(p);
  REQUIRE(a.luck.xp.to_double() > 0.0);    // Scavenging fed its main, Luck...
  REQUIRE(a.strength.xp == eng::Fixed{});  // ...and leaked to nothing else
  REQUIRE(a.dexterity.xp == eng::Fixed{});
  REQUIRE(a.endurance.xp == eng::Fixed{});
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

TEST_CASE("a landed blow stamps a hit-flash that then decays away", "[sim]") {
  // Presentation juice: any blow leaves a brief HitFlash so the renderer can blink
  // the struck dot white. It's stamped at the damage site and decayed by dt.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Stats>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::Attributes>(player);  // DEX 1 -> never dodges, so the blow lands
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe);

  REQUIRE_FALSE(reg.all_of<eng::sim::HitFlash>(player));  // no blow yet, no flash

  std::mt19937 rng{1234};
  eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);

  const eng::sim::HitFlash* flash = reg.try_get<eng::sim::HitFlash>(player);
  REQUIRE(flash != nullptr);                                        // the blow lit it up
  REQUIRE(flash->remaining == Approx(eng::sim::kHitFlashSeconds));  // a fresh, full flash

  // Age it past its lifetime (0.15s ~ 9 ticks); after ~10 ticks it's removed entirely.
  for (int i = 0; i < 10; ++i) eng::sim::decay_flashes(reg, 1.0f / 60.0f);
  REQUIRE_FALSE(reg.all_of<eng::sim::HitFlash>(player));  // faded and gone
}

TEST_CASE("wounded_brightness dims a dot toward the floor as health falls", "[sim]") {
  // The steady twin of the hit-flash: a pure function the renderer uses to DIM a wounded
  // dot. Full health draws at full brightness; a dying dot fades toward (never past) the floor.
  REQUIRE(eng::sim::wounded_brightness(100.0f, 100.0f) == Approx(1.0f));  // full = unchanged
  REQUIRE(eng::sim::wounded_brightness(0.0f, 100.0f) ==
          Approx(eng::sim::kWoundedFloor));  // dead = floor
  const float half = eng::sim::wounded_brightness(50.0f, 100.0f);
  REQUIRE(half > eng::sim::kWoundedFloor);  // half health sits strictly...
  REQUIRE(half < 1.0f);                     // ...between floor and full (monotonic)
  REQUIRE(eng::sim::wounded_brightness(150.0f, 100.0f) ==
          Approx(1.0f));  // overshoot clamps, never brighter
  REQUIRE(eng::sim::wounded_brightness(50.0f, 0.0f) ==
          Approx(1.0f));  // no bar -> full, no divide-by-zero
}

TEST_CASE("personality_tint warms the brave and cools the coward, neutral untinted", "[sim]") {
  // Pure presentation helper: a colour multiplier so bravery reads on screen. Neutral is the
  // identity, the extremes shift red-vs-blue in mirror image, and green is never touched (so a
  // tinted NPC stays green-dominant, never enemy-red or player-blue).
  const eng::Vec3 neutral = eng::sim::personality_tint(0);
  REQUIRE(neutral.r == 1.0f);  // bravery 0 -> {1,1,1} EXACTLY: an IEEE identity, so a neutral or
  REQUIRE(neutral.g == 1.0f);  // no-Personality dot renders bit-identical to before (not just ~1)
  REQUIRE(neutral.b == 1.0f);

  const eng::Vec3 brave = eng::sim::personality_tint(100);
  REQUIRE(brave.r > 1.0f);           // warm: red up...
  REQUIRE(brave.b < 1.0f);           // ...blue down...
  REQUIRE(brave.g == Approx(1.0f));  // ...green untouched (stays green-dominant)

  const eng::Vec3 coward = eng::sim::personality_tint(-100);
  REQUIRE(coward.r < 1.0f);  // cool: the mirror — red down...
  REQUIRE(coward.b > 1.0f);  // ...blue up...
  REQUIRE(coward.g == Approx(1.0f));

  // Symmetric about neutral: the brave's red shift equals the coward's blue shift, and vice-versa.
  REQUIRE(brave.r == Approx(coward.b));
  REQUIRE(brave.b == Approx(coward.r));
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

  eng::sim::handle_deaths(reg, eng::Vec2{0.0f, 0.0f}, 1.0f / 60.0f);

  REQUIRE(!reg.valid(foe));                              // reaped for good (permadeath)
  REQUIRE(reg.storage<eng::sim::Pickup>().size() == 1);  // ...and it left loot
}

TEST_CASE("a weapon is no pure-upside: it carries a bane", "[sim]") {
  // The design's non-negotiable: every item has a positive AND a negative trait, nothing
  // rolls pure-upside. The one hardcoded weapon def must honour it. (The CI-lint in miniature.)
  const eng::sim::Weapon w{};
  REQUIRE(w.strength_bonus > 0);   // the upside — more Strength...
  REQUIRE(w.move_penalty > 0.0f);  // ...paid for with a downside — it slows you
}

namespace {
// Deal one strike from a fresh attacker (optionally wielding a weapon) at a foe `dist` away;
// return the damage dealt (0 if out of reach). Shared by the wield-combat tests below.
float weapon_strike(bool armed, float dist) {
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  if (armed) reg.emplace<eng::sim::Equipped>(atk, eng::sim::Equipped{4, 0.25f});
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{dist, 0.0f});
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{1.0e6f, 1.0e6f, 0.0f});  // never dies
  reg.emplace<eng::sim::Attributes>(foe);  // DEX 1 -> never dodges, so every in-reach strike lands
  reg.emplace<eng::sim::Enemy>(foe);
  std::mt19937 rng{1234};
  const float before = reg.get<eng::sim::Stats>(foe).health.current;
  eng::sim::perform_attack(reg, atk, rng);
  return before - reg.get<eng::sim::Stats>(foe).health.current;
}
}  // namespace

TEST_CASE("wielding a weapon hits harder", "[sim]") {
  // At a distance both can reach (20), the +Strength weapon deals strictly more damage.
  REQUIRE(weapon_strike(true, 20.0f) > weapon_strike(false, 20.0f));
}

TEST_CASE("a wielded weapon extends attack reach", "[sim]") {
  // Bare reach is ~45 (STR 1); +4 STR from the weapon stretches it to ~69. A foe 55 units
  // out is beyond a bare swing but within an armed one.
  REQUIRE(weapon_strike(false, 55.0f) == Approx(0.0f));  // bare: whiffs (out of reach)...
  REQUIRE(weapon_strike(true, 55.0f) > 0.0f);            // armed: connects
}

TEST_CASE("a wielded weapon slows the player (the heft bane)", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();

  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
  world.step();
  const float bare = glm::length(world.registry().get<eng::sim::Velocity>(player).value);

  world.registry().emplace<eng::sim::Equipped>(player, eng::sim::Equipped{4, 0.25f});
  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
  world.step();
  const float armed = glm::length(world.registry().get<eng::sim::Velocity>(player).value);

  REQUIRE(armed < bare);  // heft: you trade speed for power
}

TEST_CASE("each creature archetype drops its own loot on death", "[sim]") {
  // The symmetric loot economy: a brute yields OFFENCE (a weapon), a swarmer SUSTAIN (a health
  // orb), a sentinel DEFENCE (armour) — each keyed on DropKind, resolving independently.
  entt::registry reg;
  const entt::entity brute = reg.create();
  reg.emplace<eng::sim::Transform>(brute, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::Stats>(brute, eng::sim::Vital{0.0f, 40.0f, 0.0f});  // dead
  reg.emplace<eng::sim::Enemy>(brute).drop = eng::sim::DropKind::Weapon;
  const entt::entity swarmer = reg.create();
  reg.emplace<eng::sim::Transform>(swarmer, eng::Vec2{200.0f, 200.0f});
  reg.emplace<eng::sim::Stats>(swarmer, eng::sim::Vital{0.0f, 15.0f, 0.0f});  // dead
  reg.emplace<eng::sim::Enemy>(swarmer);  // drop defaults to HealthOrb
  const eng::Vec2 sentinel_pos{300.0f, 300.0f};
  const entt::entity sentinel = reg.create();
  reg.emplace<eng::sim::Transform>(sentinel, sentinel_pos);
  reg.emplace<eng::sim::Stats>(sentinel, eng::sim::Vital{0.0f, 60.0f, 0.0f});  // dead
  reg.emplace<eng::sim::Enemy>(sentinel).drop = eng::sim::DropKind::Armour;

  eng::sim::handle_deaths(reg, eng::Vec2{0.0f, 0.0f}, 1.0f / 60.0f);

  REQUIRE(reg.storage<eng::sim::Weapon>().size() == 1);  // the brute yields gear...
  REQUIRE(reg.storage<eng::sim::Pickup>().size() == 1);  // ...the swarmer, sustain...
  REQUIRE(reg.storage<eng::sim::Armour>().size() == 1);  // ...the sentinel, armour
  // ...and the armour lies where the sentinel fell.
  const entt::entity dropped = *reg.view<eng::sim::Armour>().begin();
  REQUIRE(reg.get<eng::sim::Transform>(dropped).position.x == Approx(sentinel_pos.x));
  REQUIRE(reg.get<eng::sim::Transform>(dropped).position.y == Approx(sentinel_pos.y));
}

TEST_CASE("a sentinel's dropped armour is a real acquisition path: pick it up and wear it",
          "[sim]") {
  // Prove the loot seam is genuinely wearable, not just a spawn: a bare wearer standing on the
  // dropped armour dons it (equip_nearest_gear folds its defence into the Equipped cache).
  entt::registry reg;
  const eng::Vec2 pos{50.0f, 50.0f};
  const entt::entity sentinel = reg.create();
  reg.emplace<eng::sim::Transform>(sentinel, pos);
  reg.emplace<eng::sim::Stats>(sentinel, eng::sim::Vital{0.0f, 60.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(sentinel).drop = eng::sim::DropKind::Armour;
  eng::sim::handle_deaths(reg, eng::Vec2{0.0f, 0.0f}, 1.0f / 60.0f);

  const entt::entity wearer = reg.create();
  reg.emplace<eng::sim::Transform>(wearer, pos);  // standing on the dropped armour
  const entt::entity grabbed = eng::sim::equip_nearest_gear(reg, wearer);

  REQUIRE(reg.valid(grabbed));  // grabbed the grounded armour (entt::null would be invalid)...
  REQUIRE(reg.get<eng::sim::Equipped>(wearer).defence_bonus ==
          Approx(6.0f));  // ...and wears its defence
}

TEST_CASE("the Equip command wields the nearest weapon in reach", "[sim]") {
  eng::sim::World world;
  const entt::entity player = world.player();
  const eng::Vec2 ppos = world.registry().get<eng::sim::Transform>(player).position;
  const entt::entity w = world.registry().create();
  world.registry().emplace<eng::sim::Transform>(w, ppos);  // a weapon right on the player
  world.registry().emplace<eng::sim::Weapon>(w);           // default {+4 STR, -25% speed}

  world.submit(eng::sim::equip(eng::sim::kLocalPlayer));
  world.step();

  REQUIRE(world.registry().all_of<eng::sim::Equipped>(player));                   // now wielding...
  REQUIRE(world.registry().get<eng::sim::Equipped>(player).strength_bonus == 4);  // ...its mods...
  REQUIRE_FALSE(world.registry().valid(w));                                       // ...and consumed
}

TEST_CASE("the Drop command sheds your wielded weapon at your feet", "[sim]") {
  // The inverse of Equip: ditch the blade to reclaim your full move speed. Wield first, then
  // drop — the Equipped bane is gone, a weapon lies where you stood, and you move free again.
  eng::sim::World world;
  const entt::entity player = world.player();
  const eng::Vec2 ppos = world.registry().get<eng::sim::Transform>(player).position;
  const entt::entity w = world.registry().create();
  world.registry().emplace<eng::sim::Transform>(w, ppos);  // a weapon right on the player
  world.registry().emplace<eng::sim::Weapon>(w);           // default {+4 STR, -25% speed}
  world.submit(eng::sim::equip(eng::sim::kLocalPlayer));
  world.step();
  REQUIRE(world.registry().all_of<eng::sim::Equipped>(player));  // armed and slowed

  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();

  REQUIRE_FALSE(world.registry().all_of<eng::sim::Equipped>(player));  // heft shed
  // Exactly one Weapon now lies on the ground, where the player stood, with its mods intact.
  int weapons = 0;
  entt::entity dropped = entt::null;
  for (const entt::entity e : world.registry().view<eng::sim::Weapon>()) {
    ++weapons;
    dropped = e;
  }
  REQUIRE(weapons == 1);
  REQUIRE(world.registry().get<eng::sim::Weapon>(dropped).strength_bonus == 4);  // faithful mods
  REQUIRE(world.registry().get<eng::sim::Transform>(dropped).position.x ==
          Approx(ppos.x));  // at feet
  REQUIRE(world.registry().get<eng::sim::Transform>(dropped).position.y == Approx(ppos.y));

  // Unburdened, a move now reaches the full base speed (320), not the wielded 240 (320 x 0.75).
  world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, {1.0f, 0.0f}));
  world.step();
  REQUIRE(world.registry().get<eng::sim::Velocity>(player).value.x == Approx(320.0f));

  // ...and the dropped blade is re-wieldable — put it under the player and Equip returns the heft.
  world.registry().get<eng::sim::Transform>(dropped).position =
      world.registry().get<eng::sim::Transform>(player).position;
  world.submit(eng::sim::equip(eng::sim::kLocalPlayer));
  world.step();
  REQUIRE(world.registry().all_of<eng::sim::Equipped>(player));  // wielded again — a closed loop
}

TEST_CASE("a bare-handed Drop does nothing", "[sim]") {
  // Drop with no weapon wielded is a harmless no-op — no phantom weapon appears.
  eng::sim::World world;
  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() == 0);  // nothing spawned
  REQUIRE_FALSE(world.registry().all_of<eng::sim::Equipped>(world.player()));
}

TEST_CASE("a downed player cannot drop its weapon", "[sim]") {
  // A helpless (Downed) player can't act — Drop is skipped, exactly like Equip and MovePlayer.
  eng::sim::World world;
  const entt::entity player = world.player();
  world.registry().emplace<eng::sim::Equipped>(player, eng::sim::Equipped{4, 0.25f});
  world.registry().emplace<eng::sim::Downed>(player);

  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();

  REQUIRE(world.registry().all_of<eng::sim::Equipped>(player));  // still wielding — couldn't drop
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() == 0);  // nothing hit the ground
}

TEST_CASE("equipping a weapon then armour fills two independent slots", "[sim]") {
  // The two-slot invariant: grabbing armour must NOT clobber a wielded weapon (or vice-versa).
  // Both items sit on the player; the first Equip grabs the weapon (ties break to weapon), the
  // second grabs the armour — and the cache ends up carrying BOTH.
  eng::sim::World world;
  const entt::entity player = world.player();
  const eng::Vec2 ppos = world.registry().get<eng::sim::Transform>(player).position;
  const entt::entity w = world.registry().create();
  world.registry().emplace<eng::sim::Transform>(w, ppos);
  world.registry().emplace<eng::sim::Weapon>(w);  // default {+4 STR, -25% speed}
  const entt::entity a = world.registry().create();
  world.registry().emplace<eng::sim::Transform>(a, ppos);
  world.registry().emplace<eng::sim::Armour>(a);  // default {+6 DEF, -30% stamina regen}

  world.submit(eng::sim::equip(eng::sim::kLocalPlayer));  // grabs the weapon
  world.step();
  world.submit(eng::sim::equip(eng::sim::kLocalPlayer));  // grabs the armour
  world.step();

  const eng::sim::Equipped& eq = world.registry().get<eng::sim::Equipped>(player);
  REQUIRE(eq.strength_bonus == 4);            // weapon slot filled...
  REQUIRE(eq.defence_bonus == Approx(6.0f));  // ...AND armour slot filled — neither clobbered
}

TEST_CASE("worn armour softens a creature's blow", "[sim]") {
  // Armour adds flat defence through defence_of, so it soaks part of a creature's hit — while
  // an unarmoured, VIT-1 target takes the raw blow (the determinism baseline: no gear, no change).
  entt::registry reg;
  const entt::entity bare = reg.create();
  reg.emplace<eng::sim::Transform>(bare, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(bare);
  reg.emplace<eng::sim::Attributes>(bare);  // DEX 1 (never dodges), END 1 (no VIT defence)
  const entt::entity foe1 = reg.create();
  reg.emplace<eng::sim::Transform>(foe1, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe1);  // brute: attack_damage 15, ready to swing

  const entt::entity armoured = reg.create();  // its own foe, far away so they don't cross
  reg.emplace<eng::sim::Transform>(armoured, eng::Vec2{1000.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(armoured);
  reg.emplace<eng::sim::Attributes>(armoured);
  reg.emplace<eng::sim::Equipped>(armoured,
                                  eng::sim::Equipped{0, 0.0f, 6.0f, 0.30f});  // armour only
  const entt::entity foe2 = reg.create();
  reg.emplace<eng::sim::Transform>(foe2, eng::Vec2{1000.0f, 0.0f});
  reg.emplace<eng::sim::Enemy>(foe2);

  const float bare_before = reg.get<eng::sim::Stats>(bare).health.current;
  const float arm_before = reg.get<eng::sim::Stats>(armoured).health.current;
  std::mt19937 rng{1234};
  eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);
  const float bare_dmg = bare_before - reg.get<eng::sim::Stats>(bare).health.current;
  const float arm_dmg = arm_before - reg.get<eng::sim::Stats>(armoured).health.current;

  REQUIRE(bare_dmg == Approx(15.0f));  // no gear, no VIT -> the full raw blow (unchanged baseline)
  REQUIRE(arm_dmg < bare_dmg);         // armour soaked part of it
  REQUIRE(arm_dmg > 0.0f);             // ...but never negates (mitigate's floor still lands a hit)
}

TEST_CASE("worn armour slows stamina recovery (its bane)", "[sim]") {
  // The armour bane: plate gives a weaker second wind. Two idle (resting) entities differing
  // only in armour; the armoured one recovers strictly less stamina over the same rest.
  entt::registry reg;
  const entt::entity bare = reg.create();
  reg.emplace<eng::sim::Stats>(bare).stamina =
      eng::sim::Vital{50.0f, 100.0f, 20.0f};  // recovers 20/s
  reg.emplace<eng::sim::Velocity>(bare);      // zero velocity -> resting
  const entt::entity armoured = reg.create();
  reg.emplace<eng::sim::Stats>(armoured).stamina = eng::sim::Vital{50.0f, 100.0f, 20.0f};
  reg.emplace<eng::sim::Velocity>(armoured);
  reg.emplace<eng::sim::Equipped>(armoured,
                                  eng::sim::Equipped{0, 0.0f, 6.0f, 0.30f});  // -30% regen

  const float dt = 1.0f / 60.0f;
  for (int i = 0; i < 30; ++i) eng::sim::update_stamina(reg, dt);  // half a second resting

  const float bare_sta = reg.get<eng::sim::Stats>(bare).stamina.current;
  const float arm_sta = reg.get<eng::sim::Stats>(armoured).stamina.current;
  REQUIRE(bare_sta > 50.0f);  // both recovered from resting...
  REQUIRE(arm_sta > 50.0f);
  REQUIRE(arm_sta < bare_sta);  // ...but the armour's bane held the armoured one back
}

TEST_CASE("dropping a weapon keeps your armour (slot-aware Drop)", "[sim]") {
  // Drop is slot-aware: it sheds only the weapon, leaving worn armour on — a blanket
  // remove<Equipped> would silently strip the armour.
  eng::sim::World world;
  const entt::entity player = world.player();
  world.registry().emplace<eng::sim::Equipped>(player,
                                               eng::sim::Equipped{4, 0.25f, 6.0f, 0.30f});  // both

  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();

  const eng::sim::Equipped* eq = world.registry().try_get<eng::sim::Equipped>(player);
  REQUIRE(eq != nullptr);            // still wearing something (the armour)...
  REQUIRE(eq->strength_bonus == 0);  // ...the weapon slot was shed...
  REQUIRE(eq->move_penalty == Approx(0.0f));
  REQUIRE(eq->defence_bonus == Approx(6.0f));  // ...but the armour slot survived intact
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() == 1);  // and a weapon hit the ground
}

TEST_CASE("dropping with only armour worn is a no-op", "[sim]") {
  // No weapon to shed -> Drop does nothing (no phantom weapon, armour untouched).
  eng::sim::World world;
  const entt::entity player = world.player();
  world.registry().emplace<eng::sim::Equipped>(
      player, eng::sim::Equipped{0, 0.0f, 6.0f, 0.30f});  // armour only

  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();

  const eng::sim::Equipped* eq = world.registry().try_get<eng::sim::Equipped>(player);
  REQUIRE(eq != nullptr);  // armour untouched...
  REQUIRE(eq->defence_bonus == Approx(6.0f));
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() == 0);  // ...and no phantom weapon
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
