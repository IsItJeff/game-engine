#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <random>
#include <string>

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

TEST_CASE("a hearth speeds nearby health regen: mend by the fire", "[sim]") {
  // The base-building recovery seed: a wounded character resting within a hearth's radius mends
  // faster than one out in the cold, but is rooted to the spot (a positioning trade). Same wound
  // and regen rate; only the distance to the hearth differs.
  const auto healed_in_1s = [](float dist_from_hearth) {
    entt::registry reg;
    const entt::entity hearth = reg.create();
    reg.emplace<eng::sim::Transform>(hearth, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Hearth>(hearth, eng::sim::Hearth{100.0f});  // 100u of warmth
    const entt::entity e = reg.create();
    reg.emplace<eng::sim::Transform>(e, eng::Vec2{dist_from_hearth, 0.0f});
    reg.emplace<eng::sim::Stats>(e, eng::sim::Vital{50.0f, 100.0f, 8.0f});  // wounded; heals 8/sec

    const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
    for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::regenerate_vitals(reg, dt);  // 1s
    return reg.get<eng::sim::Stats>(e).health.current - 50.0f;
  };
  REQUIRE(healed_in_1s(500.0f) > 0.0f);  // out in the cold -> base regen still heals...
  REQUIRE(healed_in_1s(50.0f) > healed_in_1s(500.0f));  // ...but within its warmth, mends faster
}

TEST_CASE("a hearth speeds stamina recovery too: catch your breath by the fire", "[sim]") {
  // The stamina twin of the fireside health boost — a RESTING character inside a hearth's radius
  // recovers stamina faster than one resting in the open, so the fire is a FULL recovery spot. Same
  // spent stamina and rest; only the presence of a nearby hearth differs (away = base baseline).
  const auto recovered_in_1s = [](bool by_hearth) {
    entt::registry reg;
    const entt::entity e = reg.create();
    reg.emplace<eng::sim::Transform>(e, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(e);  // zero velocity -> resting (recovers, not drains)
    reg.emplace<eng::sim::Stats>(e).stamina.current = 10.0f;  // spent, with room to recover
    if (by_hearth) {
      const entt::entity hearth = reg.create();
      reg.emplace<eng::sim::Transform>(hearth, eng::Vec2{0.0f, 0.0f});  // on top of the rester
      reg.emplace<eng::sim::Hearth>(hearth, eng::sim::Hearth{50.0f});   // within its warmth
    }
    const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
    for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::update_stamina(reg, dt);  // 1s
    return reg.get<eng::sim::Stats>(e).stamina.current - 10.0f;
  };
  REQUIRE(recovered_in_1s(false) > 0.0f);                   // resting in the open recovers some...
  REQUIRE(recovered_in_1s(true) > recovered_in_1s(false));  // ...but faster by the fire
}

TEST_CASE("a wounded colonist retreats to a hearth and holds in its warmth", "[sim]") {
  // The retreat rung makes the hearth a USED landmark: a safe but wounded colonist falls back to
  // the nearest fire to mend, then holds inside its radius; a healthy one ignores it. Health max
  // defaults to 100, so kRetreatFraction 0.5 -> wounded below 50.
  const auto steer = [](float health, float npc_x) {
    entt::registry reg;
    const entt::entity fire = reg.create();
    reg.emplace<eng::sim::Transform>(fire, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Hearth>(fire, eng::sim::Hearth{60.0f});  // healing radius 60
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{npc_x, 0.0f});
    reg.emplace<eng::sim::Velocity>(npc, eng::Vec2{7.0f, 0.0f});  // a drift, so a HOLD must zero it
    reg.emplace<eng::sim::Npc>(npc);
    reg.emplace<eng::sim::Stats>(npc).health.current = health;
    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(npc).value;
  };
  REQUIRE(steer(40.0f, 200.0f).x < 0.0f);  // wounded + out in the cold (200 > 60) -> heads in (-x)
  REQUIRE(steer(80.0f, 200.0f).x > 0.0f);  // healthy -> ignores the fire, keeps its drift (+x)
  const eng::Vec2 held = steer(40.0f, 30.0f);  // wounded + already in the warmth (30 < 60)...
  REQUIRE(held.x == 0.0f);                     // ...holds: velocity zeroed, sits and mends...
  REQUIRE(held.y == 0.0f);
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

TEST_CASE("an exhausted colonist crawls too: NPC steer speed drops at 0 stamina", "[sim]") {
  // Parity with the player's exhaustion crawl: a colonist that has spent its stamina to 0 steers
  // slower (kExhaustedMoveScale), so a tireless colony is no more. Same flee setup (a hazard
  // nearby), differing only in stamina; the fled velocity is slower when spent, but never zero (it
  // can limp).
  const auto flee_speed = [](float stamina) {
    entt::registry reg;
    const entt::entity hazard = reg.create();
    reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Hazard>(hazard);
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{50.0f, 0.0f});  // inside sense range -> flees
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);
    reg.emplace<eng::sim::Stats>(npc).stamina.current = stamina;
    eng::sim::steer_npcs(reg);
    return glm::length(reg.get<eng::sim::Velocity>(npc).value);
  };
  const float rested = flee_speed(100.0f);  // full stamina -> full flee speed...
  const float spent = flee_speed(0.0f);     // ...spent -> a crawl
  REQUIRE(rested > 0.0f);                   // it fled...
  REQUIRE(spent > 0.0f);    // ...even spent it still limps away (never fully stopped)...
  REQUIRE(spent < rested);  // ...but slower when exhausted
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

TEST_CASE("water drains over time, faster while moving than at rest", "[sim]") {
  // The SECOND Need, the twin of hunger: it only falls (no self-recovery), and exertion drains it
  // faster (the design's "exertion drains needs"). Applies to any person (bare Stats) — parity.
  entt::registry reg;
  const entt::entity rester = reg.create();
  reg.emplace<eng::sim::Stats>(rester);     // water starts full (100)
  reg.emplace<eng::sim::Velocity>(rester);  // zero velocity -> at rest
  const entt::entity mover = reg.create();
  reg.emplace<eng::sim::Stats>(mover);
  reg.emplace<eng::sim::Velocity>(mover, eng::Vec2{50.0f, 0.0f});  // moving = exerting

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < 10 * eng::sim::kTicksPerSecond; ++i) eng::sim::drain_water(reg, dt);

  REQUIRE(reg.get<eng::sim::Stats>(rester).water.current < 100.0f);  // it fell...
  REQUIRE(reg.get<eng::sim::Stats>(rester).water.current > 0.0f);    // ...gently (not empty in 10s)
  REQUIRE(reg.get<eng::sim::Stats>(mover).water.current <
          reg.get<eng::sim::Stats>(rester).water.current);  // exertion costs extra water
}

TEST_CASE("an empty canteen dehydrates health and blocks healing", "[sim]") {
  // Dehydration is starvation's twin: at 0 water it chips health through the same death path, and
  // regenerate_vitals gates healing off (the `|| water<=0` clause), so a parched character nets
  // health strictly DOWN even with a strong heal — the same guarantee starvation has. Hunger stays
  // full, isolating the WATER gate.
  entt::registry reg;
  const entt::entity e = reg.create();
  auto& s = reg.emplace<eng::sim::Stats>(e);
  s.health = eng::sim::Vital{50.0f, 100.0f, 8.0f};  // wounded, heals 8/s
  s.water.current = 0.0f;                           // parched
  reg.emplace<eng::sim::Attributes>(e).endurance.level =
      8;  // boosted regen (13.6/s) would beat 12/s

  const float before = s.health.current;
  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) {  // one second, the real tick order
    eng::sim::drain_water(reg, dt);                      // chips health at 0 water...
    eng::sim::regenerate_vitals(reg, dt);  // ...and the water gate blocks the clawback
  }
  REQUIRE(reg.get<eng::sim::Stats>(e).health.current < before);
}

TEST_CASE("drinking at a water source refills water without consuming the source", "[sim]") {
  // The refill loop: standing in a source tops your water up, and the source PERSISTS (unlike a
  // one-shot food orb) — so many can drink and you return to it. Standing outside its radius
  // refills nothing.
  entt::registry reg;
  const entt::entity well = reg.create();
  reg.emplace<eng::sim::Transform>(well, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::WaterSource>(well, eng::sim::WaterSource{60.0f});

  const entt::entity in_pool = reg.create();
  reg.emplace<eng::sim::Transform>(in_pool, eng::Vec2{20.0f, 0.0f});  // within the 60 radius
  reg.emplace<eng::sim::Stats>(in_pool).water.current = 10.0f;        // parched
  const entt::entity dry = reg.create();
  reg.emplace<eng::sim::Transform>(dry, eng::Vec2{200.0f, 0.0f});  // well outside
  reg.emplace<eng::sim::Stats>(dry).water.current = 10.0f;

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::drink(reg, dt);  // 1 second

  REQUIRE(reg.get<eng::sim::Stats>(in_pool).water.current > 10.0f);  // the one in the pool drank...
  REQUIRE(reg.get<eng::sim::Stats>(dry).water.current == Approx(10.0f));  // ...the far one didn't
  REQUIRE(reg.valid(well));  // the source is NOT consumed
}

TEST_CASE("a thirsty NPC steers toward the nearest water source", "[sim]") {
  // The thirst rung: a safe, low-water NPC heads for the nearest WaterSource; a well-watered one
  // ignores it (falls through to arm-up/drift). Ranks below hunger, above arm-up.
  entt::registry reg;
  const entt::entity well = reg.create();
  reg.emplace<eng::sim::Transform>(well, eng::Vec2{300.0f, 0.0f});  // off to the +x
  reg.emplace<eng::sim::WaterSource>(well, eng::sim::WaterSource{60.0f});

  const entt::entity thirsty = reg.create();
  reg.emplace<eng::sim::Transform>(thirsty, eng::Vec2{100.0f, 0.0f});  // 200 away, inside seek 260
  reg.emplace<eng::sim::Velocity>(thirsty);
  reg.emplace<eng::sim::Npc>(thirsty);
  reg.emplace<eng::sim::Stats>(thirsty).water.current =
      10.0f;  // parched -> below the 0.6 threshold
  const entt::entity sated = reg.create();
  reg.emplace<eng::sim::Transform>(sated, eng::Vec2{100.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(sated);
  reg.emplace<eng::sim::Npc>(sated);
  reg.emplace<eng::sim::Stats>(sated);  // full water -> not thirsty

  eng::sim::steer_npcs(reg);

  REQUIRE(reg.get<eng::sim::Velocity>(thirsty).value.x > 0.0f);  // steering toward the well (+x)...
  REQUIRE(reg.get<eng::sim::Velocity>(sated).value.x == 0.0f);   // ...the sated one ignores it
}

TEST_CASE("a colonist seeks its more depleted need before the less urgent one", "[sim]") {
  // Hunger is checked before thirst in the steer ladder, but a colonist dying of thirst shouldn't
  // forage first just because of that order: it seeks whichever need is the more depleted (lower
  // current/max). A food orb sits to the -x, a water source to the +x, both in reach; the
  // colonist's steer direction says which need it chose.
  const auto steer_x = [](float hunger, float water, bool has_well) {
    entt::registry reg;
    const entt::entity orb = reg.create();
    reg.emplace<eng::sim::Transform>(orb, eng::Vec2{-100.0f, 0.0f});  // food to the -x
    reg.emplace<eng::sim::Pickup>(orb);
    if (has_well) {
      const entt::entity well = reg.create();
      reg.emplace<eng::sim::Transform>(well, eng::Vec2{100.0f, 0.0f});  // water to the +x
      reg.emplace<eng::sim::WaterSource>(well, eng::sim::WaterSource{60.0f});
    }
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);
    auto& st = reg.emplace<eng::sim::Stats>(npc);
    st.hunger.current = hunger;  // both below their 0.6 seek thresholds (max 100)...
    st.water.current = water;    // ...so both needs bite; urgency decides which is sought
    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(npc).value.x;
  };
  REQUIRE(steer_x(50.0f, 20.0f, true) >
          0.0f);  // thirst more urgent (0.2 < 0.5) -> toward WATER (+x)
  REQUIRE(steer_x(20.0f, 50.0f, true) <
          0.0f);  // hunger more urgent (0.2 < 0.5) -> toward FOOD (-x)
  // ...but an unreachable thirst must not block a reachable meal: thirstier, yet with NO well in
  // range, the colonist forages the food it CAN reach rather than stalling on both needs.
  REQUIRE(steer_x(50.0f, 40.0f, false) <
          0.0f);  // water more depleted but no well -> toward FOOD (-x)
}

TEST_CASE("grazing a food plot refills hunger and depletes its stock", "[sim]") {
  // A FoodSource is the food twin of the pond, but FINITE: a grazer within reach eats (hunger up)
  // and the plot's stock falls. Someone outside the radius eats nothing.
  entt::registry reg;
  const entt::entity plot = reg.create();
  reg.emplace<eng::sim::Transform>(plot, eng::Vec2{0.0f, 0.0f});
  eng::sim::FoodSource fs{};
  fs.regrow_per_second = 0.0f;  // freeze regrowth so we measure pure depletion
  reg.emplace<eng::sim::FoodSource>(plot, fs);

  const entt::entity grazer = reg.create();
  reg.emplace<eng::sim::Transform>(grazer, eng::Vec2{20.0f, 0.0f});  // within the plot radius (60)
  reg.emplace<eng::sim::Stats>(grazer).hunger.current = 10.0f;       // hungry
  const entt::entity away = reg.create();
  reg.emplace<eng::sim::Transform>(away, eng::Vec2{300.0f, 0.0f});  // well outside
  reg.emplace<eng::sim::Stats>(away).hunger.current = 10.0f;

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::graze(reg, dt);  // 1 second

  REQUIRE(reg.get<eng::sim::Stats>(grazer).hunger.current > 10.0f);  // the grazer ate...
  REQUIRE(reg.get<eng::sim::FoodSource>(plot).stock < 100.0f);       // ...drawing the plot down...
  REQUIRE(reg.get<eng::sim::Stats>(away).hunger.current == Approx(10.0f));  // ...the far one didn't
}

TEST_CASE("a food plot regrows its stock over time, capped at its yield", "[sim]") {
  // Renewable but finite: a picked-bare plot recovers over time, and never past its max_stock.
  entt::registry reg;
  const entt::entity plot = reg.create();
  reg.emplace<eng::sim::Transform>(plot, eng::Vec2{0.0f, 0.0f});
  eng::sim::FoodSource fs{};
  fs.stock = 0.0f;  // picked bare, no grazers present
  reg.emplace<eng::sim::FoodSource>(plot, fs);

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < 3 * eng::sim::kTicksPerSecond; ++i) eng::sim::graze(reg, dt);  // 3s
  REQUIRE(reg.get<eng::sim::FoodSource>(plot).stock > 0.0f);  // it recovered some...

  // Run long past full: it holds AT max, never overgrows.
  for (int i = 0; i < 90 * eng::sim::kTicksPerSecond; ++i) eng::sim::graze(reg, dt);
  const eng::sim::FoodSource& grown = reg.get<eng::sim::FoodSource>(plot);
  REQUIRE(grown.stock == Approx(grown.max_stock));
}

TEST_CASE("grazing a food plot trains Foraging -> Wisdom like an orb trains Scavenging", "[sim]") {
  // The food-plot mirror of the loot loop: a real graze now trains a Foraging skill feeding the new
  // Wisdom attribute (the first WIS trainer), just as grabbing an orb trains Scavenging -> Luck. A
  // grazer carrying the progression pair learns; a bare Stats-only eater (the other graze tests)
  // trains nothing, so those stay bit-identical.
  entt::registry reg;
  const entt::entity plot = reg.create();
  reg.emplace<eng::sim::Transform>(plot, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::FoodSource>(plot);  // default stock (100)
  const entt::entity grazer = reg.create();
  reg.emplace<eng::sim::Transform>(grazer, eng::Vec2{10.0f, 0.0f});  // within reach
  reg.emplace<eng::sim::Stats>(grazer).hunger.current = 10.0f;       // hungry -> actually eats
  reg.emplace<eng::sim::Attributes>(grazer);
  reg.emplace<eng::sim::Skills>(grazer);

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i)
    eng::sim::graze(reg, dt);  // one second grazing

  REQUIRE(reg.get<eng::sim::Skills>(grazer).find(eng::sim::SkillId::Foraging) !=
          nullptr);  // learned Foraging by eating...
  REQUIRE(reg.get<eng::sim::Attributes>(grazer).wisdom.xp > eng::Fixed{});  // ...which fed Wisdom
}

TEST_CASE("a wiser forager eats more from the same plot: Wisdom raises graze yield", "[sim]") {
  // Wisdom's first EFFECT (the reason the attribute isn't dead weight): each level past the first
  // lifts how much a food plot yields per tick, so a seasoned forager tops off faster than a novice
  // at the SAME patch. Level 1 is the base rate (bit-identical to the pre-Wisdom graze).
  const auto fed_in_one_tick = [](int wisdom_level) {
    entt::registry reg;
    const entt::entity plot = reg.create();
    reg.emplace<eng::sim::Transform>(plot, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::FoodSource>(plot);  // plenty of stock, so the bite isn't stock-limited
    const entt::entity grazer = reg.create();
    reg.emplace<eng::sim::Transform>(grazer, eng::Vec2{10.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(grazer).hunger.current = 0.0f;  // very hungry -> lots of room
    reg.emplace<eng::sim::Attributes>(grazer).wisdom.level = wisdom_level;

    eng::sim::graze(reg, static_cast<float>(eng::sim::kSecondsPerTick));  // one tick
    return reg.get<eng::sim::Stats>(grazer).hunger.current;  // how much it ate that tick
  };
  const float novice = fed_in_one_tick(1);
  const float sage = fed_in_one_tick(10);
  REQUIRE(novice > 0.0f);
  REQUIRE(sage > novice);  // the wiser forager drew more from the same plot in the same tick
}

TEST_CASE("a hungry NPC forages a stocked food plot but ignores a bare one", "[sim]") {
  // The forage rung now seeks food PLOTS as well as loot orbs — closing the "quiet corner with no
  // orbs" starvation gap. A stocked plot pulls a hungry NPC in; a bare one (stock 0) isn't a meal.
  const auto npc_steer_x = [](float plot_stock) {
    entt::registry reg;
    const entt::entity plot = reg.create();
    reg.emplace<eng::sim::Transform>(plot,
                                     eng::Vec2{200.0f, 0.0f});  // +x, inside forage radius 260
    eng::sim::FoodSource fs{};
    fs.stock = plot_stock;
    reg.emplace<eng::sim::FoodSource>(plot, fs);
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);
    reg.emplace<eng::sim::Stats>(npc).hunger.current = 10.0f;  // hungry; full water -> not thirsty
    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(npc).value.x;
  };
  REQUIRE(npc_steer_x(100.0f) > 0.0f);  // a stocked plot pulls the hungry NPC toward it (+x)...
  REQUIRE(npc_steer_x(0.0f) == 0.0f);   // ...a bare one doesn't (falls through to drift, no weapon)
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

TEST_CASE("a starving colonist trudges: the Need debuff drags every step slower", "[sim]") {
  // need_efficiency reaches the LEGS, not just the swing: the same 1.0-at-comfort -> 0.5-at-empty
  // curve that saps combat damage now scales steer speed, so the emptier the belly the heavier the
  // step. Two colonists forage the same orb, differing ONLY in how hungry they are; both still head
  // for it (never frozen — the 0.5 floor), but the starving one crawls. Full water on both, so
  // hunger is the worst need and the sole difference.
  const auto forage_speed = [](float hunger) {
    entt::registry reg;
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);
    reg.emplace<eng::sim::Stats>(npc).hunger.current =
        hunger;  // full water; hunger is the worst need
    const entt::entity orb = reg.create();
    reg.emplace<eng::sim::Transform>(orb,
                                     eng::Vec2{100.0f, 0.0f});  // food to the +x, in forage range
    reg.emplace<eng::sim::Pickup>(orb);
    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(npc).value.x;  // speed toward the orb
  };
  const float hungry = forage_speed(10.0f);   // 10% of max -> need_efficiency 0.7
  const float starving = forage_speed(0.0f);  // bone-empty -> need_efficiency at its 0.5 floor
  REQUIRE(hungry > 0.0f);                     // a hungry colonist heads for food...
  REQUIRE(starving >
          0.0f);  // ...a starving one still crawls toward it (the floor, never frozen)...
  REQUIRE(starving < hungry);  // ...but slower: an emptier belly is a heavier step
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

TEST_CASE("wisdom sharpens danger awareness: an alert forager senses a hazard from further",
          "[sim]") {
  // Wisdom's SECOND effect (beyond forage yield): it widens the flee sense radius, a distinct
  // source from bravery's nerve. Two neutral-bravery NPCs the same distance from one hazard, JUST
  // beyond the base 120 radius — a WIS-1 one never senses it (holds), but a WIS-trained one
  // perceives it from further and flees.
  const auto flees = [](int wisdom_level) {
    entt::registry reg;
    const entt::entity hazard = reg.create();
    reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Hazard>(hazard);
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{130.0f, 0.0f});  // 130 > the base radius 120
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);  // no Personality -> neutral bravery (base radius)
    reg.emplace<eng::sim::Attributes>(npc).wisdom.level = wisdom_level;
    eng::sim::steer_npcs(reg);
    return glm::length(reg.get<eng::sim::Velocity>(npc).value) > 0.0f;  // moved = sensed and fled
  };
  REQUIRE_FALSE(flees(1));  // WIS 1: radius 120 < 130 -> the hazard is beyond its senses
  REQUIRE(flees(10));       // WIS 10: radius 120 * 1.45 = 174 > 130 -> it senses and flees
}

TEST_CASE("a colonist flees a villain player: standing's first gameplay reader", "[sim]") {
  // Cruelty finally BITES the world, not just the HUD: once a player's deeds mark them a villain
  // (standing at or below the -15 "Suspect" line), a nearby colonist flees them exactly like a
  // hazard. This is the first time `standing` changes the SIM — the colony recoiling from someone
  // it has cause to fear.
  entt::registry reg;
  const entt::entity villain = reg.create();
  reg.emplace<eng::sim::Transform>(villain, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(villain);
  eng::sim::record_deed(reg, villain, eng::sim::Deed::Cruelty,
                        3);  // 3 cruel strikes -> standing -18
  REQUIRE(eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(villain)) <= -eng::sim::kKnownAt);

  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist,
                                   eng::Vec2{50.0f, 0.0f});  // inside the base sense radius
  reg.emplace<eng::sim::Velocity>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);

  eng::sim::steer_npcs(reg);

  // Flees RIGHT — straight away from the villain at the origin (pos - threat points +x).
  REQUIRE(reg.get<eng::sim::Velocity>(colonist).value.x > 0.0f);
}

TEST_CASE("only villainy drives the colony off: a hero or unproven player is not feared", "[sim]") {
  // Fear fires ONLY at/below the Suspect line, so a non-villain is never FLED (never +x, away from
  // the player at the origin). A celebrated hero is the opposite — it RALLIES the colonist TOWARD
  // it
  // (-x; see the rally tests) — and an unproven player (no ledger) is neither feared nor rallied,
  // so its colonist stays at rest. Neither reads as a threat.
  const auto colonist_flee_x = [](bool make_hero) {
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::PlayerControlled>(player);
    if (make_hero) {
      eng::sim::record_deed(reg, player, eng::sim::Deed::Valor, 10);  // standing +50, a clear hero
    }
    const entt::entity colonist = reg.create();
    reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{50.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(colonist);
    reg.emplace<eng::sim::Npc>(colonist);
    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(colonist).value.x;  // > 0 would be fleeing (away)
  };
  REQUIRE(colonist_flee_x(true) < 0.0f);            // a hero -> rallied toward (-x), never fled
  REQUIRE(colonist_flee_x(false) == Approx(0.0f));  // an unproven player -> unmoved, not fled
}

TEST_CASE("a downed villain is no threat: fear yields, and the colony moves to help", "[sim]") {
  // The Downed exclusion on the fear view, pinned by PRIORITY. Fear beats rescue (it's checked
  // first and bolts straight away), so if a downed villain still read as a threat the colonist
  // would flee it (+x, away). Instead the colonist runs TOWARD the fallen body (-x) to rescue it —
  // which it can only do because the villain was dropped from the fear view. (Whether colonists
  // SHOULD save a villain is a separate, standing-blind rescue rung — a later refinement, not this
  // reader's job.)
  entt::registry reg;
  const entt::entity villain = reg.create();
  reg.emplace<eng::sim::Transform>(villain, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(villain);
  reg.emplace<eng::sim::Downed>(villain);  // helpless -> excluded from fear
  eng::sim::record_deed(reg, villain, eng::sim::Deed::Cruelty, 5);  // Notorious (-30), but down

  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{50.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);

  eng::sim::steer_npcs(reg);

  // Toward the fallen villain (-x) to rescue, NOT fleeing away (+x): the fear reader skipped it.
  REQUIRE(reg.get<eng::sim::Velocity>(colonist).value.x < 0.0f);
}

TEST_CASE("an idle colonist rallies to a renowned hero: the twin of villain-fear", "[sim]") {
  // The inverted mirror of the flee: a colonist with nothing urgent to do drifts TOWARD a player
  // whose deeds have earned "Known"+ standing (>= +kKnownAt), gathering around its champion — the
  // exact opposite of fleeing a Suspect villain.
  entt::registry reg;
  const entt::entity hero = reg.create();
  reg.emplace<eng::sim::Transform>(hero, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(hero);
  eng::sim::record_deed(reg, hero, eng::sim::Deed::Valor, 3);  // +15 standing -> "Known"
  REQUIRE(eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(hero)) >= eng::sim::kKnownAt);

  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{50.0f, 0.0f});  // within rally radius, idle
  reg.emplace<eng::sim::Velocity>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);

  eng::sim::steer_npcs(reg);

  // Steers LEFT (-x), toward the hero at the origin.
  REQUIRE(reg.get<eng::sim::Velocity>(colonist).value.x < 0.0f);
}

TEST_CASE("rally is the lowest priority: a real need overrides the pull of a hero", "[sim]") {
  // Rally never overrides survival. A colonist beside a renowned hero but ALSO within reach of a
  // hazard flees the hazard (priority 1) — it does not drift to the champion while in danger. Both
  // the hero and the hazard sit to the LEFT, so a rally would pull the colonist left; fleeing sends
  // it right, proving which rung won.
  entt::registry reg;
  const entt::entity hero = reg.create();
  reg.emplace<eng::sim::Transform>(hero, eng::Vec2{-50.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(hero);
  eng::sim::record_deed(reg, hero, eng::sim::Deed::Valor, 5);  // +25 -> clearly Known+
  const entt::entity hazard = reg.create();
  reg.emplace<eng::sim::Transform>(hazard, eng::Vec2{-40.0f, 0.0f});  // close, to the left
  reg.emplace<eng::sim::Hazard>(hazard);
  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);

  eng::sim::steer_npcs(reg);

  // Flees RIGHT (+x) from the hazard — survival beats rally, so it does NOT drift left to the hero.
  REQUIRE(reg.get<eng::sim::Velocity>(colonist).value.x > 0.0f);
}

TEST_CASE("rally needs a real hero: below the Known line, an idle colonist stays put", "[sim]") {
  // The threshold + bit-identical guard (mirror of the fear side): rally fires ONLY at/above
  // +kKnownAt. An unproven player (no ledger) and a merely-positive one below the line both leave
  // an idle colonist adrift — no pull until you're a bona fide hero.
  const auto idle_colonist_speed = [](int valor_deeds) {
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::PlayerControlled>(player);
    if (valor_deeds > 0) eng::sim::record_deed(reg, player, eng::sim::Deed::Valor, valor_deeds);
    const entt::entity colonist = reg.create();
    reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{50.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(colonist);
    reg.emplace<eng::sim::Npc>(colonist);
    eng::sim::steer_npcs(reg);
    const eng::Vec2 v = reg.get<eng::sim::Velocity>(colonist).value;
    return v.x * v.x + v.y * v.y;  // squared speed — zero iff it never moved
  };
  REQUIRE(idle_colonist_speed(0) == Approx(0.0f));  // unproven (no ledger) -> no pull
  REQUIRE(idle_colonist_speed(2) ==
          Approx(0.0f));  // +10 standing, below Known (15) -> still no pull
}

TEST_CASE("sociability shapes how far an idle colonist travels to rally to a hero", "[sim]") {
  // The fifth personality axis, read by the rally rung's radius exactly as industry reads the
  // arm-up radius (so every acting steer rung now reads a trait). Base rally radius is 220; a hero
  // sits 150 away. A SOCIABLE colonist (+100 -> radius 330) crosses to gather round the champion; a
  // LONER
  // (-100 -> radius 110) stays put — same distance, opposite pulls.
  entt::registry reg;
  const entt::entity hero = reg.create();
  reg.emplace<eng::sim::Transform>(hero, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::PlayerControlled>(hero);
  eng::sim::record_deed(reg, hero, eng::sim::Deed::Valor, 5);  // +25 standing -> clearly Known+

  const entt::entity sociable = reg.create();
  reg.emplace<eng::sim::Transform>(sociable, eng::Vec2{150.0f, 0.0f});  // 150 from the hero
  reg.emplace<eng::sim::Velocity>(sociable);
  reg.emplace<eng::sim::Npc>(sociable);
  reg.emplace<eng::sim::Personality>(sociable,
                                     eng::sim::Personality{0, 0, 0, 0, 100});  // sociable -> 330

  const entt::entity loner = reg.create();
  reg.emplace<eng::sim::Transform>(loner, eng::Vec2{0.0f, 150.0f});  // also 150 from the hero
  reg.emplace<eng::sim::Velocity>(loner);
  reg.emplace<eng::sim::Npc>(loner);
  reg.emplace<eng::sim::Personality>(loner,
                                     eng::sim::Personality{0, 0, 0, 0, -100});  // loner -> 110

  eng::sim::steer_npcs(reg);

  // The sociable one steers toward the hero (-x, toward the origin)...
  REQUIRE(reg.get<eng::sim::Velocity>(sociable).value.x < 0.0f);
  // ...the loner, out of its shrunk radius, stays put.
  REQUIRE(reg.get<eng::sim::Velocity>(loner).value.x == Approx(0.0f));
  REQUIRE(reg.get<eng::sim::Velocity>(loner).value.y == Approx(0.0f));
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

TEST_CASE("industry shapes how far an NPC ranges to arm itself", "[sim]") {
  // The fourth axis, on the LAST unpersonalized rung (arm-up), so now EVERY want in the steer
  // ladder reads personality. It reuses bravery's RADIUS shape on a new want: the industrious range
  // across the field to loot a weapon, the idle only grab one underfoot. Distances straddle the
  // base seek radius (260), so each assertion pins the industry bonus rather than the baseline.
  entt::registry reg;
  const entt::entity weapon = reg.create();
  reg.emplace<eng::sim::Transform>(weapon, eng::Vec2{0.0f, 0.0f});  // the loot, at the origin
  reg.emplace<eng::sim::Weapon>(weapon);

  // Industrious, FAR out at 320 — beyond the base radius (260), so only a GROWN radius reaches it.
  const entt::entity keen = reg.create();
  reg.emplace<eng::sim::Transform>(keen, eng::Vec2{320.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(keen);
  reg.emplace<eng::sim::Npc>(keen);
  reg.emplace<eng::sim::Stats>(keen);  // full hunger -> not foraging, falls through to arming
  reg.emplace<eng::sim::Personality>(keen, eng::sim::Personality{0, 0, 0, 100});  // industry +100

  // Neutral, at 200 — inside the base radius, so the baseline DOES seek (the positive control).
  const entt::entity plain = reg.create();
  reg.emplace<eng::sim::Transform>(plain, eng::Vec2{200.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(plain);
  reg.emplace<eng::sim::Npc>(plain);
  reg.emplace<eng::sim::Stats>(plain);  // NO Personality -> base seek radius (unchanged)

  // Idle, also at 200 — where the neutral one sets off, but its SHRUNK radius no longer reaches.
  const entt::entity idle = reg.create();
  reg.emplace<eng::sim::Transform>(idle, eng::Vec2{200.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(idle);
  reg.emplace<eng::sim::Npc>(idle);
  reg.emplace<eng::sim::Stats>(idle);
  reg.emplace<eng::sim::Personality>(idle, eng::sim::Personality{0, 0, 0, -100});  // industry -100

  eng::sim::steer_npcs(reg);

  // GROW: the industrious one steers toward the blade (-x) from 320 — past where the base radius
  // reaches, so the bonus, not the baseline, brought it in range.
  REQUIRE(reg.get<eng::sim::Velocity>(keen).value.x < 0.0f);
  // Baseline: the neutral one, inside the base radius, sets off.
  REQUIRE(reg.get<eng::sim::Velocity>(plain).value.x < 0.0f);
  // SHRINK: the idle one, at the SAME 200 the neutral seeks from, holds still — its radius shrank
  // below the distance, so its velocity is untouched from its zero start.
  const eng::Vec2 idle_v = reg.get<eng::sim::Velocity>(idle).value;
  REQUIRE(idle_v.x == 0.0f);
  REQUIRE(idle_v.y == 0.0f);
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

TEST_CASE("a DamagePlayer command blinks the player white: hit-flash parity", "[sim]") {
  // Every OTHER damage source (creature melee, a hazard mote, a projectile) stamps a hit-flash on
  // its victim; the DamagePlayer command must do the same, so a hit through the funnel gives the
  // player the same "I got hit" feedback. No flash while unhurt; a HitFlash once the command lands.
  eng::sim::World world;
  const entt::entity player = world.player();
  REQUIRE_FALSE(world.registry().all_of<eng::sim::HitFlash>(player));  // unhurt: no flash

  world.submit(eng::sim::damage_player(eng::sim::kLocalPlayer, 15.0f));
  world.step();

  REQUIRE(world.registry().all_of<eng::sim::HitFlash>(player));  // took a hit -> blinks white
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
  stats.water.current = 0.0f;  // ...and parched (each Need must come back full or it re-downs you)
  reg.emplace<eng::sim::Poisoned>(player, eng::sim::Poisoned{5.0f, 10.0f});  // ...and envenomed

  const eng::Vec2 centre{640.0f, 360.0f};
  eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // 1st call: goes Downed (no ally near)
  REQUIRE(reg.all_of<eng::sim::Downed>(player));
  eng::sim::handle_deaths(reg, centre, 6.0f);  // a fat dt expires the 5s timer -> respawn

  const eng::sim::Stats& after = reg.get<eng::sim::Stats>(player);
  REQUIRE_FALSE(reg.all_of<eng::sim::Downed>(player));          // back up...
  REQUIRE(after.health.current == Approx(after.health.max));    // ...at full health...
  REQUIRE(after.hunger.current == Approx(after.hunger.max));    // ...fed (not re-starving)...
  REQUIRE(after.stamina.current == Approx(after.stamina.max));  // ...and rested...
  REQUIRE(after.water.current == Approx(after.water.max));  // ...and watered (not re-dehydrating)
  REQUIRE_FALSE(
      reg.all_of<eng::sim::Poisoned>(player));  // ...and cured of venom (no lethal status)
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

TEST_CASE("record_deed accrues on the ledger and standing weighs the dimensions", "[sim]") {
  // The morality write-point and the derived standing scalar, in isolation. A brand-new entity has
  // NO ledger (lazy — it earns one only on its first deed); one Charity deed lifts standing, and an
  // oppositely-signed Cruelty deed sinks it, pinning both the accumulate and the SIGNED formula.
  entt::registry reg;
  const entt::entity e = reg.create();
  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(e) == nullptr);  // no deed yet -> no component

  eng::sim::record_deed(reg, e, eng::sim::Deed::Charity, 1);
  REQUIRE(eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(e)) == 4);  // Charity ×4

  eng::sim::record_deed(reg, e, eng::sim::Deed::Cruelty, 1);
  REQUIRE(eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(e)) == -2);  // 4 - Cruelty ×6
}

TEST_CASE("standing decays toward neutral over time: reputation fades if unrenewed", "[sim]") {
  // The design's leaky redemption/corruption — every kDecayPeriod ticks a deed dimension creeps one
  // step toward 0, so a HERO's fame fades and a VILLAIN climbs back if they stop acting, symmetric
  // about neutral. A short run leaves standing untouched (the counter hasn't crossed a period), so
  // the pre-decay world is bit-identical; a long run drifts both toward 0 but never past it.
  entt::registry reg;
  const entt::entity hero = reg.create();
  eng::sim::record_deed(reg, hero, eng::sim::Deed::Valor, 10);  // +50 standing (Valor ×5)
  const entt::entity villain = reg.create();
  eng::sim::record_deed(reg, villain, eng::sim::Deed::Cruelty, 10);  // -60 standing (Cruelty ×6)
  const auto std_of = [&](entt::entity x) {
    return eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(x));
  };
  const int hero_before = std_of(hero);        // +50
  const int villain_before = std_of(villain);  // -60

  // A brief run: the counter accrues but no period has elapsed, so standing is UNCHANGED.
  for (int i = 0; i < 100; ++i) eng::sim::decay_standing(reg);
  REQUIRE(std_of(hero) == hero_before);
  REQUIRE(std_of(villain) == villain_before);

  // A long run (many decay periods): both leak TOWARD neutral, neither crossing it.
  for (int i = 0; i < 40000; ++i) eng::sim::decay_standing(reg);
  REQUIRE(std_of(hero) < hero_before);        // the hero's fame faded...
  REQUIRE(std_of(hero) >= 0);                 // ...but not into villainy
  REQUIRE(std_of(villain) > villain_before);  // the villain redeemed toward neutral...
  REQUIRE(std_of(villain) <= 0);              // ...but hasn't overshot into heroism
}

TEST_CASE("bonds decay toward neutral but the deepest latch: a Partner and Nemesis hold", "[sim]") {
  // The relationships twin of standing decay — an UNLATCHED tie cools toward 0 over time, but a
  // deep bond (Partner, >= +80) or grudge (Nemesis, <= -60) LATCHES and persists (bond_latched). A
  // short run is bit-identical (no whole period elapsed); a long run fades the casual tie while the
  // latched ones hold fast.
  entt::registry reg;
  const entt::entity a = reg.create();
  const entt::entity casual = reg.create();
  const entt::entity partner = reg.create();
  const entt::entity nemesis = reg.create();
  eng::sim::nudge_affinity(reg, a, casual, 30);    // a casual friendship (unlatched)
  eng::sim::nudge_affinity(reg, a, partner, 90);   // a Partner (latched, >= +80)
  eng::sim::nudge_affinity(reg, a, nemesis, -80);  // a Nemesis (latched, <= -60)
  const auto aff = [&](entt::entity to) { return eng::sim::affinity_toward(reg, a, to); };
  const std::int8_t casual_before = aff(casual);  // 30

  // A brief run: no whole period elapsed, so every edge is UNCHANGED (bit-identical).
  for (int i = 0; i < 100; ++i) eng::sim::decay_bonds(reg);
  REQUIRE(aff(casual) == casual_before);
  REQUIRE(aff(partner) == 90);
  REQUIRE(aff(nemesis) == -80);

  // A long run: the casual tie cools toward neutral, but the latched Partner and Nemesis hold.
  for (int i = 0; i < 40000; ++i) eng::sim::decay_bonds(reg);
  REQUIRE(aff(casual) < casual_before);  // the casual friendship faded...
  REQUIRE(aff(casual) >= 0);             // ...toward neutral, not past
  REQUIRE(aff(partner) == 90);           // the Partner latch held fast...
  REQUIRE(aff(nemesis) == -80);          // ...and so did the Nemesis
}

TEST_CASE("standing weights each deed dimension by the design's signed factors", "[sim]") {
  // The full ×5 formula ships now though only Charity is fed by a deed yet, so pin EVERY term's
  // weight and SIGN — a wrong factor on an as-yet-unfed dimension would otherwise ship silently and
  // corrupt the first deed that comes to feed it. One deed of magnitude 1 per kind isolates its
  // weight.
  const auto standing_of = [](eng::sim::Deed k) {
    entt::registry reg;
    const entt::entity e = reg.create();
    eng::sim::record_deed(reg, e, k, 1);
    return eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(e));
  };
  REQUIRE(standing_of(eng::sim::Deed::Charity) == 4);    // .8 ×5
  REQUIRE(standing_of(eng::sim::Deed::Valor) == 5);      // 1.0 ×5
  REQUIRE(standing_of(eng::sim::Deed::Honesty) == 3);    // .6 ×5
  REQUIRE(standing_of(eng::sim::Deed::Loyalty) == 3);    // .6 ×5
  REQUIRE(standing_of(eng::sim::Deed::Cruelty) == -6);   // -1.2 ×5
  REQUIRE(standing_of(eng::sim::Deed::Violence) == -4);  // -.8 ×5
}

TEST_CASE("a deed drifts the actor's matching personality axis, bounded and clamped", "[sim]") {
  // The design's "you are what you do": recording a deed nudges the actor's matching Personality
  // axis a bounded step — Valor hardens bravery, Charity softens toward compassion, Loyalty deepens
  // the loyalty leaning — so a character is reshaped by its deeds. The drift CLAMPS at the ±100
  // bound.
  entt::registry reg;
  const entt::entity n = reg.create();
  reg.emplace<eng::sim::Personality>(n, eng::sim::Personality{0, 0, 0, 0});

  eng::sim::record_deed(reg, n, eng::sim::Deed::Valor, 1);
  eng::sim::record_deed(reg, n, eng::sim::Deed::Valor, 1);
  REQUIRE(reg.get<eng::sim::Personality>(n).bravery == 4);     // two Valor deeds -> +2 each
  REQUIRE(reg.get<eng::sim::Personality>(n).compassion == 0);  // Valor doesn't touch compassion

  eng::sim::record_deed(reg, n, eng::sim::Deed::Charity, 1);
  REQUIRE(reg.get<eng::sim::Personality>(n).compassion == 2);  // Charity -> compassion
  REQUIRE(reg.get<eng::sim::Personality>(n).bravery == 4);     // ...and leaves bravery alone

  eng::sim::record_deed(reg, n, eng::sim::Deed::Loyalty, 1);
  REQUIRE(reg.get<eng::sim::Personality>(n).loyalty == 2);     // Loyalty -> the loyalty axis...
  REQUIRE(reg.get<eng::sim::Personality>(n).compassion == 2);  // ...and leaves compassion alone

  // A long heroic career CLAMPS at the axis bound rather than overflowing the int8.
  for (int i = 0; i < 100; ++i) eng::sim::record_deed(reg, n, eng::sim::Deed::Valor, 1);
  REQUIRE(reg.get<eng::sim::Personality>(n).bravery == 100);  // pinned at +100, no wrap
}

TEST_CASE("a deed on an entity with no Personality drifts nothing (stays Personality-free)",
          "[sim]") {
  // Drift is try_get, never emplace: an actor without a Personality — the player, every creature —
  // must accrue the deed on its ledger yet NOT sprout a Personality, or the bit-identical
  // absent-Personality world would break.
  entt::registry reg;
  const entt::entity e = reg.create();  // no Personality

  eng::sim::record_deed(reg, e, eng::sim::Deed::Valor, 1);

  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(e) !=
          nullptr);  // the deed still landed on the ledger...
  REQUIRE(eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(e)) == 5);
  REQUIRE(reg.try_get<eng::sim::Personality>(e) == nullptr);  // ...but no Personality was conjured
}

TEST_CASE("rescuing a downed ally records a Charity deed on the rescuer", "[sim]") {
  // The first deed wired to a live event: completing a rescue in handle_deaths credits the RESCUER
  // (not the rescued) with Charity. It fires identically whether the rescuer is an NPC or a player,
  // because the allies view spans both — morality gets player==NPC parity for free.
  const auto rescue_then_standing = [](bool rescuer_is_player) {
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{100.0f, 100.0f});
    reg.emplace<eng::sim::PlayerControlled>(player);
    reg.emplace<eng::sim::Velocity>(player);
    reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // down
    const entt::entity ally = reg.create();
    reg.emplace<eng::sim::Transform>(ally, eng::Vec2{110.0f, 100.0f});  // within revive reach
    reg.emplace<eng::sim::Velocity>(ally);
    reg.emplace<eng::sim::Stats>(ally);  // full health -> alive
    if (rescuer_is_player)
      reg.emplace<eng::sim::PlayerControlled>(ally);
    else
      reg.emplace<eng::sim::Npc>(ally);

    const eng::Vec2 centre{640.0f, 360.0f};
    eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // player goes Downed
    eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // ally in reach -> revive + deed

    REQUIRE_FALSE(reg.all_of<eng::sim::Downed>(player));  // the rescue happened...
    REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(player) ==
            nullptr);  // ...the RESCUED earns nothing
    return eng::sim::standing(reg.get<eng::sim::BehaviorLedger>(ally));  // the RESCUER's credit
  };

  const auto npc_credit = rescue_then_standing(false);
  const auto player_credit = rescue_then_standing(true);
  REQUIRE(npc_credit == 4);              // one Charity deed -> standing +4...
  REQUIRE(player_credit == npc_credit);  // ...identical for a player rescuer (parity)
}

TEST_CASE("rescuing a bonded ally is loyalty too: a friend save records both deeds", "[sim]") {
  // The dormant Loyalty dimension lands on the rescue path. Hauling up a STRANGER is charity only,
  // but hauling up someone the rescuer was ALREADY bonded to (affinity at/above the +10 bond floor)
  // is also LOYALTY — standing by your own. Gated on the tie that existed BEFORE the rescue's own
  // affinity nudge, so a first save of a stranger never counts as loyalty.
  using eng::sim::BehaviorLedger;
  using eng::sim::Deed;
  const auto dim = [](const BehaviorLedger& l, Deed k) {
    return l.dims[static_cast<std::size_t>(k)];
  };

  // Run one rescue where the rescuer starts with `prebond` affinity toward the fallen; return the
  // rescuer's ledger afterward (same geometry as "a living ally revives a downed player in place").
  const auto rescue_with_prebond = [](std::int8_t prebond) {
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{100.0f, 100.0f});
    reg.emplace<eng::sim::PlayerControlled>(player);
    reg.emplace<eng::sim::Velocity>(player);
    reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // down
    const entt::entity ally = reg.create();
    reg.emplace<eng::sim::Transform>(ally, eng::Vec2{110.0f, 100.0f});  // within revive reach
    reg.emplace<eng::sim::Npc>(ally);
    reg.emplace<eng::sim::Stats>(ally);
    if (prebond != 0) eng::sim::nudge_affinity(reg, ally, player, prebond);  // a prior tie

    const eng::Vec2 centre{640.0f, 360.0f};
    eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // player goes Downed
    eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // ally in reach -> revive + deeds
    return reg.get<BehaviorLedger>(ally);
  };

  SECTION("a stranger save is charity only") {
    const BehaviorLedger led = rescue_with_prebond(0);  // affinity 0 -> below the bond floor
    REQUIRE(dim(led, Deed::Charity) == 1);
    REQUIRE(dim(led, Deed::Loyalty) == 0);  // no prior bond -> no loyalty
  }
  SECTION("a bonded save is charity AND loyalty") {
    const BehaviorLedger led = rescue_with_prebond(10);  // at the +10 bond floor (kBondPull)
    REQUIRE(dim(led, Deed::Charity) == 1);
    REQUIRE(dim(led, Deed::Loyalty) == 1);  // stood by a friend -> loyalty too
  }
}

TEST_CASE("nudge_affinity forms one directed edge and deepens it, clamped", "[sim]") {
  // The single relationships write-point (the record_deed twin): the first nudge appends a directed
  // edge, a second to the SAME target deepens it (not a duplicate), and repeated nudges saturate at
  // ±100 rather than overflowing the int8.
  entt::registry reg;
  const entt::entity a = reg.create();
  const entt::entity b = reg.create();

  eng::sim::nudge_affinity(reg, a, b, 20);
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges.size() == 1u);   // one edge appended...
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges[0].other == b);  // ...directed toward b
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges[0].affinity == 20);

  eng::sim::nudge_affinity(reg, a, b, 20);
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges.size() == 1u);  // deepened, not duplicated
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges[0].affinity == 40);

  for (int i = 0; i < 10; ++i) eng::sim::nudge_affinity(reg, a, b, 100);
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges[0].affinity == 100);  // saturates, no overflow
  for (int i = 0; i < 20; ++i) eng::sim::nudge_affinity(reg, a, b, -100);
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges[0].affinity == -100);  // symmetric floor

  // The clamp holds on FIRST append too, not just on deepen: a big first nudge to a NEW target
  // lands at the band edge, never out of range.
  const entt::entity c = reg.create();
  eng::sim::nudge_affinity(reg, a, c, 120);
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges.size() == 2u);  // a second edge appended
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges[1].other == c);
  REQUIRE(reg.get<eng::sim::Relationships>(a).edges[1].affinity == 100);  // clamped on append
}

TEST_CASE("the public hero-rally still beats a personal bond", "[sim]") {
  // The gating claim that keeps the public rally byte-identical: the bond-pull sits BELOW the
  // hero-rally, so when a renowned hero is present it claims the rung first. A colonist with a
  // bonded friend to its LEFT and a Known+ hero to its RIGHT gathers to the HERO (+x), not the
  // friend.
  entt::registry reg;
  const entt::entity hero = reg.create();
  reg.emplace<eng::sim::Transform>(hero, eng::Vec2{100.0f, 0.0f});  // to the right
  reg.emplace<eng::sim::PlayerControlled>(hero);
  eng::sim::record_deed(reg, hero, eng::sim::Deed::Valor, 5);  // +25 standing -> Known+

  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);
  const entt::entity friend_e = reg.create();
  reg.emplace<eng::sim::Transform>(friend_e, eng::Vec2{-100.0f, 0.0f});  // to the left
  eng::sim::nudge_affinity(reg, colonist, friend_e, 30);

  eng::sim::steer_npcs(reg);

  // Toward the hero (+x), NOT the friend (-x): the hero-rally claimed the rung first.
  REQUIRE(reg.get<eng::sim::Velocity>(colonist).value.x > 0.0f);
}

TEST_CASE("a rescue forms a bond: the rescuer gains affinity toward the one it saved", "[sim]") {
  // The relationships seed's one forming event, wired at the same rescue site as the Charity deed:
  // the RESCUER (not the rescued) gains a directed affinity edge toward the player it hauled up.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Velocity>(player);
  reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // down
  const entt::entity rescuer = reg.create();
  reg.emplace<eng::sim::Transform>(rescuer, eng::Vec2{110.0f, 100.0f});  // within revive reach
  reg.emplace<eng::sim::Velocity>(rescuer);
  reg.emplace<eng::sim::Stats>(rescuer);
  reg.emplace<eng::sim::Npc>(rescuer);
  REQUIRE(reg.try_get<eng::sim::Relationships>(rescuer) == nullptr);  // no bond before (lazy)

  const eng::Vec2 centre{640.0f, 360.0f};
  eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // player goes Downed
  eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // rescuer in reach -> revive + bond

  const eng::sim::Relationships* rel = reg.try_get<eng::sim::Relationships>(rescuer);
  REQUIRE(rel != nullptr);
  REQUIRE(rel->edges.size() == 1u);
  REQUIRE(rel->edges[0].other == player);  // directed toward the saved player
  REQUIRE(rel->edges[0].affinity == 20);   // kRescueAffinity
  REQUIRE(reg.try_get<eng::sim::Relationships>(player) == nullptr);  // the rescued forms no edge
}

TEST_CASE("an idle colonist drifts toward a friend it has bonded with", "[sim]") {
  // The first reader of the seed: with no hero to rally to, an idle colonist gathers toward its
  // nearest positive-affinity friend (e.g. the player it rescued). Base bond radius is 220.
  const auto colonist_velocity_x = [](float friend_x) {
    entt::registry reg;
    const entt::entity colonist = reg.create();
    reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(colonist);
    reg.emplace<eng::sim::Npc>(colonist);
    const entt::entity friend_e = reg.create();
    reg.emplace<eng::sim::Transform>(friend_e, eng::Vec2{friend_x, 0.0f});
    eng::sim::nudge_affinity(reg, colonist, friend_e, 30);  // a real fondness (>= kBondPull 10)

    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(colonist).value.x;
  };
  REQUIRE(colonist_velocity_x(150.0f) > 0.0f);           // friend in range -> drift toward (+x)
  REQUIRE(colonist_velocity_x(400.0f) == Approx(0.0f));  // friend past the bond radius -> no pull
}

TEST_CASE("a grudge-holder keeps its distance: an idle colonist backs away from the resented",
          "[sim]") {
  // The ACTIVE completion of a grudge and the negative twin of the bond pull above: an idle
  // colonist that resents a nearby (non-downed) entity — affinity <= the grudge threshold, e.g. a
  // player who struck it — steers AWAY from it, where a friend it would drift toward. A neutral tie
  // triggers neither (it stays idle); a positive tie falls through to the bond pull. Base avoid
  // radius 150.
  const auto colonist_velocity_x = [](std::int8_t affinity) {
    entt::registry reg;
    const entt::entity colonist = reg.create();
    reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(colonist);
    reg.emplace<eng::sim::Npc>(colonist);
    const entt::entity other = reg.create();
    reg.emplace<eng::sim::Transform>(other, eng::Vec2{100.0f, 0.0f});  // 100 away, +x (within 150)
    eng::sim::nudge_affinity(reg, colonist, other, affinity);          // how it regards `other`

    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(colonist).value.x;
  };
  REQUIRE(colonist_velocity_x(-30) < 0.0f);         // resented (<= -20) -> backs AWAY (-x)
  REQUIRE(colonist_velocity_x(0) == Approx(0.0f));  // neutral -> neither avoid nor bond -> idle
  REQUIRE(colonist_velocity_x(30) > 0.0f);          // a friend (>= +10) -> bond pull draws it (+x)
}

TEST_CASE("loyalty shapes how far a colonist follows a bonded friend", "[sim]") {
  // The SIXTH and last personality axis, read by the bond-pull radius exactly as sociability reads
  // the rally radius (so every acting steer rung now reads a trait, and all six axes are wired).
  // Base bond radius is 220; a friend sits 150 away. A LOYAL colonist (+100 -> radius 330) crosses
  // to stay near it; a FICKLE one (-100 -> radius 110) stays put — same distance, opposite pulls.
  const auto colonist_velocity_x = [](int loyalty) {
    entt::registry reg;
    const entt::entity colonist = reg.create();
    reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(colonist);
    reg.emplace<eng::sim::Npc>(colonist);
    reg.emplace<eng::sim::Personality>(
        colonist, eng::sim::Personality{0, 0, 0, 0, 0, static_cast<std::int8_t>(loyalty)});
    const entt::entity friend_e = reg.create();
    reg.emplace<eng::sim::Transform>(friend_e, eng::Vec2{150.0f, 0.0f});  // 150 away
    eng::sim::nudge_affinity(reg, colonist, friend_e, 30);

    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(colonist).value.x;
  };
  REQUIRE(colonist_velocity_x(100) > 0.0f);            // loyal -> radius 330 -> follows (+x)
  REQUIRE(colonist_velocity_x(-100) == Approx(0.0f));  // fickle -> radius 110 < 150 -> stays put
}

TEST_CASE("a bond to a vanished friend is skipped, not dereferenced", "[sim]") {
  // The dangling-handle guard: edges store entity ids by value and ids recycle, so the reader gates
  // on reg.valid(other). A colonist bonded to an entity that is then destroyed steers nowhere and
  // does not crash (ASan would catch a stale read).
  entt::registry reg;
  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);
  const entt::entity friend_e = reg.create();
  reg.emplace<eng::sim::Transform>(friend_e, eng::Vec2{100.0f, 0.0f});
  eng::sim::nudge_affinity(reg, colonist, friend_e, 30);
  reg.destroy(friend_e);  // the friend vanishes before the reader runs

  eng::sim::steer_npcs(reg);  // must not dereference the stale edge
  REQUIRE(reg.get<eng::sim::Velocity>(colonist).value.x == Approx(0.0f));  // no pull toward a ghost
  REQUIRE(reg.get<eng::sim::Velocity>(colonist).value.y == Approx(0.0f));
}

TEST_CASE("a cruel strike earns a personal grudge: the victim resents the striker", "[sim]") {
  // The negative mirror of the rescue-bond: striking a peaceful colonist makes IT form negative
  // affinity toward the striker (a grudge), alongside the Cruelty deed the striker earns.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::PlayerControlled>(player);
  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist,
                                   eng::Vec2{20.0f, 0.0f});  // in reach, no hostile around
  reg.emplace<eng::sim::Stats>(colonist, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);

  std::mt19937 rng{1234};
  eng::sim::perform_attack(reg, player, rng);

  REQUIRE(eng::sim::affinity_toward(reg, colonist, player) < 0);  // the colonist now resents...
  REQUIRE(eng::sim::affinity_toward(reg, player, colonist) ==
          0);  // ...but the striker forms no tie
}

TEST_CASE("witnessed cruelty spreads: nearby colonists grudge the striker too", "[sim]") {
  // The negative mirror of camaraderie's bond_witnesses: a cruel strike makes not just the VICTIM
  // resent you but the bystanders who SAW it too (a smaller grudge), so cruelty earns a spreading
  // bad reputation. A colonist far from the strike sees nothing. The witness is a SEPARATE colonist
  // from the one struck.
  const auto witness_affinity = [](float witness_x) {
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(player);
    reg.emplace<eng::sim::Skills>(player);
    reg.emplace<eng::sim::PlayerControlled>(player);
    const entt::entity victim = reg.create();
    reg.emplace<eng::sim::Transform>(victim, eng::Vec2{20.0f, 0.0f});  // in reach -> the struck one
    reg.emplace<eng::sim::Stats>(victim, eng::sim::Vital{40.0f, 40.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(victim);
    reg.emplace<eng::sim::Npc>(victim);
    const entt::entity witness = reg.create();
    reg.emplace<eng::sim::Transform>(witness, eng::Vec2{witness_x, 0.0f});  // a bystander
    reg.emplace<eng::sim::Stats>(witness);
    reg.emplace<eng::sim::Npc>(witness);

    std::mt19937 rng{1234};
    eng::sim::perform_attack(reg, player,
                             rng);  // no hostile in reach -> a cruel strike on the victim
    return eng::sim::affinity_toward(reg, witness, player);
  };
  REQUIRE(witness_affinity(60.0f) < 0);    // a bystander near the strike resents the striker...
  REQUIRE(witness_affinity(500.0f) == 0);  // ...one far off saw nothing, so bears no grudge
}

TEST_CASE("felling a foe near an ally forges camaraderie: the witness bonds to the killer",
          "[sim]") {
  // The THIRD relationship-forming event ("fighting a common foe"), alongside the rescue-bond and
  // the cruelty-grudge: a killing blow on a hostile bonds nearby colonists TO the killer
  // (witness->killer), so the colony warms to a protector who fights beside it. An ally out of
  // reach of the skirmish forms no tie.
  const auto witness_affinity = [](float ally_x) {
    entt::registry reg;
    const entt::entity killer = reg.create();
    reg.emplace<eng::sim::Transform>(killer, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(killer);
    reg.emplace<eng::sim::Skills>(killer);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{10.0f, 0.0f});         // within melee reach
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{3.0f, 3.0f, 0.0f});  // frail: one blow
    reg.emplace<eng::sim::Enemy>(foe);                                     // ...and hostile
    const entt::entity ally = reg.create();
    reg.emplace<eng::sim::Transform>(ally, eng::Vec2{ally_x, 0.0f});
    reg.emplace<eng::sim::Npc>(ally);  // a peaceful colonist bystander (not the target)

    std::mt19937 rng{42};
    eng::sim::perform_attack(reg, killer, rng);  // fells the foe -> bonds nearby witnesses
    return eng::sim::affinity_toward(reg, ally, killer);
  };
  REQUIRE(witness_affinity(30.0f) > 0);    // a nearby ally bonds to the killer...
  REQUIRE(witness_affinity(500.0f) == 0);  // ...one far from the skirmish does not
}

TEST_CASE("a charismatic champion inspires more devotion: Charisma deepens a witnessed bond",
          "[sim]") {
  // The first Charisma EFFECT: bond_witnesses scales the camaraderie a witness feels by the
  // KILLER's Charisma, so a charismatic champion earns a deeper bond per shared victory than a
  // plain fighter. CHA 1 is x1 (the bit-identical floor); CHA 11 hits the x2 devotion cap. Only the
  // killer's CHA differs between the two runs, so the bond gap is Charisma alone.
  const auto witness_affinity = [](int charisma_level) {
    entt::registry reg;
    const entt::entity killer = reg.create();
    reg.emplace<eng::sim::Transform>(killer, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(killer).charisma.level = charisma_level;
    reg.emplace<eng::sim::Skills>(killer);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{10.0f, 0.0f});  // within melee reach
    reg.emplace<eng::sim::Stats>(foe,
                                 eng::sim::Vital{3.0f, 3.0f, 0.0f});  // frail: one blow fells it
    reg.emplace<eng::sim::Enemy>(foe);                                // ...and hostile
    const entt::entity ally = reg.create();
    reg.emplace<eng::sim::Transform>(ally,
                                     eng::Vec2{30.0f, 0.0f});  // inside the 120 camaraderie range
    reg.emplace<eng::sim::Npc>(ally);                          // a watching colonist

    std::mt19937 rng{42};
    eng::sim::perform_attack(reg, killer, rng);  // fells the foe -> bonds the watching ally
    return eng::sim::affinity_toward(reg, ally, killer);
  };
  const std::int8_t plain = witness_affinity(1);         // CHA 1 -> the base camaraderie (x1)
  const std::int8_t charismatic = witness_affinity(11);  // CHA 11 -> the x2 devotion cap
  REQUIRE(plain > 0);  // a plain fighter still bonds its witness...
  REQUIRE(charismatic >
          plain);  // ...but a charismatic one bonds it harder, from the very same kill
}

TEST_CASE("leading trains Leadership into Charisma: a witnessed kill builds a following", "[sim]") {
  // The Charisma STRAND, closing the loop: felling a foe with an ally WATCHING trains Leadership ->
  // Charisma (the social mirror of Striking -> Strength). A LONE kill, with no one to lead, trains
  // no Charisma at all — leadership is inspiring others, not merely killing.
  const auto charisma_after_kills = [](bool witnessed) {
    entt::registry reg;
    const entt::entity killer = reg.create();
    reg.emplace<eng::sim::Transform>(killer, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(killer);
    reg.emplace<eng::sim::Skills>(killer);
    reg.emplace<eng::sim::Stats>(killer);     // full health -> no berserk; and advance_progression
    reg.emplace<eng::sim::Velocity>(killer);  // ...needs Stats+Velocity+CharacterLevel to level it
    reg.emplace<eng::sim::CharacterLevel>(killer);
    if (witnessed) {
      const entt::entity ally = reg.create();
      reg.emplace<eng::sim::Transform>(ally, eng::Vec2{30.0f, 0.0f});
      reg.emplace<eng::sim::Npc>(ally);  // a standing witness to every kill
    }
    std::mt19937 rng{42};
    // A dozen kills: Leadership grants 10 XP each, and Charisma needs 100 to reach level 2, so a
    // witnessed run clears the bar. Spawn -> fell -> reap -> level, one fresh foe at a time (only
    // an alive->dead TRANSITION fires bond_witnesses, so the corpse must be cleared before the
    // next).
    for (int i = 0; i < 12; ++i) {
      const entt::entity foe = reg.create();
      reg.emplace<eng::sim::Transform>(foe, eng::Vec2{10.0f, 0.0f});
      reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{3.0f, 3.0f, 0.0f});
      reg.emplace<eng::sim::Enemy>(foe);
      eng::sim::perform_attack(reg, killer, rng);  // fell it (bonds+trains if witnessed)
      eng::sim::handle_deaths(reg, eng::Vec2{0.0f, 0.0f}, 1.0f);  // reap the corpse
      eng::sim::advance_progression(reg);                         // turn accrued XP into levels
    }
    return reg.get<eng::sim::Attributes>(killer).charisma.level;
  };
  REQUIRE(charisma_after_kills(true) > 1);    // led a dozen victories -> Charisma grew...
  REQUIRE(charisma_after_kills(false) == 1);  // ...but killing alone builds no following
}

TEST_CASE("a resented player is abandoned: a grudge-holder won't steer to the rescue", "[sim]") {
  // The abandonment reader in steer_npcs: a colonist that dislikes the downed player (a grudge past
  // the threshold) won't cross the field to save it, where a neutral one would.
  const auto rescuer_velocity_x = [](bool holds_grudge) {
    entt::registry reg;
    const entt::entity downed = reg.create();
    reg.emplace<eng::sim::Transform>(downed, eng::Vec2{100.0f, 0.0f});
    reg.emplace<eng::sim::PlayerControlled>(downed);
    reg.emplace<eng::sim::Stats>(downed).health.current = 0.0f;
    reg.emplace<eng::sim::Downed>(downed);
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});  // 100 away, within rescue radius
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);
    if (holds_grudge) eng::sim::nudge_affinity(reg, npc, downed, -30);  // resents the downed player

    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(npc).value.x;  // > 0 = steering toward the downed at +x
  };
  REQUIRE(rescuer_velocity_x(false) > 0.0f);          // neutral -> crosses to rescue (+x)
  REQUIRE(rescuer_velocity_x(true) == Approx(0.0f));  // grudge -> stays put, abandons the resented
}

TEST_CASE("a resentful ally in reach still won't haul you up", "[sim]") {
  // The completing half in handle_deaths: even a bystander in revive range refuses to rescue
  // someone it resents, so a grudge-holder leaves the player Downed where a neutral ally would
  // revive.
  const auto revived = [](bool holds_grudge) {
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{100.0f, 100.0f});
    reg.emplace<eng::sim::PlayerControlled>(player);
    reg.emplace<eng::sim::Velocity>(player);
    reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // down
    const entt::entity ally = reg.create();
    reg.emplace<eng::sim::Transform>(ally, eng::Vec2{110.0f, 100.0f});  // within revive reach
    reg.emplace<eng::sim::Velocity>(ally);
    reg.emplace<eng::sim::Stats>(ally);
    reg.emplace<eng::sim::Npc>(ally);
    if (holds_grudge) eng::sim::nudge_affinity(reg, ally, player, -30);

    const eng::Vec2 centre{640.0f, 360.0f};
    eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // player goes Downed
    eng::sim::handle_deaths(reg, centre,
                            1.0f / 60.0f);         // ally in reach -> revive, unless it resents
    return !reg.all_of<eng::sim::Downed>(player);  // true = hauled up
  };
  REQUIRE(revived(false));       // a neutral ally revives...
  REQUIRE_FALSE(revived(true));  // ...but a grudge-holder leaves you Downed
}

TEST_CASE("friendship grades the rescue reach: a bond extends it and a mild dislike shortens it",
          "[sim]") {
  // The graded positive mirror of the grudge cutoff (a hard refusal at/below kGrudgeThreshold).
  // ABOVE that line, affinity scales how far a colonist will cross to save the fallen: a bonded
  // ally is worth a longer trek, a mild dislike a shorter one. Isolated from the idle bond-pull
  // rung by placing the fallen beyond kBondRadius (the bond case) or using negative affinity below
  // kBondPull (the dislike case), so ONLY the rescue rung can move the NPC here.
  const auto rescuer_velocity_x = [](std::int8_t affinity, float fallen_x) {
    entt::registry reg;
    const entt::entity downed = reg.create();
    reg.emplace<eng::sim::Transform>(downed, eng::Vec2{fallen_x, 0.0f});
    reg.emplace<eng::sim::PlayerControlled>(downed);
    reg.emplace<eng::sim::Stats>(downed).health.current = 0.0f;
    reg.emplace<eng::sim::Downed>(downed);
    const entt::entity npc = reg.create();
    reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(npc);
    reg.emplace<eng::sim::Npc>(npc);
    if (affinity != 0) eng::sim::nudge_affinity(reg, npc, downed, affinity);
    eng::sim::steer_npcs(reg);
    return reg.get<eng::sim::Velocity>(npc).value.x;  // > 0 = steering toward the fallen at +x
  };
  // Bond EXTENDS the reach: the fallen lies at 400 — beyond the base rescue radius (300) AND beyond
  // kBondRadius (220, so the idle bond-pull can't reach it either). A neutral colonist won't cross;
  // a bonded one (+80, ~four past saves) reaches through the extended trek.
  REQUIRE(rescuer_velocity_x(0, 400.0f) == Approx(0.0f));  // neutral: 400 > 300, stays put
  REQUIRE(rescuer_velocity_x(80, 400.0f) > 0.0f);          // bonded: reach extends, crosses to save
  // Mild dislike SHORTENS the reach: the fallen lies at 285 — just inside the base radius. A
  // neutral colonist crosses; a mildly-disliked one (-18, still ABOVE the -20 grudge line) has its
  // reach pulled in past the fallen, so it drops the rescue where the hard grudge gate hasn't yet
  // fired.
  REQUIRE(rescuer_velocity_x(0, 285.0f) > 0.0f);  // neutral: 285 < 300, crosses
  REQUIRE(rescuer_velocity_x(-18, 285.0f) ==
          Approx(0.0f));  // mild dislike: reach shrinks, abandons
}

TEST_CASE("no rescue means no ledger: the deed path stays lazy", "[sim]") {
  // The absent-ledger path must be bit-identical to before morality existed: an entity that never
  // completes a deed never gets a BehaviorLedger. Neither a far-off bystander nor the unrescued
  // timer-expiry respawn records anything.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{100.0f, 100.0f});
  reg.emplace<eng::sim::PlayerControlled>(player);
  reg.emplace<eng::sim::Velocity>(player);
  reg.emplace<eng::sim::Stats>(player).health.current = 0.0f;  // down
  const entt::entity bystander = reg.create();
  reg.emplace<eng::sim::Transform>(bystander, eng::Vec2{900.0f, 900.0f});  // far out of reach
  reg.emplace<eng::sim::Npc>(bystander);
  reg.emplace<eng::sim::Stats>(bystander);

  const eng::Vec2 centre{640.0f, 360.0f};
  eng::sim::handle_deaths(reg, centre, 1.0f / 60.0f);  // goes Downed (no ally near)
  eng::sim::handle_deaths(reg, centre, 6.0f);          // timer expires -> respawn, still no rescue

  REQUIRE_FALSE(reg.all_of<eng::sim::Downed>(player));                   // respawned...
  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(player) == nullptr);     // ...but recorded no deed
  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(bystander) == nullptr);  // bystander untouched
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

TEST_CASE("a bonded survivor grieves a fallen friend: permadeath shakes the living", "[sim]") {
  // The permadeath pillar reaching past the one who died. When a colonist a survivor was truly
  // bonded to (Friend or above) is slain, the survivor's BRAVERY drifts down a grief step — the
  // negative mirror of a Valor deed's bravery-up. A mere acquaintance is no such loss, so only a
  // real friend-bond grieves; that gate is what keeps the pre-bond world bit-identical.
  entt::registry reg;

  // The fallen: an NPC already at 0 health, so handle_deaths reaps it this call (permadeath). No
  // Transform needed — only slain CREATURES need a position (for their loot drop); a dead NPC just
  // goes.
  const entt::entity fallen = reg.create();
  reg.emplace<eng::sim::Stats>(fallen, eng::sim::Vital{0.0f, 100.0f, 0.0f});  // dead on arrival
  reg.emplace<eng::sim::Npc>(fallen);

  // The mourner: a LIVING colonist (full health, so it can grieve) with a real FRIEND bond toward
  // the fallen (affinity 60 >= the kBondFriendAt = 40 floor). Bravery starts at +20 so the drop is
  // unambiguous and clear of the clamp; compassion is set too, to prove ONLY bravery moves.
  const entt::entity mourner = reg.create();
  reg.emplace<eng::sim::Stats>(mourner);
  reg.emplace<eng::sim::Npc>(mourner);
  eng::sim::Personality& mp = reg.emplace<eng::sim::Personality>(mourner);
  mp.bravery = 20;
  mp.compassion = 20;
  reg.emplace<eng::sim::Relationships>(mourner).edges.push_back(eng::sim::Relation{fallen, 60});

  // The bystander: also living, but only an ACQUAINTANCE of the fallen (affinity 20, below the
  // friend floor) — a weak tie is no bereavement, so its nerve must be untouched.
  const entt::entity bystander = reg.create();
  reg.emplace<eng::sim::Stats>(bystander);
  reg.emplace<eng::sim::Npc>(bystander);
  reg.emplace<eng::sim::Personality>(bystander).bravery = 20;
  reg.emplace<eng::sim::Relationships>(bystander).edges.push_back(eng::sim::Relation{fallen, 20});

  eng::sim::handle_deaths(reg, eng::Vec2{0.0f, 0.0f}, 1.0f);

  REQUIRE_FALSE(reg.valid(fallen));  // permadeath still reaps the fallen NPC...
  // ...the bonded mourner's bravery slipped exactly one grief step (kGriefDrift = -kDeedDriftStep =
  // -2): 20 -> 18. Only bravery moved; the compassion axis is untouched.
  REQUIRE(reg.get<eng::sim::Personality>(mourner).bravery == 18);
  REQUIRE(reg.get<eng::sim::Personality>(mourner).compassion == 20);
  // ...and the mere acquaintance did NOT grieve — its nerve holds at 20 (the bit-identity gate).
  REQUIRE(reg.get<eng::sim::Personality>(bystander).bravery == 20);
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

TEST_CASE("a thrown attack chips a distant creature and spends stamina", "[sim]") {
  // The player's RANGED option: hurl at a hostile far beyond melee reach (~45 units). It chips the
  // creature at range, costs stamina (so it can't be kited forever), and trains Throwing ->
  // Dexterity — the aim-led mirror of a swing's Striking -> Strength.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk);
  const float stamina_before = reg.get<eng::sim::Stats>(atk).stamina.current;

  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(
      foe, eng::Vec2{200.0f, 0.0f});  // way past melee reach, in throw range
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe);
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::perform_throw(reg, atk);
  eng::sim::advance_projectiles(reg, 1.0f);  // fly the launched shot home (one big step)

  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current < 40.0f);  // the throw connected at range...
  REQUIRE(reg.get<eng::sim::Stats>(atk).stamina.current < stamina_before);  // ...and cost stamina
  REQUIRE(reg.get<eng::sim::Skills>(atk).find(eng::sim::SkillId::Throwing) != nullptr);
  REQUIRE(reg.get<eng::sim::Attributes>(atk).dexterity.xp >
          eng::Fixed{});  // trained Throwing -> DEX
}

TEST_CASE("a throw beyond its range whiffs: no damage, no stamina spent", "[sim]") {
  // The range gate. A creature past the throw range is a held throw — nothing thrown, so no stamina
  // is spent (the cost is only paid on a connecting hurl).
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{400.0f, 0.0f});  // beyond the 350 throw range
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe);
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::perform_throw(reg, atk);

  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current == 40.0f);  // untouched — out of range
  REQUIRE(reg.get<eng::sim::Stats>(atk).stamina.current ==
          Approx(100.0f));  // a held throw costs nothing
}

TEST_CASE("an exhausted thrower fizzles: a throw needs stamina in hand", "[sim]") {
  // The stamina gate on ranged. With a creature in range but the bar nearly empty, the throw
  // fizzles: the foe is untouched and no stamina is spent, so an emptied bar cuts you off from
  // ranged until you recover (bursting or kiting is what empties it).
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk).stamina.current = 5.0f;  // below the ~15 throw cost
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{100.0f, 0.0f});  // well within range
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe);
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::perform_throw(reg, atk);

  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current == 40.0f);  // no throw -> foe unharmed
  REQUIRE(reg.get<eng::sim::Stats>(atk).stamina.current == Approx(5.0f));  // stamina untouched
}

TEST_CASE("a throw only targets creatures: a peaceful colonist is never hit", "[sim]") {
  // The Enemy-only filter that sets a throw apart from a melee swing: unlike perform_attack's
  // cruel-strike branch, perform_throw never targets an Npc, so a colonist standing in range is
  // ignored entirely — villainy stays a deliberate MELEE choice. With only a colonist near, the
  // throw is a pure no-op: colonist unharmed, no stamina spent, no deed recorded.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk);
  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{100.0f, 0.0f});  // in range, but peaceful
  reg.emplace<eng::sim::Stats>(colonist, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);

  eng::sim::perform_throw(reg, atk);

  REQUIRE(reg.get<eng::sim::Stats>(colonist).health.current == 40.0f);       // colonist unharmed
  REQUIRE(reg.get<eng::sim::Stats>(atk).stamina.current == Approx(100.0f));  // no target -> no cost
  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(atk) == nullptr);            // and no Cruelty deed
}

TEST_CASE("a killing throw earns Valor, exactly like a melee kill", "[sim]") {
  // A ranged kill is still a kill: the same alive->dead Valor credit as a melee killing blow, so
  // the morality ledger doesn't care which hand felled the foe.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk).dexterity.level = 20;  // throws hard enough to one-shot
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{100.0f, 0.0f});        // in range
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{2.0f, 2.0f, 0.0f});  // frail
  reg.emplace<eng::sim::Attributes>(foe).endurance.level = 1;            // low VIT
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::perform_throw(reg, atk);
  eng::sim::advance_projectiles(reg, 1.0f);  // the shot flies home and fells the frail foe

  const eng::sim::BehaviorLedger* led = reg.try_get<eng::sim::BehaviorLedger>(atk);
  REQUIRE(led != nullptr);
  REQUIRE(eng::sim::standing(*led) == 5);  // a ranged kill is Valor ×5, same as a melee kill
}

TEST_CASE("a ranged kill also forges camaraderie centred on the shooter", "[sim]") {
  // The ranged half of bond_witnesses: a killing THROW bonds colonists near the SHOOTER (who
  // fought), NOT the distant impact. The foe sits far off (x=300) so the shooter's and impact's
  // camaraderie circles don't overlap — an ally by the shooter bonds; one by the impact does not.
  const auto witness_affinity = [](float ally_x) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk).dexterity.level = 20;  // throws hard enough to one-shot
    reg.emplace<eng::sim::Skills>(atk);
    reg.emplace<eng::sim::Stats>(atk);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{300.0f, 0.0f});  // far, but in throw range
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{2.0f, 2.0f, 0.0f});  // frail: one shot
    reg.emplace<eng::sim::Enemy>(foe);
    const entt::entity ally = reg.create();
    reg.emplace<eng::sim::Transform>(ally, eng::Vec2{ally_x, 0.0f});
    reg.emplace<eng::sim::Npc>(ally);  // a peaceful colonist bystander (not the throw's target)

    eng::sim::perform_throw(reg, atk);
    eng::sim::advance_projectiles(reg,
                                  1.0f);  // flies home, fells the foe -> bonds nearby witnesses
    return eng::sim::affinity_toward(reg, ally, atk);
  };
  REQUIRE(witness_affinity(30.0f) > 0);  // an ally by the SHOOTER (dist 30) bonds...
  REQUIRE(witness_affinity(300.0f) ==
          0);  // ...one by the IMPACT (dist 300 from the shooter) does not
}

TEST_CASE("a thrown shot travels: it lands on the target only after it flies there", "[sim]") {
  // The projectile has a travel time — perform_throw LAUNCHES a homing shot, it doesn't hit
  // instantly. So right after the throw the foe is untouched and a shot is airborne; the hit lands
  // only once advance_projectiles flies it home.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{200.0f, 0.0f});
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe);
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::perform_throw(reg, atk);
  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current == 40.0f);  // in flight -> not hit yet
  REQUIRE(reg.storage<eng::sim::Projectile>().size() == 1u);       // ...and a shot is airborne

  eng::sim::advance_projectiles(reg, 1.0f);                       // fly it home (one big step)
  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current < 40.0f);  // now it landed...
  REQUIRE(reg.storage<eng::sim::Projectile>().size() == 0u);      // ...and the shot is spent
}

TEST_CASE("a thrown shot whose target dies mid-flight is wasted, not orphaned", "[sim]") {
  // The homing target can vanish before the shot arrives (another blow fells it first). The shot
  // then despawns cleanly rather than dereferencing a dead entity — ASan would catch a stale read.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk);
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe,
                                   eng::Vec2{300.0f, 0.0f});  // far enough to still be in flight
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe);
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::perform_throw(reg, atk);
  REQUIRE(reg.storage<eng::sim::Projectile>().size() == 1u);
  reg.destroy(foe);  // the target dies before the shot arrives

  eng::sim::advance_projectiles(reg, 1.0f / 60.0f);  // a normal step: can't home on a corpse
  REQUIRE(reg.storage<eng::sim::Projectile>().size() == 0u);  // the shot was dropped, no crash
}

namespace {
// Build a lone spitter at the origin (spit_range 250, the given spit_damage) plus one person at
// `victim_x`. Returns {spitter, victim}. Used by the creature_spit tests below.
std::pair<entt::entity, entt::entity> make_spitter_and_person(entt::registry& reg, float victim_x,
                                                              float spit_damage,
                                                              bool victim_is_npc) {
  const entt::entity spitter = reg.create();
  reg.emplace<eng::sim::Transform>(spitter, eng::Vec2{0.0f, 0.0f});
  eng::sim::Enemy& enemy = reg.emplace<eng::sim::Enemy>(spitter);
  enemy.spit_range = 250.0f;
  enemy.spit_damage = spit_damage;
  reg.emplace<eng::sim::Stats>(spitter, eng::sim::Vital{25.0f, 25.0f, 0.0f});
  const entt::entity victim = reg.create();
  reg.emplace<eng::sim::Transform>(victim, eng::Vec2{victim_x, 0.0f});
  reg.emplace<eng::sim::Stats>(victim, eng::sim::Vital{50.0f, 50.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(victim);  // default VIT
  if (victim_is_npc) reg.emplace<eng::sim::Npc>(victim);
  return {spitter, victim};
}
}  // namespace

TEST_CASE("a spitter launches a homing spit at a person in range", "[sim]") {
  // The ranged CREATURE attack, reusing the Projectile primitive: a spitter with a person inside
  // its spit_range fires a homing bolt, and advance_projectiles flies it in and chips the victim —
  // the hostile mirror of the player's throw, sharing the same projectile machinery.
  entt::registry reg;
  const auto [spitter, victim] = make_spitter_and_person(reg, 100.0f, 7.0f, false);
  (void)spitter;

  eng::sim::creature_spit(reg, 1.0f / 60.0f);
  REQUIRE(reg.storage<eng::sim::Projectile>().size() == 1u);         // a spit is airborne...
  eng::sim::advance_projectiles(reg, 1.0f);                          // ...fly it home
  REQUIRE(reg.get<eng::sim::Stats>(victim).health.current < 50.0f);  // the spit chipped the victim
}

TEST_CASE("a venom spitter's spit envenoms its target; a plain one doesn't", "[sim]") {
  // The spit carries the spitter's own poison_per_second as its payload, so a VENOM spitter's shot
  // leaves the struck person Poisoned (the ranged echo of a swarmer's bite) while a plain shot (or
  // the player's throw, which carries poison 0 through the same Projectile) just chips.
  const auto poisoned_after_spit = [](float spitter_poison) {
    entt::registry reg;
    const auto [spitter, victim] = make_spitter_and_person(reg, 100.0f, 7.0f, false);
    reg.get<eng::sim::Enemy>(spitter).poison_per_second = spitter_poison;  // venomous or not
    eng::sim::creature_spit(reg, 1.0f / 60.0f);  // launch the spit (carrying the venom)
    eng::sim::advance_projectiles(reg, 1.0f);    // fly it home and land it
    return reg.all_of<eng::sim::Poisoned>(victim);
  };
  REQUIRE(poisoned_after_spit(5.0f));        // a venom spit leaves the victim poisoned...
  REQUIRE_FALSE(poisoned_after_spit(0.0f));  // ...a plain spit (poison 0, like a throw) does not
}

TEST_CASE("a spitter with no one in range holds its fire", "[sim]") {
  // The range gate: a person past spit_range draws no shot.
  entt::registry reg;
  const auto [spitter, victim] = make_spitter_and_person(reg, 400.0f, 7.0f, false);  // out of range
  (void)spitter;
  (void)victim;

  eng::sim::creature_spit(reg, 1.0f / 60.0f);
  REQUIRE(reg.storage<eng::sim::Projectile>().size() == 0u);  // nothing in range -> no spit
}

TEST_CASE("a spitter reloads between shots: no spit while its cooldown ticks", "[sim]") {
  // The spit cooldown: after firing, spit_timer holds it off, so two back-to-back ticks yield only
  // ONE shot, not a stream.
  entt::registry reg;
  const auto [spitter, victim] = make_spitter_and_person(reg, 100.0f, 7.0f, false);
  (void)spitter;
  (void)victim;

  eng::sim::creature_spit(reg, 1.0f / 60.0f);                 // fires once
  eng::sim::creature_spit(reg, 1.0f / 60.0f);                 // still reloading -> no second shot
  REQUIRE(reg.storage<eng::sim::Projectile>().size() == 1u);  // exactly one spit, not two
}

TEST_CASE("a spitter earns no Valor for felling a colonist: creatures have no morality", "[sim]") {
  // The Valor guard in advance_projectiles: Valor is only for felling a HOSTILE, and a spit's
  // target is a PERSON, so a spitter that kills a colonist records nothing (a creature never gets a
  // ledger).
  entt::registry reg;
  const auto [spitter, colonist] =
      make_spitter_and_person(reg, 100.0f, 1000.0f, true);  // lethal spit

  eng::sim::creature_spit(reg, 1.0f / 60.0f);
  eng::sim::advance_projectiles(reg, 1.0f);  // the lethal spit lands and fells the colonist

  REQUIRE(reg.get<eng::sim::Stats>(colonist).health.current == 0.0f);  // the colonist was felled...
  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(spitter) ==
          nullptr);  // ...but the spitter earned nothing
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

TEST_CASE("need_efficiency saps a starving or parched fighter toward a floor", "[sim]") {
  // The design's "an empty Need is an escalating inefficiency debuff", as a pure function of the
  // sheet. Full (both needs topped) is 1.0 so a fed colony fights bit-identically; the WORST of
  // hunger/water governs, ramping to a floor at empty (weakened, never toothless).
  using eng::sim::need_efficiency;
  using eng::sim::Stats;
  Stats s;                                      // defaults: hunger and water both 100/100
  REQUIRE(need_efficiency(s) == Approx(1.0f));  // well-fed and watered -> full power

  s.hunger.current = 0.0f;                      // starving
  REQUIRE(need_efficiency(s) == Approx(0.5f));  // bottoms out at the floor, not zero

  s.hunger.current = 100.0f;  // fed again...
  s.water.current = 0.0f;     // ...but now parched
  REQUIRE(need_efficiency(s) ==
          Approx(0.5f));  // the same floor from the OTHER need (worst governs)

  // The penalty bites only in the last quarter: at exactly 25% of a need it is still full power...
  s.water.current = 25.0f;
  REQUIRE(need_efficiency(s) == Approx(1.0f));
  // ...and half-way into that quarter it is half-way to the floor (a linear ramp).
  s.water.current = 12.5f;  // worst frac 0.125, midway in [0, 0.25)
  REQUIRE(need_efficiency(s) == Approx(0.75f));
}

TEST_CASE("need_pallor tracks the combat debuff: a starving colonist looks as gaunt as it fights",
          "[sim]") {
  // The presentation cue is DERIVED from need_efficiency, so the sallow look can never drift from
  // the damage penalty. Well-fed -> 0 pallor (an unchanged draw); empty -> full pallor; and it is
  // exactly 2*(1 - need_efficiency) at every point in between.
  using eng::sim::need_efficiency;
  using eng::sim::need_pallor;
  using eng::sim::Stats;
  Stats s;                                  // full needs
  REQUIRE(need_pallor(s) == Approx(0.0f));  // fed -> no pallor (bit-identical draw)

  s.hunger.current = 0.0f;                  // starving
  REQUIRE(need_pallor(s) == Approx(1.0f));  // empty -> full pallor (need_efficiency's floor)

  s.hunger.current = 100.0f;
  s.water.current = 0.0f;                   // parched
  REQUIRE(need_pallor(s) == Approx(1.0f));  // the other need pales it too

  // Half-way into the last quarter: need_efficiency 0.75 -> pallor 0.5, and it stays locked to the
  // debuff formula so the look and the penalty are one number apart.
  s.water.current = 12.5f;
  REQUIRE(need_pallor(s) == Approx(0.5f));
  REQUIRE(need_pallor(s) == Approx(2.0f * (1.0f - need_efficiency(s))));
}

TEST_CASE("a starving fighter hits softer: an empty need saps melee damage", "[sim]") {
  // The survival Need debuff on the battlefield: two identical swings, the only difference the
  // attacker's belly. The starving one deals the floor fraction (half) of the fed one's blow, so
  // keeping the colony fed is now a combat concern too. The foe has VIT 1 (no defence) and a big
  // pool, so the blow reads clean and it survives one hit.
  const auto swing_damage = [](float hunger_current) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk);
    reg.emplace<eng::sim::Skills>(atk);
    reg.emplace<eng::sim::Stats>(atk).hunger.current = hunger_current;  // 100 fed, 0 starving
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});             // inside melee reach
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{200.0f, 200.0f, 0.0f});  // survives one hit
    reg.emplace<eng::sim::Attributes>(foe);  // VIT 1 -> zero defence, so the raw blow reads clean
    reg.emplace<eng::sim::Enemy>(foe);
    std::mt19937 rng{1234};  // foe Dexterity 1 -> never dodges; attacker Luck 1 -> never crits
    eng::sim::perform_attack(reg, atk, rng);
    return 200.0f - reg.get<eng::sim::Stats>(foe).health.current;
  };
  const float fed = swing_damage(100.0f);
  const float starving = swing_damage(0.0f);
  REQUIRE(fed > 0.0f);
  REQUIRE(starving < fed);                  // an empty belly weakens the blow...
  REQUIRE(starving == Approx(fed * 0.5f));  // ...to the floor fraction (kNeedFloor)
}

TEST_CASE("landing the killing blow on a hostile earns the attacker Valor", "[sim]") {
  // The morality write-point's SECOND deed (after rescue -> Charity), proving it generalises across
  // dimensions: slaying a monster is Valor. Only the FATAL blow credits — a chip that leaves the
  // foe standing records nothing — and NPCs earn it too (they share perform_attack), so a colonist
  // who fells a creature is valorous the same as the player.
  // Asserts INSIDE the lambda, while `reg` is still alive — the ledger lives in the registry's
  // storage, so a pointer to it must not outlive the local registry.
  const auto expect_valor = [](float foe_hp, bool expect_kill) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk).strength.level = 20;  // hits hard enough to one-shot
    reg.emplace<eng::sim::Skills>(atk);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});  // inside reach
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{foe_hp, foe_hp, 0.0f});
    reg.emplace<eng::sim::Attributes>(foe).endurance.level = 1;  // low VIT; DEX 1 -> never dodges
    reg.emplace<eng::sim::Enemy>(foe);

    std::mt19937 rng{1234};  // foe DEX 1 never dodges, atk LCK 1 never crits -> no RNG drawn
    eng::sim::perform_attack(reg, atk, rng);

    const eng::sim::BehaviorLedger* led = reg.try_get<eng::sim::BehaviorLedger>(atk);
    if (expect_kill) {
      REQUIRE(led != nullptr);                 // the kill was credited...
      REQUIRE(eng::sim::standing(*led) == 5);  // ...as Valor ×5
    } else {
      REQUIRE(led == nullptr);  // a mere chip records no deed at all
    }
  };

  expect_valor(2.0f, true);        // a frail foe dies to the strong swing -> Valor
  expect_valor(100000.0f, false);  // a foe that survives the swing -> no deed
}

TEST_CASE("a kill grants vigor: felling a foe heals the killer", "[sim]") {
  // Combat's direct sustain axis: the fatal blow restores a little HEALTH to the killer (a comeback
  // tool), capped at max. A full-health killer is unchanged (the heal clamps) — which is why every
  // existing kill test, fought at full HP, stays bit-identical. Only the kill transition heals, not
  // a chip.
  const auto kill_and_read_health = [](float killer_hp) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk).strength.level = 20;  // one-shots the frail foe
    reg.emplace<eng::sim::Skills>(atk);
    reg.emplace<eng::sim::Stats>(atk).health.current = killer_hp;  // wounded or full (max 100)
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{2.0f, 2.0f, 0.0f});  // frail -> dies in one
    reg.emplace<eng::sim::Attributes>(foe).endurance.level = 1;
    reg.emplace<eng::sim::Enemy>(foe);
    std::mt19937 rng{1234};  // foe DEX 1 never dodges, atk LCK 1 never crits
    eng::sim::perform_attack(reg, atk, rng);
    REQUIRE(reg.get<eng::sim::Stats>(foe).health.current == 0.0f);  // the foe fell
    return reg.get<eng::sim::Stats>(atk).health.current;
  };
  REQUIRE(kill_and_read_health(50.0f) > 50.0f);  // a wounded killer gains health...
  REQUIRE(kill_and_read_health(100.0f) ==
          Approx(100.0f));  // ...but a full one is capped (unchanged)
}

TEST_CASE("a ranged kill grants vigor too: a killing throw heals the thrower", "[sim]") {
  // Parity with the melee kill vigor — a killing THROW restores the same health to the owner, at
  // the projectile-impact kill site.
  entt::registry reg;
  const entt::entity atk = reg.create();
  reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(atk).dexterity.level = 20;  // throws hard enough to one-shot
  reg.emplace<eng::sim::Skills>(atk);
  reg.emplace<eng::sim::Stats>(atk).health.current = 50.0f;  // wounded thrower (max 100)
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{100.0f, 0.0f});        // in throw range
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{2.0f, 2.0f, 0.0f});  // frail -> dies
  reg.emplace<eng::sim::Attributes>(foe).endurance.level = 1;
  reg.emplace<eng::sim::Enemy>(foe);

  eng::sim::perform_throw(reg, atk);
  eng::sim::advance_projectiles(reg, 1.0f);  // the shot flies home and fells the frail foe
  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current == 0.0f);  // the foe fell...
  REQUIRE(reg.get<eng::sim::Stats>(atk).health.current >
          50.0f);  // ...and the kill healed the thrower
}

TEST_CASE("a badly wounded fighter hits harder: berserk mirrors a creature's enrage", "[sim]") {
  // The player/NPC-side mirror of enrage — drop below 30% of your max HP and your blows land 1.5x.
  // Two identical swings differing only in the attacker's health: the cornered one deals more. A
  // full-HP fighter is unchanged (the guard is false), which is why every existing combat test with
  // a hale attacker stays bit-identical.
  const auto swing_damage = [](float attacker_hp) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk);
    reg.emplace<eng::sim::Skills>(atk);
    reg.emplace<eng::sim::Stats>(atk).health.current =
        attacker_hp;  // hale or badly wounded (max 100)
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{1000.0f, 1000.0f, 0.0f});  // survives one hit
    reg.emplace<eng::sim::Attributes>(foe);  // VIT 1 -> zero defence -> the raw blow reads clean
    reg.emplace<eng::sim::Enemy>(foe);
    std::mt19937 rng{1234};  // foe DEX 1 never dodges, atk LCK 1 never crits
    eng::sim::perform_attack(reg, atk, rng);
    return 1000.0f - reg.get<eng::sim::Stats>(foe).health.current;
  };
  const float hale = swing_damage(100.0f);    // full HP -> no berserk
  const float wounded = swing_damage(20.0f);  // 20% of max, below the 30% line -> berserk
  REQUIRE(hale > 0.0f);
  REQUIRE(wounded > hale);                  // the cornered fighter hits harder...
  REQUIRE(wounded == Approx(hale * 1.5f));  // ...by exactly the berserk factor
}

TEST_CASE("an NPC that fells a creature via npc_attack earns Valor (parity)", "[sim]") {
  // The parity claim through the NPC combat SYSTEM (not perform_attack directly): npc_attack
  // iterates the NPC view and, when a colonist's swing kills an adjacent hostile, credits it Valor
  // the same as the player. This also drives the lazy get_or_emplace<BehaviorLedger> WHILE the NPC
  // view is being iterated — safe because the ledger is in none of that view's pools.
  entt::registry reg;
  const entt::entity npc = reg.create();
  reg.emplace<eng::sim::Transform>(npc, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(npc).strength.level = 20;  // one-shots the frail foe
  reg.emplace<eng::sim::Skills>(npc);
  reg.emplace<eng::sim::Npc>(npc);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{10.0f, 0.0f});  // inside reach
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{2.0f, 2.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe).endurance.level = 1;  // DEX 1 -> never dodges
  reg.emplace<eng::sim::Enemy>(foe);

  std::mt19937 rng{1234};
  eng::sim::npc_attack(reg, rng);

  const eng::sim::BehaviorLedger* led = reg.try_get<eng::sim::BehaviorLedger>(npc);
  REQUIRE(led != nullptr);
  REQUIRE(eng::sim::standing(*led) == 5);  // a colonist's kill is Valor, same as the player's
}

TEST_CASE("a player who strikes a peaceful colonist earns Cruelty and sinks their standing",
          "[sim]") {
  // The morality ledger's VILLAIN mirror: with nothing hostile in reach, a PLAYER swinging near a
  // colonist hits it — a deliberate act of Cruelty that both wounds the victim and drops the
  // striker's standing below zero. This is the first deed that makes standing NEGATIVE, so it also
  // proves the signed formula sinks as well as it lifts.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::PlayerControlled>(player);  // the gate: only a player turns cruel
  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{20.0f, 0.0f});  // inside reach
  reg.emplace<eng::sim::Stats>(colonist, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);

  std::mt19937 rng{1234};  // the cruel branch draws no RNG at all
  eng::sim::perform_attack(reg, player, rng);

  REQUIRE(reg.get<eng::sim::Stats>(colonist).health.current < 40.0f);  // the betrayal wounded them
  const eng::sim::BehaviorLedger* led = reg.try_get<eng::sim::BehaviorLedger>(player);
  REQUIRE(led != nullptr);
  REQUIRE(eng::sim::standing(*led) == -6);  // Cruelty ×6 -> the first negative standing
}

TEST_CASE("a hostile in reach shields a nearby colonist: no accidental Cruelty mid-combat",
          "[sim]") {
  // The safety gate that makes cruelty a CHOICE, not a slip: hostiles are always searched first and
  // win the target, so even a player standing beside a colonist strikes the CREATURE while one is
  // in reach — no cruelty is recorded, and the colonist is untouched. You can only turn on the
  // colony when there's nothing else to fight.
  entt::registry reg;
  const entt::entity player = reg.create();
  reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(player);
  reg.emplace<eng::sim::Skills>(player);
  reg.emplace<eng::sim::PlayerControlled>(player);
  const entt::entity colonist = reg.create();
  reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{15.0f, 0.0f});  // in reach
  reg.emplace<eng::sim::Stats>(colonist, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(colonist);
  reg.emplace<eng::sim::Npc>(colonist);
  const entt::entity foe = reg.create();
  reg.emplace<eng::sim::Transform>(foe, eng::Vec2{25.0f, 0.0f});  // also in reach, further off
  reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(foe);  // default DEX 1 -> dodge_chance 0, so the strike lands
  reg.emplace<eng::sim::Enemy>(foe);

  std::mt19937 rng{1234};
  eng::sim::perform_attack(reg, player, rng);

  REQUIRE(reg.get<eng::sim::Stats>(colonist).health.current == 40.0f);  // colonist unharmed
  REQUIRE(reg.get<eng::sim::Stats>(foe).health.current < 40.0f);        // the creature took the hit
  // No ledger at all: the cruel branch never ran (a hostile was in reach), and the foe survived the
  // chip, so there's no Valor either.
  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(player) == nullptr);
}

TEST_CASE("an NPC never turns cruel: only the player can strike the colony", "[sim]") {
  // The player-only gate through npc_attack: an NPC with a peaceful colonist beside it and nothing
  // hostile around swings a whiff (motes/enemies only) — it does NOT strike its neighbour, so no
  // Cruelty is ever recorded on an NPC. NPC-vs-NPC villainy is a later ring's AI, not a side effect
  // of the shared perform_attack.
  entt::registry reg;
  const entt::entity aggressor = reg.create();
  reg.emplace<eng::sim::Transform>(aggressor, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(aggressor).strength.level = 20;
  reg.emplace<eng::sim::Skills>(aggressor);
  reg.emplace<eng::sim::Npc>(aggressor);
  const entt::entity neighbour = reg.create();
  reg.emplace<eng::sim::Transform>(neighbour, eng::Vec2{10.0f, 0.0f});  // well inside reach
  reg.emplace<eng::sim::Stats>(neighbour, eng::sim::Vital{40.0f, 40.0f, 0.0f});
  reg.emplace<eng::sim::Attributes>(neighbour);
  reg.emplace<eng::sim::Npc>(neighbour);

  std::mt19937 rng{1234};
  eng::sim::npc_attack(reg, rng);

  REQUIRE(reg.get<eng::sim::Stats>(neighbour).health.current ==
          40.0f);  // untouched by its neighbour
  REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(aggressor) == nullptr);  // no NPC ever turns cruel
}

TEST_CASE("the cruel branch stays its hand: a downed colonist and an empty swing record nothing",
          "[sim]") {
  // The two do-NOTHING guards of the villain path, each deliberate branch logic that ships without
  // a visible effect of its own: (1) a DOWNED colonist is excluded from the victim search — no
  // infamy for kicking a fallen body — and (2) a player swinging at literally nothing (no hostile,
  // no colonist in reach) still just whiffs. Neither leaves a ledger.
  {
    // (1) A player, a colonist in reach that is already Downed, nothing hostile around.
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(player);
    reg.emplace<eng::sim::Skills>(player);
    reg.emplace<eng::sim::PlayerControlled>(player);
    const entt::entity colonist = reg.create();
    reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{15.0f, 0.0f});  // in reach
    reg.emplace<eng::sim::Stats>(colonist, eng::sim::Vital{40.0f, 40.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(colonist);
    reg.emplace<eng::sim::Npc>(colonist);
    reg.emplace<eng::sim::Downed>(colonist);  // excluded from the cruel search

    std::mt19937 rng{1234};
    eng::sim::perform_attack(reg, player, rng);

    REQUIRE(reg.get<eng::sim::Stats>(colonist).health.current == 40.0f);  // a corpse is spared
    REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(player) == nullptr);    // so no Cruelty
  }
  {
    // (2) A player alone — nothing hostile, no colonist in reach: a plain whiff, no deed.
    entt::registry reg;
    const entt::entity player = reg.create();
    reg.emplace<eng::sim::Transform>(player, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(player);
    reg.emplace<eng::sim::Skills>(player);
    reg.emplace<eng::sim::PlayerControlled>(player);

    std::mt19937 rng{1234};
    // Extra parens: Catch2 can't decompose `entity == entt::null` (entt's operator== is ambiguous
    // with ExprLhs), so evaluate the bool ourselves.
    REQUIRE((eng::sim::perform_attack(reg, player, rng) == entt::null));  // hand nothing back
    REQUIRE(reg.try_get<eng::sim::BehaviorLedger>(player) == nullptr);    // and record nothing
  }
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

TEST_CASE("poison_tint_strength greens a dot more with a stronger dose and then caps", "[sim]") {
  // The venom visual, a pure function the renderer uses to tint a poisoned dot green: no venom = no
  // tint, a nastier dose glows greener, but capped so the dot never fully greens out (stays
  // legible).
  REQUIRE(eng::sim::poison_tint_strength(0.0f) == 0.0f);  // no venom -> no tint
  REQUIRE(eng::sim::poison_tint_strength(5.0f) > 0.0f);   // a spit's dose -> some green...
  REQUIRE(eng::sim::poison_tint_strength(9.0f) >
          eng::sim::poison_tint_strength(5.0f));  // ...a swarmer's, greener (monotonic)
  REQUIRE(eng::sim::poison_tint_strength(1000.0f) ==
          Approx(eng::sim::kPoisonTintCap));  // a huge dose -> capped, not runaway
  REQUIRE(eng::sim::kPoisonTintCap < 1.0f);   // and the cap keeps the dot readable, never all green
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

TEST_CASE("renown_scale scales a dot by standing: heroes loom, villains shrink, both cap",
          "[sim]") {
  // Pure presentation helper: STANDING is shown as size. Renown (positive) swells a dot so you can
  // watch a colonist earn repute; infamy (negative) shrinks it to a shunned husk. Symmetric about
  // neutral, capped at both ends so nobody balloons or vanishes.
  REQUIRE(eng::sim::renown_scale(0) == 1.0f);  // neutral -> authored size EXACTLY (identity)
  REQUIRE(eng::sim::renown_scale(eng::sim::kRenownFullAt) ==
          Approx(1.0f + eng::sim::kRenownMaxScale));  // full renown -> the max bump
  REQUIRE(eng::sim::renown_scale(eng::sim::kRenownFullAt * 10) ==
          Approx(1.0f + eng::sim::kRenownMaxScale));  // beyond full -> capped, no runaway
  REQUIRE(eng::sim::renown_scale(-eng::sim::kRenownFullAt) ==
          Approx(1.0f - eng::sim::kInfamyMaxShrink));  // full infamy -> the max shrink (the floor)
  REQUIRE(eng::sim::renown_scale(-eng::sim::kRenownFullAt * 10) ==
          Approx(1.0f - eng::sim::kInfamyMaxShrink));  // beyond -> capped, never shrinks to nothing

  // Monotonic on BOTH sides: a mid renown sits strictly between neutral and the swell cap; a mid
  // infamy strictly between neutral and the shrink floor (and the floor stays well above zero).
  const float hero = eng::sim::renown_scale(eng::sim::kRenownFullAt / 2);
  REQUIRE(hero > 1.0f);
  REQUIRE(hero < 1.0f + eng::sim::kRenownMaxScale);
  const float villain = eng::sim::renown_scale(-eng::sim::kRenownFullAt / 2);
  REQUIRE(villain < 1.0f);
  REQUIRE(villain > 1.0f - eng::sim::kInfamyMaxShrink);
  REQUIRE(1.0f - eng::sim::kInfamyMaxShrink > 0.0f);  // the shrink floor never reaches nothing
}

TEST_CASE("standing_title names each band, symmetric about neutral", "[sim]") {
  // Derived recognition: a pure query over standing, always in sync with the deeds behind it. Pin
  // each band edge — neutral is "Unproven", the hero bands rise, the villain bands mirror them
  // (ready though no villain deed exists yet). The upper band reuses kRenownFullAt, so the title
  // flips to "Renowned" exactly when the renown dot-size caps.
  const auto title = [](std::int32_t s) { return std::string(eng::sim::standing_title(s)); };
  REQUIRE(title(0) == "Unproven");
  REQUIRE(title(eng::sim::kKnownAt - 1) == "Unproven");    // just shy of the first band...
  REQUIRE(title(eng::sim::kKnownAt) == "Known");           // ...and onto it
  REQUIRE(title(eng::sim::kRenownFullAt - 1) == "Known");  // still Known just below the cap...
  REQUIRE(title(eng::sim::kRenownFullAt) == "Renowned");   // ...Renowned exactly at it
  REQUIRE(title(-eng::sim::kKnownAt + 1) == "Unproven");   // just shy of Suspect...
  REQUIRE(title(-eng::sim::kKnownAt) == "Suspect");  // ...and onto it (villain side mirrors —
  REQUIRE(title(-eng::sim::kRenownFullAt + 1) ==
          "Suspect");                                       // unreachable today, but the bands are
  REQUIRE(title(-eng::sim::kRenownFullAt) == "Notorious");  // ready and symmetric)
}

TEST_CASE("bond_tier names each affinity band from nemesis to partner", "[sim]") {
  // The relationships twin of standing_title — a pure query naming where an affinity falls, always
  // in sync with the number behind it. The band edges reuse the behavioural thresholds:
  // Acquaintance begins at +10 (the kBondPull follow line), Rival at -20 (the kGrudgeThreshold
  // abandon line).
  const auto tier = [](std::int8_t a) { return std::string(eng::sim::bond_tier(a)); };
  REQUIRE(tier(100) == "Partner");
  REQUIRE(tier(80) == "Partner");
  REQUIRE(tier(79) == "Friend");
  REQUIRE(tier(40) == "Friend");
  REQUIRE(tier(10) == "Acquaintance");  // exactly the +10 kBondPull line...
  REQUIRE(tier(9) == "Neutral");        // ...just below it is Neutral
  REQUIRE(tier(0) == "Neutral");
  REQUIRE(tier(-19) == "Neutral");
  REQUIRE(tier(-20) == "Rival");  // exactly the -20 kGrudgeThreshold line...
  REQUIRE(tier(-59) == "Rival");
  REQUIRE(tier(-60) == "Nemesis");  // ...deepening to Nemesis
  REQUIRE(tier(-100) == "Nemesis");
}

TEST_CASE("build_title names the dominant trained attribute", "[sim]") {
  // The "from build" derived title, the twin of standing_title: which of the four trained
  // Attributes leads names what KIND of fighter you are. Pure query; untrained = Greenhorn; ties
  // break in a fixed order so it's deterministic.
  const auto title = [](const eng::sim::Attributes& a) {
    return std::string(eng::sim::build_title(a));
  };

  REQUIRE(title(eng::sim::Attributes{}) == "Greenhorn");  // all at level 1 -> no build yet

  eng::sim::Attributes warrior{};
  warrior.strength.level = 6;
  REQUIRE(title(warrior) == "Warrior");
  eng::sim::Attributes skirmisher{};
  skirmisher.dexterity.level = 6;
  REQUIRE(title(skirmisher) == "Skirmisher");
  eng::sim::Attributes bulwark{};
  bulwark.endurance.level = 6;
  REQUIRE(title(bulwark) == "Bulwark");
  eng::sim::Attributes chancer{};
  chancer.luck.level = 6;
  REQUIRE(title(chancer) == "Chancer");

  // A tie for the top breaks in the fixed order (strength first), so the title is deterministic.
  eng::sim::Attributes tied{};
  tied.strength.level = 6;
  tied.dexterity.level = 6;
  REQUIRE(title(tied) == "Warrior");
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

TEST_CASE("poison chips health over time and then wears off", "[sim]") {
  // The lingering half of a venomous bite: it keeps chipping health for its duration (through the
  // normal death path), then tick_poison reaps the status.
  entt::registry reg;
  const entt::entity e = reg.create();
  reg.emplace<eng::sim::Stats>(e);                                      // full health (100)
  reg.emplace<eng::sim::Poisoned>(e, eng::sim::Poisoned{2.0f, 10.0f});  // 2s of 10/s venom

  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::tick_poison(reg, dt);  // 1 second
  const float after_1s = reg.get<eng::sim::Stats>(e).health.current;
  REQUIRE(after_1s < 100.0f);                  // venom chipped health...
  REQUIRE(reg.all_of<eng::sim::Poisoned>(e));  // ...and half of a 2s dose still lingers

  for (int i = 0; i < 2 * eng::sim::kTicksPerSecond; ++i)
    eng::sim::tick_poison(reg, dt);  // 2 more s
  REQUIRE(reg.get<eng::sim::Stats>(e).health.current <
          after_1s);                                 // it chipped more before expiring...
  REQUIRE_FALSE(reg.all_of<eng::sim::Poisoned>(e));  // ...then wore off (reaped)
}

TEST_CASE("a hardy constitution resists venom: VIT shaves the poison chip", "[sim]") {
  // VIT (Endurance) now reduces poison damage directly — the DoT counterpart of how it softens a
  // blow. A tough character loses less health to the same venom than a frail one, but venom is
  // never fully negated. VIT 1 is unchanged from before (resists nothing).
  const auto health_lost = [](int endurance_level) {
    entt::registry reg;
    const entt::entity e = reg.create();
    reg.emplace<eng::sim::Stats>(e);  // full health (100)
    reg.emplace<eng::sim::Attributes>(e).endurance.level = endurance_level;
    reg.emplace<eng::sim::Poisoned>(e, eng::sim::Poisoned{5.0f, 10.0f});  // 10/s venom, lasts > 1s
    const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
    for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) eng::sim::tick_poison(reg, dt);  // 1 second
    return 100.0f - reg.get<eng::sim::Stats>(e).health.current;
  };
  const float frail = health_lost(1);   // resists nothing -> full chip
  const float hardy = health_lost(11);  // 10 levels past 1 -> 50% resist
  REQUIRE(frail > 0.0f);                // a frail body takes the full chip...
  REQUIRE(hardy < frail);               // ...a hardy one takes less...
  REQUIRE(hardy > 0.0f);                // ...but venom still bites (never fully negated)
}

TEST_CASE("a venomous creature's bite leaves the victim poisoned; a plain one doesn't", "[sim]") {
  // A landed blow from a venomous archetype (swarmers) applies Poisoned; a non-venomous one leaves
  // none. Fires for any victim — player or NPC — through the same resolve_creature_contacts
  // (parity).
  const auto poisoned_after_bite = [](float foe_poison, bool victim_is_npc) {
    entt::registry reg;
    const entt::entity victim = reg.create();
    reg.emplace<eng::sim::Transform>(victim, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(victim);
    reg.emplace<eng::sim::Attributes>(victim);  // Dexterity 1 -> never dodges, so the bite lands
    if (victim_is_npc)
      reg.emplace<eng::sim::Npc>(victim);
    else
      reg.emplace<eng::sim::PlayerControlled>(victim);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});  // in contact (distance 0)
    reg.emplace<eng::sim::Enemy>(foe).poison_per_second = foe_poison;

    std::mt19937 rng{1234};
    eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);
    return reg.all_of<eng::sim::Poisoned>(victim);
  };
  REQUIRE(poisoned_after_bite(9.0f, false));        // a venomous bite poisons the player...
  REQUIRE(poisoned_after_bite(9.0f, true));         // ...and an NPC identically (parity)...
  REQUIRE_FALSE(poisoned_after_bite(0.0f, false));  // ...a non-venomous bite leaves no venom
}

TEST_CASE("poison suppresses healing, so venom nets health strictly down", "[sim]") {
  // Venom gates regen (regenerate_vitals skips a poisoned entity), so even a high-regen character
  // LOSES health while poisoned instead of out-healing the chip — the same gate starvation uses,
  // and the reason the lingering bite is a real threat rather than cancelled by a fed character's
  // regen.
  entt::registry reg;
  const entt::entity e = reg.create();
  auto& s = reg.emplace<eng::sim::Stats>(e);
  s.health = eng::sim::Vital{50.0f, 100.0f, 8.0f};  // wounded, heals 8/s base...
  reg.emplace<eng::sim::Attributes>(e).endurance.level =
      8;  // ...boosted to 13.6/s, ABOVE the venom
  reg.emplace<eng::sim::Poisoned>(e,
                                  eng::sim::Poisoned{2.0f, 9.0f});  // 9/s venom (< the 13.6 regen)

  const float before = s.health.current;
  const float dt = static_cast<float>(eng::sim::kSecondsPerTick);
  for (int i = 0; i < eng::sim::kTicksPerSecond; ++i) {  // one second, the real tick order
    eng::sim::tick_poison(reg, dt);                      // venom chips...
    eng::sim::regenerate_vitals(reg,
                                dt);  // ...and healing is gated off while poisoned, no clawback
  }
  REQUIRE(reg.get<eng::sim::Stats>(e).health.current < before);  // strictly down despite high regen
}

TEST_CASE("a worn-down creature enrages and hits harder", "[sim]") {
  // Below the enrage threshold of its OWN HP, a creature's blows hit harder — so leaving a foe
  // half-dead is dangerous, and finishing it fast is the safe play. Compare one blow from a full-HP
  // vs a wounded creature of the same archetype.
  const auto hit_damage = [](float creature_hp_fraction) {
    entt::registry reg;
    const entt::entity victim = reg.create();
    reg.emplace<eng::sim::Transform>(victim, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(victim);  // no Attributes -> DEX 1 (never dodges), defence 0
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});  // in contact
    reg.emplace<eng::sim::Enemy>(foe);                             // default attack_damage
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{100.0f * creature_hp_fraction, 100.0f, 0.0f});

    std::mt19937 rng{1234};
    const float before = reg.get<eng::sim::Stats>(victim).health.current;
    eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);
    return before - reg.get<eng::sim::Stats>(victim).health.current;
  };
  const float healthy_hit = hit_damage(1.0f);  // full HP -> its normal blow
  const float wounded_hit = hit_damage(0.2f);  // below the enrage threshold -> harder
  REQUIRE(healthy_hit > 0.0f);                 // it connected...
  REQUIRE(wounded_hit > healthy_hit);          // ...and the cornered beast hit harder
}

TEST_CASE("a finishing blow hits a worn-down creature harder: execute, the mirror of enrage",
          "[sim]") {
  // The offensive twin of enrage: a creature already below the same worn-down fraction of its HP
  // takes MORE from the next blow, so a half-dead foe both rages and folds fast. Two identical
  // foes, the same swing (LCK/DEX 1 -> no crit, no dodge, no RNG); the ONLY difference is current
  // HP, so any damage gap is the execute bonus alone.
  const auto damage_dealt = [](float current_hp) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk);
    reg.emplace<eng::sim::Skills>(atk);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});                 // inside reach
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{current_hp, 100.0f, 0.0f});  // max 100
    reg.emplace<eng::sim::Attributes>(
        foe);  // default DEX 1 -> dodge_chance 0, so every swing lands
    reg.emplace<eng::sim::Enemy>(foe);

    std::mt19937 rng{1234};
    eng::sim::perform_attack(reg, atk, rng);
    return current_hp - reg.get<eng::sim::Stats>(foe).health.current;
  };
  const float healthy = damage_dealt(100.0f);  // full HP (100%) -> no execute
  const float wounded = damage_dealt(20.0f);   // 20% (< 30% threshold) -> execute bonus
  const float on_edge = damage_dealt(35.0f);   // 35% (> 30%) -> still no execute
  REQUIRE(healthy > 0.0f);                     // the swing landed...
  REQUIRE(wounded > healthy);                  // ...and the worn-down foe took more...
  REQUIRE(on_edge == Approx(healthy));         // ...but only once it's past the threshold
}

TEST_CASE("a melee hit cleaves a second creature when they cluster", "[sim]") {
  // Anti-swarm melee: the swing that strikes the nearest creature ALSO chips the nearest OTHER
  // creature within a swing's width of it — one hit, two foes when they cluster. A second foe far
  // from the struck one takes no cleave. Two identical foes; the primary at 20 is the aimed target.
  const auto second_hp_lost = [](float second_x) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk);
    reg.emplace<eng::sim::Skills>(atk);
    const entt::entity primary = reg.create();
    reg.emplace<eng::sim::Transform>(primary,
                                     eng::Vec2{20.0f, 0.0f});  // the aimed target (in reach)
    reg.emplace<eng::sim::Stats>(primary, eng::sim::Vital{100.0f, 100.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(primary);  // DEX 1 -> never dodges, so the hit lands
    reg.emplace<eng::sim::Enemy>(primary);
    const entt::entity second = reg.create();
    reg.emplace<eng::sim::Transform>(second, eng::Vec2{second_x, 0.0f});
    reg.emplace<eng::sim::Stats>(second, eng::sim::Vital{100.0f, 100.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(second);
    reg.emplace<eng::sim::Enemy>(second);

    std::mt19937 rng{1234};
    eng::sim::perform_attack(reg, atk, rng);
    return 100.0f - reg.get<eng::sim::Stats>(second).health.current;
  };
  REQUIRE(second_hp_lost(50.0f) > 0.0f);    // 30 from the struck foe (< the ~40 width) -> cleaved
  REQUIRE(second_hp_lost(400.0f) == 0.0f);  // 380 away -> well outside the swing, untouched
}

TEST_CASE("a raised guard softens a creature's blow", "[sim]") {
  // A Blocking victim takes less damage — the reward that pays for the mobility a guard costs.
  const auto hit_damage = [](bool guarding) {
    entt::registry reg;
    const entt::entity victim = reg.create();
    reg.emplace<eng::sim::Transform>(victim, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(victim);
    if (guarding) reg.emplace<eng::sim::Blocking>(victim);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});  // in contact
    reg.emplace<eng::sim::Enemy>(foe);
    reg.emplace<eng::sim::Stats>(foe,
                                 eng::sim::Vital{100.0f, 100.0f, 0.0f});  // full HP -> not enraged

    std::mt19937 rng{1234};
    const float before = reg.get<eng::sim::Stats>(victim).health.current;
    eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);
    return before - reg.get<eng::sim::Stats>(victim).health.current;
  };
  REQUIRE(hit_damage(false) > 0.0f);              // an open stance takes the full blow...
  REQUIRE(hit_damage(true) < hit_damage(false));  // ...a raised guard softens it
}

TEST_CASE("a raised guard ripostes only while it has the stamina to spend", "[sim]") {
  // The offence half of the block: a guarding defender bites back, so planting your guard wears the
  // attacker down as well as softening its blows. But a riposte is an EXERTION — a WINDED guard
  // softens but can't turn a blow. Same contact setup as the softening test; here we watch the FOE.
  const auto foe_damage_taken = [](bool guarding, float victim_stamina) {
    entt::registry reg;
    const entt::entity victim = reg.create();
    reg.emplace<eng::sim::Transform>(victim, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(victim).stamina.current = victim_stamina;
    if (guarding) reg.emplace<eng::sim::Blocking>(victim);
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{0.0f, 0.0f});  // in contact
    reg.emplace<eng::sim::Enemy>(foe);
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{100.0f, 100.0f, 0.0f});  // full HP

    std::mt19937 rng{1234};
    const float before = reg.get<eng::sim::Stats>(foe).health.current;
    eng::sim::resolve_creature_contacts(reg, 1.0f / 60.0f, rng);
    return before - reg.get<eng::sim::Stats>(foe).health.current;
  };
  REQUIRE(foe_damage_taken(false, 100.0f) == 0.0f);  // an open stance leaves the attacker untouched
  REQUIRE(foe_damage_taken(true, 100.0f) > 0.0f);  // a fresh guard ripostes a chip back into it...
  REQUIRE(foe_damage_taken(true, 0.0f) == 0.0f);   // ...but a winded guard can only soften
}

TEST_CASE("a raised guard gives no second wind: no stamina recovery while blocking", "[sim]") {
  // Bracing to turn blows is exertion, not rest — a still, guarding character doesn't recover
  // stamina (the twin of the starvation gate), so a prolonged guard bleeds what its ripostes spend
  // and can't be held risk-free forever. A non-guarding still character recovers as normal.
  const auto rested_stamina = [](bool guarding) {
    entt::registry reg;
    const entt::entity e = reg.create();
    reg.emplace<eng::sim::Stats>(e).stamina = eng::sim::Vital{50.0f, 100.0f, 20.0f};
    reg.emplace<eng::sim::Velocity>(e);  // zero velocity -> still (would recover, if not guarding)
    if (guarding) reg.emplace<eng::sim::Blocking>(e);
    for (int i = 0; i < 30; ++i) eng::sim::update_stamina(reg, 1.0f / 60.0f);
    return reg.get<eng::sim::Stats>(e).stamina.current;
  };
  REQUIRE(rested_stamina(false) > 50.0f);          // open stance, still -> recovers
  REQUIRE(rested_stamina(true) == Approx(50.0f));  // guarding -> no second wind, stays put
}

TEST_CASE("guarding slows the player's movement: the block's trade-off", "[sim]") {
  // The cost that keeps a guard from being free upside: a guarding player crawls. Driven through
  // the real MovePlayer command's `guard` flag, so the whole funnel is exercised.
  const auto move_speed_x = [](bool guarding) {
    eng::sim::World world;
    const entt::entity player = world.player();
    world.submit(eng::sim::move_player(eng::sim::kLocalPlayer, eng::Vec2{1.0f, 0.0f}, guarding));
    world.step();
    return world.registry().get<eng::sim::Velocity>(player).value.x;
  };
  const float open = move_speed_x(false);
  const float guarded = move_speed_x(true);
  REQUIRE(open > 0.0f);
  REQUIRE(guarded > 0.0f);  // still moving — a crawl, not rooted...
  REQUIRE(guarded < open);  // ...but slower than moving with an open stance
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

TEST_CASE("reinforcement colonists roll varied archetypes, not just bravery", "[sim]") {
  // Before archetypes, reinforcements varied ONLY bravery — greed/compassion/industry defaulted to
  // 0 — so the colony drifted toward neutral as the hand-authored opening four died. Now each rolls
  // a coherent archetype + jitter, so the ongoing population stays varied on all four wired axes.
  eng::sim::World world;
  entt::registry& reg = world.registry();

  // Wipe the opening four (their spread is hand-authored) so ONLY reinforcements remain to inspect.
  std::vector<entt::entity> openers;
  for (const entt::entity e : reg.view<eng::sim::Npc>()) openers.push_back(e);
  for (const entt::entity e : openers) reg.destroy(e);

  // Refill: clear threats each tick so nothing culls the recovering colony (as the cap test does).
  for (int i = 0; i < 90 * eng::sim::kTicksPerSecond; ++i) {
    std::vector<entt::entity> threats;
    for (const entt::entity e : reg.view<eng::sim::Enemy>()) threats.push_back(e);
    for (const entt::entity e : reg.view<eng::sim::Hazard>()) threats.push_back(e);
    for (const entt::entity e : threats) reg.destroy(e);
    world.step();
  }
  REQUIRE(static_cast<int>(reg.storage<eng::sim::Npc>().size()) == eng::sim::kMaxNpcs);

  // The refilled colonists vary on the axes that used to be flat zero — proof they roll archetypes,
  // not the old bravery-only roll. (Would fail before: greed/compassion/industry were all 0.)
  bool any_greed = false, any_compassion = false, any_industry = false;
  for (const entt::entity e : reg.view<eng::sim::Npc, eng::sim::Personality>()) {
    const eng::sim::Personality& p = reg.get<eng::sim::Personality>(e);
    if (p.greed != 0) any_greed = true;
    if (p.compassion != 0) any_compassion = true;
    if (p.industry != 0) any_industry = true;
  }
  REQUIRE(any_greed);       // greed no longer defaults to 0...
  REQUIRE(any_compassion);  // ...nor compassion...
  REQUIRE(any_industry);    // ...nor industry — the archetype roll fills all four axes
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

TEST_CASE("a weapon wears with use and shatters: durability reverts the wielder to unarmed",
          "[sim]") {
  // The design's "durability now, repair later" — a connecting hit on a hostile dulls the blade by
  // one, and at 0 it SHATTERS: the weapon slot clears (strength, heft, venom). A
  // near-indestructible foe survives the swings, so we watch the blade break rather than the foe.
  const auto make_foe = [](entt::registry& reg) {
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});                 // in reach
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{10000.0f, 10000.0f, 0.0f});  // survives it
    reg.emplace<eng::sim::Attributes>(foe).endurance.level = 1;
    reg.emplace<eng::sim::Enemy>(foe);
    return foe;
  };

  SECTION("a weapon-only wielder shatters to bare hands: the empty Equipped is removed") {
    // With no armour, the shatter drops the now-empty cache entirely (mirroring the Drop command),
    // so "unarmed" reads as gear == nullptr — which is what lets a shattered NPC re-seek and
    // re-grab.
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk);
    reg.emplace<eng::sim::Skills>(atk);
    eng::sim::Equipped eq{};
    eq.strength_bonus = 4;
    eq.move_penalty = 0.25f;
    eq.weapon_durability = 2.0f;  // a nearly-spent blade: two connecting hits left
    reg.emplace<eng::sim::Equipped>(atk, eq);
    make_foe(reg);
    std::mt19937 rng{1234};  // foe DEX 1 never dodges, atk LCK 1 never crits

    eng::sim::perform_attack(reg, atk, rng);  // first hit: wears 2 -> 1, still armed
    REQUIRE(reg.get<eng::sim::Equipped>(atk).weapon_durability == Approx(1.0f));
    REQUIRE(reg.get<eng::sim::Equipped>(atk).strength_bonus == 4);

    eng::sim::perform_attack(reg, atk, rng);             // second hit: wears 1 -> 0 -> SHATTERS
    REQUIRE_FALSE(reg.all_of<eng::sim::Equipped>(atk));  // the empty cache is gone -> truly unarmed
  }

  SECTION("a weapon shattering with armour worn keeps the Equipped: only the weapon slot clears") {
    // The two slots are independent: a shattered blade must not strip the plate you are also
    // wearing.
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk);
    reg.emplace<eng::sim::Skills>(atk);
    eng::sim::Equipped eq{};
    eq.strength_bonus = 4;
    eq.move_penalty = 0.25f;
    eq.weapon_durability = 1.0f;  // one hit left...
    eq.defence_bonus = 6.0f;      // ...but armour is also worn
    eq.stamina_regen_penalty = 0.3f;
    reg.emplace<eng::sim::Equipped>(atk, eq);
    make_foe(reg);
    std::mt19937 rng{1234};

    eng::sim::perform_attack(reg, atk, rng);  // the one hit shatters the blade
    const eng::sim::Equipped& after = reg.get<eng::sim::Equipped>(atk);  // still present (armour)
    REQUIRE(after.strength_bonus == 0);                                  // weapon slot cleared...
    REQUIRE(after.move_penalty == Approx(0.0f));
    REQUIRE(after.defence_bonus == Approx(6.0f));  // ...but the armour slot is untouched
  }
}

TEST_CASE("a venom weapon's hit envenoms the struck creature", "[sim]") {
  // The player-side venom PROC: wielding a venom blade (Equipped.weapon_venom > 0) applies Poisoned
  // to a struck enemy — the mirror of a swarmer's bite, reusing the same status. A plain or
  // bare-handed swing does not. (Returns a VALUE, never a pointer into the local registry.)
  const auto venom_dps_after_hit = [](float weapon_venom) {
    entt::registry reg;
    const entt::entity atk = reg.create();
    reg.emplace<eng::sim::Transform>(atk, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Attributes>(atk);
    reg.emplace<eng::sim::Skills>(atk);
    if (weapon_venom > 0.0f) {
      reg.emplace<eng::sim::Equipped>(atk).weapon_venom = weapon_venom;  // a venom blade wielded
    }
    const entt::entity foe = reg.create();
    reg.emplace<eng::sim::Transform>(foe, eng::Vec2{20.0f, 0.0f});             // in reach
    reg.emplace<eng::sim::Stats>(foe, eng::sim::Vital{100.0f, 100.0f, 0.0f});  // survives the hit
    reg.emplace<eng::sim::Attributes>(foe);  // default DEX 1 -> never dodges
    reg.emplace<eng::sim::Enemy>(foe);

    std::mt19937 rng{1234};
    eng::sim::perform_attack(reg, atk, rng);
    const eng::sim::Poisoned* p = reg.try_get<eng::sim::Poisoned>(foe);
    return p != nullptr ? p->damage_per_second : -1.0f;  // -1 sentinel = not poisoned
  };
  REQUIRE(venom_dps_after_hit(6.0f) == Approx(6.0f));   // a venom blade poisons at its dps...
  REQUIRE(venom_dps_after_hit(0.0f) == Approx(-1.0f));  // ...a bare/plain swing leaves no poison
}

TEST_CASE("equipping a venom weapon folds its venom into the wielder", "[sim]") {
  // The equip plumbing: equip_nearest_gear copies a venom blade's venom_per_second into the
  // Equipped cache's weapon_venom, so perform_attack reads it from the cache (folded once, not per
  // tick) — exactly as it folds strength_bonus and move_penalty.
  entt::registry reg;
  const entt::entity wearer = reg.create();
  reg.emplace<eng::sim::Transform>(wearer, eng::Vec2{0.0f, 0.0f});
  const entt::entity blade = reg.create();
  reg.emplace<eng::sim::Transform>(blade, eng::Vec2{5.0f, 0.0f});  // within equip reach
  reg.emplace<eng::sim::Weapon>(blade).venom_per_second = 6.0f;

  eng::sim::equip_nearest_gear(reg, wearer);

  const eng::sim::Equipped* eq = reg.try_get<eng::sim::Equipped>(wearer);
  REQUIRE(eq != nullptr);
  REQUIRE(eq->weapon_venom == Approx(6.0f));  // the venom folded into the wielder's cache
}

TEST_CASE("each creature archetype drops its own loot on death", "[sim]") {
  // The loot economy, keyed on DropKind and resolving independently: a brute yields raw OFFENCE (a
  // steel weapon), a swarmer SUSTAIN (a health orb), a sentinel DEFENCE (armour), and a spitter a
  // VENOM fang (the poison-build blade).
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
  const eng::Vec2 spitter_pos{400.0f, 400.0f};
  const entt::entity spitter = reg.create();
  reg.emplace<eng::sim::Transform>(spitter, spitter_pos);
  reg.emplace<eng::sim::Stats>(spitter, eng::sim::Vital{0.0f, 25.0f, 0.0f});  // dead
  reg.emplace<eng::sim::Enemy>(spitter).drop = eng::sim::DropKind::VenomWeapon;

  eng::sim::handle_deaths(reg, eng::Vec2{0.0f, 0.0f}, 1.0f / 60.0f);

  REQUIRE(reg.storage<eng::sim::Weapon>().size() == 2);  // the brute's steel AND the spitter's fang
  REQUIRE(reg.storage<eng::sim::Pickup>().size() == 1);  // ...the swarmer, sustain...
  REQUIRE(reg.storage<eng::sim::Armour>().size() == 1);  // ...the sentinel, armour
  // ...the armour lies where the sentinel fell.
  const entt::entity dropped_armour = *reg.view<eng::sim::Armour>().begin();
  REQUIRE(reg.get<eng::sim::Transform>(dropped_armour).position.x == Approx(sentinel_pos.x));
  REQUIRE(reg.get<eng::sim::Transform>(dropped_armour).position.y == Approx(sentinel_pos.y));
  // ...and exactly ONE of the two weapons is envenomed (the spitter's fang, at its dps) — the
  // brute's steel is plain — lying where the spitter fell.
  entt::entity fang = entt::null;
  int venom_count = 0;
  for (const entt::entity w : reg.view<eng::sim::Weapon>()) {
    if (reg.get<eng::sim::Weapon>(w).venom_per_second > 0.0f) {
      ++venom_count;
      fang = w;
    }
  }
  REQUIRE(venom_count == 1);
  REQUIRE(reg.get<eng::sim::Weapon>(fang).venom_per_second == Approx(6.0f));
  REQUIRE(reg.get<eng::sim::Transform>(fang).position.x == Approx(spitter_pos.x));
  REQUIRE(reg.get<eng::sim::Transform>(fang).position.y == Approx(spitter_pos.y));
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
  // A Weapon now lies where the player stood, with its mods intact. Find the one AT the feet (the
  // opening scene seeds other weapons elsewhere, so match by position rather than assuming it's
  // alone).
  entt::entity dropped = entt::null;
  for (const entt::entity e : world.registry().view<eng::sim::Weapon>()) {
    if (glm::distance(world.registry().get<eng::sim::Transform>(e).position, ppos) < 0.5f) {
      dropped = e;
    }
  }
  REQUIRE((dropped != entt::null));  // extra parens: Catch2 can't decompose entity != entt::null
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
  const auto scene_weapons = world.registry().view<eng::sim::Weapon>().size();
  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() == scene_weapons);  // nothing spawned
  REQUIRE_FALSE(world.registry().all_of<eng::sim::Equipped>(world.player()));
}

TEST_CASE("a downed player cannot drop its weapon", "[sim]") {
  // A helpless (Downed) player can't act — Drop is skipped, exactly like Equip and MovePlayer.
  eng::sim::World world;
  const entt::entity player = world.player();
  const auto scene_weapons = world.registry().view<eng::sim::Weapon>().size();
  world.registry().emplace<eng::sim::Equipped>(player, eng::sim::Equipped{4, 0.25f});
  world.registry().emplace<eng::sim::Downed>(player);

  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();

  REQUIRE(world.registry().all_of<eng::sim::Equipped>(player));  // still wielding — couldn't drop
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() ==
          scene_weapons);  // nothing hit the ground
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

TEST_CASE("armour wears as it soaks blows and shatters: the plate's bane bites both ways",
          "[sim]") {
  // The defensive twin of weapon durability — a creature blow the plate softens wears it by one,
  // and at 0 the armour slot clears (bare again). A creature swings once per kAttackInterval
  // (0.8s), so two resolve_creature_contacts calls a second apart land two blows.
  const auto make_creature = [](entt::registry& reg) {
    const entt::entity c = reg.create();
    reg.emplace<eng::sim::Transform>(c, eng::Vec2{0.0f, 0.0f});  // on top of the victim -> in reach
    reg.emplace<eng::sim::Enemy>(c);  // brute: attack_damage 15, ready to swing
    return c;
  };

  SECTION("an armour-only wearer shatters to bare: the empty Equipped is removed") {
    entt::registry reg;
    const entt::entity victim = reg.create();
    reg.emplace<eng::sim::Transform>(victim, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(victim);       // 100 HP, survives two softened blows
    reg.emplace<eng::sim::Attributes>(victim);  // DEX 1 -> never dodges
    eng::sim::Equipped eq{};
    eq.defence_bonus = 6.0f;
    eq.stamina_regen_penalty = 0.30f;
    eq.armour_durability = 2.0f;  // two blows of life left
    reg.emplace<eng::sim::Equipped>(victim, eq);
    make_creature(reg);
    std::mt19937 rng{1234};

    eng::sim::resolve_creature_contacts(reg, 1.0f, rng);  // blow 1: armour 2 -> 1
    REQUIRE(reg.get<eng::sim::Equipped>(victim).armour_durability == Approx(1.0f));
    eng::sim::resolve_creature_contacts(reg, 1.0f, rng);    // blow 2: armour 1 -> 0 -> SHATTERS
    REQUIRE_FALSE(reg.all_of<eng::sim::Equipped>(victim));  // empty cache dropped -> truly bare
  }

  SECTION(
      "armour shattering with a weapon wielded keeps the Equipped: only the armour slot clears") {
    entt::registry reg;
    const entt::entity victim = reg.create();
    reg.emplace<eng::sim::Transform>(victim, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(victim);
    reg.emplace<eng::sim::Attributes>(victim);
    eng::sim::Equipped eq{};
    eq.defence_bonus = 6.0f;
    eq.stamina_regen_penalty = 0.30f;
    eq.armour_durability = 1.0f;  // one blow left...
    eq.strength_bonus = 4;        // ...but a weapon is also wielded
    eq.move_penalty = 0.25f;
    reg.emplace<eng::sim::Equipped>(victim, eq);
    make_creature(reg);
    std::mt19937 rng{1234};

    eng::sim::resolve_creature_contacts(reg, 1.0f, rng);  // the one blow shatters the plate
    const eng::sim::Equipped& after =
        reg.get<eng::sim::Equipped>(victim);       // still present (weapon)
    REQUIRE(after.defence_bonus == Approx(0.0f));  // armour slot cleared...
    REQUIRE(after.strength_bonus == 4);            // ...but the weapon slot is untouched
  }
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

TEST_CASE("a starving or parched character gets no second wind: stamina rest-recovery is gated",
          "[sim]") {
  // The stamina twin of the starvation heal-gate: an empty stomach OR canteen suppresses resting
  // stamina recovery, so survival failure drains your reserves, not just your health (and, with the
  // stamina==0 crawl, a fleeing starver tires to a crawl). A fed, watered rester recovers as
  // before.
  const auto rested_stamina = [](float hunger, float water) {
    entt::registry reg;
    const entt::entity e = reg.create();
    auto& st = reg.emplace<eng::sim::Stats>(e);
    st.stamina = eng::sim::Vital{50.0f, 100.0f, 20.0f};  // current 50, recovers 20/s
    st.hunger.current = hunger;
    st.water.current = water;
    reg.emplace<eng::sim::Velocity>(e);  // zero velocity -> resting
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) eng::sim::update_stamina(reg, dt);  // half a second resting
    return reg.get<eng::sim::Stats>(e).stamina.current;
  };
  REQUIRE(rested_stamina(100.0f, 100.0f) > 50.0f);         // fed & watered -> recovers (unchanged)
  REQUIRE(rested_stamina(0.0f, 100.0f) == Approx(50.0f));  // starving -> no second wind
  REQUIRE(rested_stamina(100.0f, 0.0f) == Approx(50.0f));  // dehydrated -> no second wind
}

TEST_CASE("dropping a weapon keeps your armour (slot-aware Drop)", "[sim]") {
  // Drop is slot-aware: it sheds only the weapon, leaving worn armour on — a blanket
  // remove<Equipped> would silently strip the armour.
  eng::sim::World world;
  const entt::entity player = world.player();
  const auto scene_weapons =
      world.registry().view<eng::sim::Weapon>().size();  // the opening scene seeds some
  world.registry().emplace<eng::sim::Equipped>(player,
                                               eng::sim::Equipped{4, 0.25f, 6.0f, 0.30f});  // both

  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();

  const eng::sim::Equipped* eq = world.registry().try_get<eng::sim::Equipped>(player);
  REQUIRE(eq != nullptr);            // still wearing something (the armour)...
  REQUIRE(eq->strength_bonus == 0);  // ...the weapon slot was shed...
  REQUIRE(eq->move_penalty == Approx(0.0f));
  REQUIRE(eq->defence_bonus == Approx(6.0f));  // ...but the armour slot survived intact
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() ==
          scene_weapons + 1);  // and a weapon hit the ground
}

TEST_CASE("dropping with only armour worn is a no-op", "[sim]") {
  // No weapon to shed -> Drop does nothing (no phantom weapon, armour untouched).
  eng::sim::World world;
  const entt::entity player = world.player();
  const auto scene_weapons = world.registry().view<eng::sim::Weapon>().size();
  world.registry().emplace<eng::sim::Equipped>(
      player, eng::sim::Equipped{0, 0.0f, 6.0f, 0.30f});  // armour only

  world.submit(eng::sim::drop(eng::sim::kLocalPlayer));
  world.step();

  const eng::sim::Equipped* eq = world.registry().try_get<eng::sim::Equipped>(player);
  REQUIRE(eq != nullptr);  // armour untouched...
  REQUIRE(eq->defence_bonus == Approx(6.0f));
  REQUIRE(world.registry().view<eng::sim::Weapon>().size() ==
          scene_weapons);  // ...and no phantom weapon dropped
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

TEST_CASE("a wounded creature limps: low HP slows its chase", "[sim]") {
  // The creature-side mirror of the player's exhaustion crawl — below 30% of its HP a creature
  // chases at kLimpMoveScale (it struggles), so a worn-down brute can be KITED even as it enrages.
  // A healthy creature chases at full speed, which is why the chase-speed test above (whose
  // creatures carry no Stats, so the limp check is skipped) stays bit-identical.
  const auto chase_speed = [](float hp_fraction) {
    entt::registry reg;
    const entt::entity person = reg.create();
    reg.emplace<eng::sim::Transform>(person, eng::Vec2{100.0f, 0.0f});  // prey to the +x
    reg.emplace<eng::sim::Stats>(person);
    const entt::entity creature = reg.create();
    reg.emplace<eng::sim::Transform>(creature, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(creature);
    reg.emplace<eng::sim::Enemy>(creature).chase_speed = 100.0f;
    reg.emplace<eng::sim::Stats>(creature, eng::sim::Vital{hp_fraction * 100.0f, 100.0f, 0.0f});
    eng::sim::chase_prey(reg);
    return glm::length(reg.get<eng::sim::Velocity>(creature).value);
  };
  REQUIRE(chase_speed(1.0f) == Approx(100.0f));  // full HP -> its full chase_speed...
  REQUIRE(chase_speed(0.2f) == Approx(60.0f));   // ...20% HP (< 30%) -> limps at 0.6x
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

TEST_CASE("the hearth wards creatures: a beast leaves prey sheltering in the fire", "[sim]") {
  // The fire's DEFENSIVE half (regenerate_vitals is its healing half): a creature won't hunt prey
  // standing in a hearth's glow, so reaching the fire is a real escape — the same in_a_hearth reach
  // both heals and hides. Same beast, same colonist; only a hearth around the prey differs.
  const auto hunts = [](bool sheltered) {
    entt::registry reg;
    const entt::entity beast = reg.create();
    reg.emplace<eng::sim::Transform>(beast, eng::Vec2{0.0f, 0.0f});
    reg.emplace<eng::sim::Velocity>(beast);
    reg.emplace<eng::sim::Enemy>(beast).chase_speed = 100.0f;
    reg.emplace<eng::sim::Stats>(beast, eng::sim::Vital{40.0f, 40.0f, 0.0f});  // full HP -> no limp
    const entt::entity colonist = reg.create();
    reg.emplace<eng::sim::Transform>(colonist, eng::Vec2{100.0f, 0.0f});
    reg.emplace<eng::sim::Stats>(colonist);
    reg.emplace<eng::sim::Npc>(colonist);
    if (sheltered) {
      const entt::entity hearth = reg.create();
      reg.emplace<eng::sim::Transform>(hearth, eng::Vec2{100.0f, 0.0f});  // right on the colonist
      reg.emplace<eng::sim::Hearth>(hearth, eng::sim::Hearth{50.0f});     // radius covers it
    }
    eng::sim::chase_prey(reg);
    return glm::length(reg.get<eng::sim::Velocity>(beast).value) > 0.0f;  // did it move to hunt?
  };
  REQUIRE(hunts(false));  // a colonist in the open is hunted...
  REQUIRE_FALSE(
      hunts(true));  // ...one in the fire's glow is left alone (the beast finds no target)

  // And it's a WARD, not a full stop: a beast passes over a sheltered colonist to run down whoever
  // is still exposed. A near colonist sits in a hearth to the +x; a farther one stands in the open
  // to the -x. The beast skips the near-sheltered and chases the far-exposed (velocity heads -x),
  // proving it RE-TARGETS rather than freezing whenever any prey is safe.
  entt::registry reg;
  const entt::entity beast = reg.create();
  reg.emplace<eng::sim::Transform>(beast, eng::Vec2{0.0f, 0.0f});
  reg.emplace<eng::sim::Velocity>(beast);
  reg.emplace<eng::sim::Enemy>(beast).chase_speed = 100.0f;
  const entt::entity sheltered = reg.create();
  reg.emplace<eng::sim::Transform>(sheltered, eng::Vec2{50.0f, 0.0f});  // near, +x
  reg.emplace<eng::sim::Stats>(sheltered);
  reg.emplace<eng::sim::Npc>(sheltered);
  const entt::entity hearth = reg.create();
  reg.emplace<eng::sim::Transform>(hearth, eng::Vec2{50.0f, 0.0f});
  reg.emplace<eng::sim::Hearth>(hearth, eng::sim::Hearth{30.0f});  // shelters the near colonist
  const entt::entity exposed = reg.create();
  reg.emplace<eng::sim::Transform>(exposed, eng::Vec2{-150.0f, 0.0f});  // far, -x, no fire
  reg.emplace<eng::sim::Stats>(exposed);
  reg.emplace<eng::sim::Npc>(exposed);
  eng::sim::chase_prey(reg);
  REQUIRE(reg.get<eng::sim::Velocity>(beast).value.x <
          0.0f);  // passed the sheltered, ran at the exposed
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

TEST_CASE("the opening archetypes spawn with their own numbers (brute, swarmer, spitter)",
          "[sim]") {
  // make_brute/make_swarmer/make_spitter are file-local, but a fresh World seeds one of each. Pin
  // their HP/speed/damage here: make_creature takes three adjacent float params (hp, chase_speed,
  // attack_damage) that a future archetype could transpose silently — this is the guard that would
  // catch it. The spitter also pins its ranged + venom knobs (spit_range/damage/poison), which
  // melee kinds leave 0.
  eng::sim::World world;
  entt::registry& reg = world.registry();

  bool saw_brute = false;
  bool saw_swarmer = false;
  bool saw_spitter = false;
  for (const entt::entity e : reg.view<eng::sim::Enemy>()) {
    const float hp = reg.get<eng::sim::Stats>(e).health.max;
    const eng::sim::Enemy& en = reg.get<eng::sim::Enemy>(e);
    const int dex = reg.get<eng::sim::Attributes>(e).dexterity.level;
    if (hp == Approx(40.0f)) {  // brute: tanky, slow, hits hard — but never dodges
      saw_brute = true;
      REQUIRE(en.chase_speed == Approx(70.0f));
      REQUIRE(en.attack_damage == Approx(15.0f));
      REQUIRE(dex == 1);               // default Dexterity -> dodge_chance 0
      REQUIRE(en.spit_range == 0.0f);  // melee-only
    } else if (hp ==
               Approx(25.0f)) {  // spitter: fragile, slow, feeble melee — but RANGED + venomous
      saw_spitter = true;
      REQUIRE(en.chase_speed == Approx(55.0f));
      REQUIRE(en.attack_damage == Approx(4.0f));
      REQUIRE(en.spit_range == Approx(250.0f));
      REQUIRE(en.spit_damage == Approx(7.0f));
      REQUIRE(en.poison_per_second == Approx(5.0f));  // its spit envenoms on hit
    } else {  // swarmer: fragile, fast, weak — and slippery
      saw_swarmer = true;
      REQUIRE(hp == Approx(15.0f));
      REQUIRE(en.chase_speed == Approx(130.0f));
      REQUIRE(en.attack_damage == Approx(8.0f));
      REQUIRE(dex == 8);               // innate Dexterity -> ~21% chance to dodge a strike
      REQUIRE(en.spit_range == 0.0f);  // melee-only
    }
  }
  REQUIRE(saw_brute);
  REQUIRE(saw_swarmer);
  REQUIRE(saw_spitter);
}
