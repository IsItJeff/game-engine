#include "engine/sim/systems.hpp"

#include <cmath>
#include <cstdint>
#include <random>
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
  // Foraging: once hunger drops below this fraction of max, a safe NPC heads for the
  // nearest food orb it can see. Larger sense radius than danger — a hungry colonist
  // scans wider for a meal. Knobs.
  constexpr float kHungerSeekFraction = 0.6f;
  constexpr float kForageRadius = 260.0f;
  constexpr float kForageSpeed = 80.0f;

  // Nested loops: every NPC against every hazard / every orb — O(n*m), fine for a handful.
  // A real crowd would query a spatial grid, the same upgrade resolve_contacts wants.
  // ponytail: no orb-reservation — several hungry NPCs can converge on one orb and the
  // first to reach it eats (collect_pickups); add claims only if the scramble looks bad.
  auto npcs = reg.view<Npc, Transform, Velocity>();
  auto hazards = reg.view<Hazard, Transform>();
  auto food = reg.view<Pickup, Transform>();
  for (const entt::entity n : npcs) {
    const Vec2 pos = npcs.get<Transform>(n).position;

    // Perception, priority 1 — danger: the single nearest hazard within sense range.
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

    // Fear beats hunger: flee straight away from a threat, ignoring food.
    if (sees_threat) {
      const Vec2 away = pos - threat;
      const float len = glm::length(away);
      if (len > 0.0f) npcs.get<Velocity>(n).value = (away / len) * kFleeSpeed;
      continue;
    }

    // Priority 2 — hunger: a safe but hungry colonist seeks the nearest orb (its FIRST
    // want-driven motion; until now NPCs only ever fled). A fed one just drifts. It eats
    // when it arrives, in collect_pickups — this only steers it there.
    const Stats* stats = reg.try_get<Stats>(n);
    if (stats == nullptr || stats->hunger.current >= stats->hunger.max * kHungerSeekFraction) {
      continue;  // not hungry (or no Stats) — keep drifting
    }
    entt::entity meal = entt::null;
    float nearest_food = kForageRadius;
    for (const entt::entity f : food) {
      const float d = glm::distance(pos, food.get<Transform>(f).position);
      if (d < nearest_food) {
        nearest_food = d;
        meal = f;
      }
    }
    if (meal == entt::null) continue;  // nothing edible in range — keep drifting
    const Vec2 toward = food.get<Transform>(meal).position - pos;
    const float len = glm::length(toward);
    if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kForageSpeed;
  }
}

void chase_prey(entt::registry& reg) {
  // Creatures hunt PEOPLE — the player and NPCs alike. "Prey" is everything with a Stats
  // sheet (so it can be hurt) that isn't itself a creature: motes and pickups have no
  // Stats, and `exclude<Enemy>` drops the creatures, so this view is exactly players +
  // NPCs. Each creature homes on the NEAREST one — the hostile mirror of steer_npcs
  // fleeing its nearest hazard. This is what makes the world feel alive rather than
  // player-centric: creatures and NPCs actually war, and an NPC caught in the open can be
  // run down and killed for good (permadeath), not just the player.
  auto prey = reg.view<Stats, Transform>(entt::exclude<Enemy>);
  auto creatures = reg.view<Enemy, Transform, Velocity>();
  for (const entt::entity c : creatures) {
    const Vec2 c_pos = creatures.get<Transform>(c).position;

    // Nearest person wins. Strict < keeps ties deterministic (first by iteration order).
    // ponytail: no target-lock/hysteresis — a creature exactly between two people can
    // flip its aim tick-to-tick; add a lock only if that wobble ever shows in play.
    entt::entity target = entt::null;
    float nearest = 0.0f;
    for (const entt::entity person : prey) {
      const float d = glm::distance(c_pos, prey.get<Transform>(person).position);
      if (target == entt::null || d < nearest) {
        nearest = d;
        target = person;
      }
    }
    if (target == entt::null) continue;  // nobody left to hunt — keep drifting

    // Home in at its OWN chase_speed — a brute lumbers, a swarmer sprints. All are slower
    // than the player's 320 top speed, so a fight is always kite-able.
    const Vec2 toward = prey.get<Transform>(target).position - c_pos;
    const float len = glm::length(toward);
    if (len > 0.0f)
      creatures.get<Velocity>(c).value = (toward / len) * creatures.get<Enemy>(c).chase_speed;
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
  // Each Endurance level past the first speeds stamina recovery by this fraction —
  // hardiness means a better second wind. A tunable, and a new *effect* for
  // Endurance beyond the pool size.
  constexpr float kRecoveryPerEndurance = 0.10f;

  // Stats + Velocity = things that both tire and move. Motes have Velocity but no
  // Stats, so the view skips them for free — only the player pays.
  auto view = reg.view<Stats, Velocity>();
  for (const entt::entity e : view) {
    Vital& stamina = view.get<Stats>(e).stamina;
    if (glm::length(view.get<Velocity>(e).value) > 0.0f) {
      stamina.current -= kDrainPerSecond * dt;             // moving: spend it...
      if (stamina.current < 0.0f) stamina.current = 0.0f;  // ...never below empty
    } else {
      // Resting: recover, faster the tougher you are. A no-Attributes entity just
      // uses the base rate (boost 1.0).
      const Attributes* attrs = reg.try_get<Attributes>(e);
      const float boost = attrs != nullptr ? 1.0f + static_cast<float>(attrs->endurance.level - 1) *
                                                        kRecoveryPerEndurance
                                           : 1.0f;
      stamina.current += stamina.regen_per_second * boost * dt;
      if (stamina.current > stamina.max) stamina.current = stamina.max;  // never past the cap
    }
  }
}

void drain_hunger(entt::registry& reg, float dt) {
  // Baseline hunger lost per second at rest, plus extra while moving (the design's
  // "exertion drains needs" rule — the same moving/idle split as update_stamina). Gentle
  // on purpose: hunger empties over MINUTES, not seconds, so it's a background pressure
  // and long-running tests don't accidentally starve their entities. Tuning knobs.
  constexpr float kDrainPerSecond = 0.3f;          // at rest
  constexpr float kExertionDrainPerSecond = 0.3f;  // added while moving
  // Health lost per second once hunger hits 0. MUST exceed the fastest self-heal in play
  // (the player regenerates 8/s) or regenerate_vitals would out-pace it and starvation
  // could never actually kill — so a starving character's health nets downward.
  constexpr float kStarvationPerSecond = 12.0f;

  // Every PERSON gets hungry — the player and NPCs (Stats without the Enemy marker, the
  // same "people, not monsters" set chase_prey hunts). Creatures are excluded: they're
  // pure combat foes, not colonists with bellies to fill. NPCs now feed themselves too:
  // a hungry one forages for the nearest orb (steer_npcs) and eats it (collect_pickups).
  // ponytail: the only food is still loot orbs (from creature deaths, finite and clustered
  // where kills happen), so a colonist starving in a quiet corner can still die — the full
  // fix is a real food economy (crops/meals), a later survival slice.
  auto view = reg.view<Stats>(entt::exclude<Enemy>);
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);
    float drain = kDrainPerSecond;
    if (const Velocity* v = reg.try_get<Velocity>(e);
        v != nullptr && glm::length(v->value) > 0.0f) {
      drain += kExertionDrainPerSecond;  // moving costs extra
    }
    s.hunger.current -= drain * dt;
    if (s.hunger.current < 0.0f) s.hunger.current = 0.0f;  // never below empty

    // Starving: an empty stomach gnaws at your health, so an unfed character dies through
    // the SAME handle_deaths path as any other 0-HP death — and NOT via Endurance, since
    // the design keeps VIT as pure combat defence that doesn't buffer needs.
    if (s.hunger.current <= 0.0f) {
      s.health.current -= kStarvationPerSecond * dt;
      if (s.health.current < 0.0f) s.health.current = 0.0f;
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
  // Resting to recover spent stamina trains Recovery a touch slower than moving
  // trains Conditioning (15 XP/sec vs 20) — resting is easier than exerting.
  const Fixed kRecoveryPerTick = Fixed::from_ratio(15, 60);

  // NB: CharacterLevel is required here, so any new progression-capable entity must
  // be spawned WITH it (see world.cpp: player + NPC) — miss it and that entity
  // silently never grows, no error. Keep the progression components together.
  auto view = reg.view<Skills, Attributes, Stats, Velocity, CharacterLevel>();
  for (const entt::entity e : view) {
    Skill& conditioning = view.get<Skills>(e).train(SkillId::Conditioning);
    Attributes& attrs = view.get<Attributes>(e);
    Attribute& endurance = attrs.endurance;
    CharacterLevel& character = view.get<CharacterLevel>(e);

    // 1. Activity earns XP for the SKILL and its MAIN attribute, plus a fraction to
    //    the global Character Level. Conditioning's main attribute is Endurance, so
    //    it takes the full share for now (skills with contributors will split it
    //    later). Moving trains Conditioning; resting to recover *spent* stamina
    //    trains Recovery instead — both feed Endurance. Idle at full stamina, or
    //    with no spending to recover from, trains nothing.
    if (glm::length(view.get<Velocity>(e).value) > 0.0f) {
      conditioning.xp += kConditioningPerTick;
      endurance.xp += kConditioningPerTick;
      character.xp += kConditioningPerTick * kCharLevelShare;
    } else if (const Vital& stamina = view.get<Stats>(e).stamina; stamina.current < stamina.max) {
      Skill& recovery = view.get<Skills>(e).train(SkillId::Recovery);
      recovery.xp += kRecoveryPerTick;
      endurance.xp += kRecoveryPerTick;
      character.xp += kRecoveryPerTick * kCharLevelShare;
    }

    // 2. Turn full XP bars into levels — EVERY trained skill, both attributes, and
    //    the character level each climb on their own {level, Fixed xp}. Iterating all
    //    owned skills means a skill trained elsewhere (Toughness via train_on_damage,
    //    Striking via the Attack command) levels here too, without this system
    //    knowing the source; likewise Strength's XP is granted at the attack site.
    for (auto& entry : view.get<Skills>(e).owned) apply_levels(entry.second.level, entry.second.xp);
    apply_levels(endurance.level, endurance.xp);
    apply_levels(attrs.strength.level, attrs.strength.xp);
    apply_levels(attrs.dexterity.level, attrs.dexterity.xp);  // fed by Evasion at the dodge site
    apply_levels(attrs.luck.level, attrs.luck.xp);            // fed by Scavenging at the loot site
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
  // Only a progression entity (Skills + Attributes) toughens; a bare-Stats target
  // trains nothing. This is THE funnel every damage source will flow through
  // (weapons, crits, falloff…), so validate the input HERE: reject non-finite or
  // non-positive damage before the float→int cast below. A bare `<= 0` check misses
  // NaN (`NaN <= 0` is false) and Inf, and casting those to int is UB — cheap to
  // guard now, a real hazard once a computed-damage source can produce one.
  Skills* skills = reg.try_get<Skills>(victim);
  Attributes* attrs = reg.try_get<Attributes>(victim);
  if (skills == nullptr || attrs == nullptr || !std::isfinite(damage) || damage <= 0.0f) return;

  // 1 XP per point of damage survived — a tunable, so ~100 damage endured earns a
  // Toughness level. Snap the float pool damage to an int BEFORE the Fixed XP path
  // so the deterministic accumulator never sees a float; cap first, since casting a
  // float past int range is UB (from_int saturates in Fixed anyway). Toughness's
  // main attribute is Endurance, so (like Conditioning) it feeds the whole share.
  const float bounded = damage < 1.0e6f ? damage : 1.0e6f;
  const Fixed gained = Fixed::from_int(static_cast<std::int32_t>(bounded));
  skills->train(SkillId::Toughness).xp += gained;
  attrs->endurance.xp += gained;
  // ponytail: the Character Level is fed by movement only for now; routing every
  // damage source into "fraction of all activity" waits until combat gives more
  // sources to balance against. advance_progression levels Toughness next tick.
}

namespace {

// Ratio mitigation (the design's damage formula): defence softens a blow forever but
// never negates it — at least a 10% chip always lands. raw > 0 and def >= 0, so the
// denominator is positive; def == 0 gives raw*raw/raw == raw (full damage).
float mitigate(float raw, float def) {
  const float softened = raw * raw / (raw + def);
  const float chip_floor = 0.10f * raw;
  return softened > chip_floor ? softened : chip_floor;
}

// An entity's physical defence from its VIT (Endurance). No Attributes → no defence,
// so a bare mote or a plain target takes full damage.
float defence_of(const entt::registry& reg, entt::entity e) {
  constexpr float kDefencePerVit = 3.0f;  // each Endurance level past 1 adds this much
  const Attributes* a = reg.try_get<Attributes>(e);
  return a != nullptr ? static_cast<float>(a->endurance.level - 1) * kDefencePerVit : 0.0f;
}

// Chance in [0, kCap] that a blow is dodged entirely, from the victim's DEX. Level 1
// is 0 — no head start, exactly like every other stat, and (usefully) it means an
// untrained world never rolls, so the RNG stream stays identical to before evasion
// existed until someone actually earns Dexterity. Hard-capped so evasion — the
// defensive mirror of mitigate's 10% chip floor — softens the incoming stream forever
// but never guarantees a miss.
// ponytail: linear ramp + flat cap are tuning knobs; swap in a POWER-curve shape if
// dodge should diminish like the rest, but linear-to-a-cap plays fine and reads clearly.
float dodge_chance(int dexterity_level) {
  constexpr float kPerLevel = 0.03f;  // +3% dodge per Dexterity level past the first
  constexpr float kCap = 0.50f;       // never dodge more than half — a stream of hits still lands
  const float chance = static_cast<float>(dexterity_level - 1) * kPerLevel;
  return chance < kCap ? chance : kCap;
}

// Chance in [0, kCap] that a strike CRITS (deals extra damage), from the attacker's LCK —
// the offensive-fortune mirror of dodge_chance. Level 1 = 0 (no head start, and the
// chance > 0 guard at the call site means an untrained attacker never even draws), each
// level tilts fortune a little further, hard-capped so Luck sways the fight without ever
// guaranteeing the big hit.
// ponytail: the ramp/cap here (and the crit multiplier at the call site) are tuning knobs,
// cloned from dodge for now — give crit its own curve if playtest wants it to feel different.
float crit_chance(int luck_level) {
  constexpr float kPerLevel = 0.03f;  // +3% crit per Luck level past the first
  constexpr float kCap = 0.50f;       // never crit more than half your swings
  const float chance = static_cast<float>(luck_level - 1) * kPerLevel;
  return chance < kCap ? chance : kCap;
}

}  // namespace

entt::entity perform_attack(entt::registry& reg, entt::entity attacker, std::mt19937& rng) {
  constexpr float kBaseReach = 45.0f;                 // a little past contact range (15)
  constexpr float kReachPerStrength = 6.0f;           // each Strength level past 1 adds reach
  constexpr float kBaseAttackDamage = 12.0f;          // a swing's raw damage before Strength
  constexpr float kDamagePerStrength = 4.0f;          // each Strength level past 1 hits harder
  const Fixed kStrikingPerHit = Fixed::from_int(10);  // XP for a strike that connects

  // A swinger needs a position (to reach from) and the progression pair to train.
  const Transform* tf = reg.try_get<Transform>(attacker);
  Attributes* attrs = reg.try_get<Attributes>(attacker);
  Skills* skills = reg.try_get<Skills>(attacker);
  if (tf == nullptr || attrs == nullptr || skills == nullptr) return entt::null;

  const Vec2 origin = tf->position;
  const float reach =
      kBaseReach + static_cast<float>(attrs->strength.level - 1) * kReachPerStrength;

  // Find the nearest attackable target in reach — a fragile mote (Hazard) or a hostile
  // creature (Enemy). Strict < breaks ties by iteration order (deterministic).
  entt::entity target = entt::null;
  float nearest = reach;
  bool target_is_enemy = false;
  auto hazards = reg.view<Hazard, Transform>();
  for (const entt::entity h : hazards) {
    const float d = glm::distance(origin, hazards.get<Transform>(h).position);
    if (d < nearest) {
      nearest = d;
      target = h;
      target_is_enemy = false;
    }
  }
  auto enemies = reg.view<Enemy, Transform>();
  for (const entt::entity e : enemies) {
    const float d = glm::distance(origin, enemies.get<Transform>(e).position);
    if (d < nearest) {
      nearest = d;
      target = e;
      target_is_enemy = true;
    }
  }
  if (target == entt::null) return entt::null;  // a whiff — nothing in reach, no XP

  // A connecting strike trains Striking -> Strength, whatever it hits.
  skills->train(SkillId::Striking).xp += kStrikingPerHit;
  attrs->strength.xp += kStrikingPerHit;

  // A mote is fragile: hand it back for the caller to destroy outright.
  if (!target_is_enemy) return target;

  // The target may DODGE the strike — the offensive mirror of the player dodging a
  // creature's blow (resolve_creature_contacts), the same dodge_chance keyed on the
  // TARGET's DEX. A swarmer is slippery (innate Dexterity), a brute is not (DEX 1 -> 0,
  // and the chance > 0 guard means it never even draws). The swing still trained Striking
  // above — you learn from a whiff — only the damage is skipped. Creatures don't grow, so
  // there's no Evasion training on their side; their DEX is a fixed archetype trait.
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  const Attributes* target_attrs = reg.try_get<Attributes>(target);
  const float dodge = dodge_chance(target_attrs != nullptr ? target_attrs->dexterity.level : 1);
  if (dodge > 0.0f && unit(rng) < dodge) return entt::null;  // slipped it — no damage dealt

  // An enemy takes STR-vs-VIT damage to its HP — base plus Strength, softened by the
  // enemy's VIT. It is NOT destroyed here; handle_deaths reaps it at 0 HP, so a weak
  // hit only chips it and it takes several. (An Enemy always carries Stats.)
  const float raw =
      kBaseAttackDamage + static_cast<float>(attrs->strength.level - 1) * kDamagePerStrength;
  const float base_damage = mitigate(raw, defence_of(reg, target));

  // A LUCKY strike CRITS for extra damage — the offensive-fortune mirror of a dodge,
  // rolled off the ATTACKER's LCK. It sits after the dodge return (only a connecting hit
  // can crit) and reuses the `unit` distribution above; the chance > 0 guard means an
  // untrained attacker (LCK 1) never draws, so the seeded stream is unchanged until Luck
  // is earned (by scavenging loot — see collect_pickups). Creatures have LCK 1, so they
  // never crit, exactly as they never dodge or train — a fixed floor, not a trained stat.
  constexpr float kCritMultiplier = 2.0f;  // a crit doubles the blow (ponytail: a knob)
  const float crit = crit_chance(attrs->luck.level);
  const float applied =
      (crit > 0.0f && unit(rng) < crit) ? base_damage * kCritMultiplier : base_damage;
  if (Stats* st = reg.try_get<Stats>(target); st != nullptr) {
    st->health.current -= applied;
    if (st->health.current < 0.0f) st->health.current = 0.0f;
  }
  return entt::null;
}

void npc_attack(entt::registry& reg, std::mt19937& rng) {
  // Every NPC swings at the nearest hazard in reach — the same perform_attack the
  // player's command uses, so NPCs build Strength too. Collect-then-destroy: if two
  // NPCs pick the same mote, the valid() guard below makes the second destroy a
  // no-op, and both trained on the swing — fine, they both connected on it.
  //
  // ponytail: NPCs swing at their FULL Strength reach every tick, so they clear
  // motes that drift near them quickly. If the field empties too fast in play, gate
  // this on a threat range (only strike a mote about to make contact) or a per-tick
  // chance — a tuning knob, not a redesign.
  std::vector<entt::entity> struck;
  auto npcs = reg.view<Npc, Transform, Attributes, Skills>();
  for (const entt::entity n : npcs) {
    const entt::entity t = perform_attack(reg, n, rng);
    if (t != entt::null) struck.push_back(t);
  }
  for (const entt::entity t : struck) {
    if (reg.valid(t)) reg.destroy(t);
  }
}

namespace {

// Drop a health pickup at `pos` — a small cyan orb a slain creature leaves behind.
void spawn_pickup(entt::registry& reg, Vec2 pos) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.3f, 0.9f, 0.8f}, 4.0f);  // small cyan orb
  reg.emplace<Pickup>(e);
}

}  // namespace

void handle_deaths(entt::registry& reg, Vec2 respawn_point) {
  // A zero-health entity meets one of two fates, and which one is the game's core
  // rule made concrete: the PLAYER respawns; an NPC dies for good (permadeath).

  // Players: reset a few components in place. Nothing is created or destroyed, so
  // writing to the view while iterating it is safe.
  auto players = reg.view<Stats, PlayerControlled, Transform, Velocity>();
  for (const entt::entity e : players) {
    Stats& s = players.get<Stats>(e);
    if (s.health.current > 0.0f) continue;  // still alive, nothing to do
    // Respawn WHOLE, not just un-dead: restore every vital, not only health. Hunger in
    // particular MUST reset — it doesn't self-recover, so respawning a starved player with
    // hunger still 0 would drop them straight back into starving and a re-death loop. Reset
    // stamina too, so a fresh life starts rested rather than mid-exhaustion.
    s.health.current = s.health.max;                     // full health...
    s.stamina.current = s.stamina.max;                   // ...rested...
    s.hunger.current = s.hunger.max;                     // ...and fed — a genuine clean slate
    players.get<Transform>(e).position = respawn_point;  // at the spawn point...
    players.get<Velocity>(e).value = Vec2{0.0f, 0.0f};   // ...and standing still
  }

  // NPCs and creatures: permadeath. Collect the dead, then destroy them AFTER the
  // loop — reg.destroy() during iteration invalidates the view (the same collect-
  // then-destroy pattern resolve_contacts uses to consume motes). A slain Enemy
  // dies here too, the same way an NPC does — one death path for everyone non-player.
  std::vector<entt::entity> dead;
  std::vector<Vec2> loot_drops;  // where slain creatures fell — a pickup lands at each
  auto npcs = reg.view<Stats, Npc>();
  for (const entt::entity e : npcs) {
    if (npcs.get<Stats>(e).health.current <= 0.0f) dead.push_back(e);
  }
  auto creatures = reg.view<Stats, Enemy>();
  for (const entt::entity e : creatures) {
    if (creatures.get<Stats>(e).health.current <= 0.0f) {
      dead.push_back(e);
      loot_drops.push_back(reg.get<Transform>(e).position);
    }
  }
  for (const entt::entity e : dead) reg.destroy(e);
  for (const Vec2& pos : loot_drops) spawn_pickup(reg, pos);  // loot for the win
}

void collect_pickups(entt::registry& reg, float dt) {
  constexpr float kPickupDistance = 15.0f;  // same reach as a contact

  std::vector<entt::entity> taken;  // collected or faded — collect, then destroy (never mid-view)
  // Anyone with a Stats sheet who ISN'T a creature can eat an orb — the player and hungry
  // NPCs alike (the same "people, not monsters" set drain_hunger and chase_prey use). The
  // exclude<Enemy> is load-bearing: creatures also carry Stats, and letting them heal and
  // gain permanent max-HP off the very orbs they drop would break the fight. The eat body
  // below already try_get-guards Skills/Attributes, so an NPC grows from loot exactly as the
  // player does — the parity the design's "NPCs run the identical system" demands.
  auto eaters = reg.view<Stats, Transform>(entt::exclude<Enemy>);
  auto pickups = reg.view<Pickup, Transform>();
  for (const entt::entity item : pickups) {
    Pickup& pk = pickups.get<Pickup>(item);
    pk.lifetime -= dt;
    if (pk.lifetime <= 0.0f) {  // ungrabbed too long — it fades
      taken.push_back(item);
      continue;
    }
    const Vec2 item_pos = pickups.get<Transform>(item).position;
    for (const entt::entity p : eaters) {
      if (glm::distance(eaters.get<Transform>(p).position, item_pos) >= kPickupDistance) continue;
      Stats& stats = eaters.get<Stats>(p);
      Vital& health = stats.health;
      // A permanent max-HP bump: grow base (which advance_progression keeps max in step
      // with each tick) and max now too — the direct max bump is also the only growth
      // for a collector without Attributes, whose max is never recomputed.
      health.base += pk.bonus_max_hp;
      health.max += pk.bonus_max_hp;
      health.current += pk.heal;
      if (health.current > health.max) health.current = health.max;  // capped, no overheal

      // The orb is also FOOD: eating it refills hunger, so the same fight -> orb -> grab loop
      // keeps you fed. (A knob; the orb is the first food source until real crops/meals exist.)
      constexpr float kFoodPerOrb = 50.0f;
      stats.hunger.current += kFoodPerOrb;
      if (stats.hunger.current > stats.hunger.max) stats.hunger.current = stats.hunger.max;

      // Grabbing loot trains Scavenging -> Luck (deterministic XP, no RNG), so foraging
      // the field is itself a build: more Luck -> more crits (perform_attack). Guard on the
      // pair — a collector without Skills/Attributes (see the max-HP note above) still gets
      // the heal, it just doesn't grow Luck.
      Skills* sk = reg.try_get<Skills>(p);
      Attributes* a = reg.try_get<Attributes>(p);
      if (sk != nullptr && a != nullptr) {
        const Fixed kScavengingPerGrab = Fixed::from_int(10);  // XP per orb (ponytail: a knob)
        sk->train(SkillId::Scavenging).xp += kScavengingPerGrab;
        a->luck.xp += kScavengingPerGrab;
      }
      taken.push_back(item);
      break;  // consumed by the first collector
    }
  }
  for (const entt::entity item : taken) reg.destroy(item);
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

  // Nested loop: every hazard against every target — anything with Stats EXCEPT a
  // creature. Motes are the player's and NPCs' environmental hazard; a creature is a
  // real fight you wear down with strikes (STR-vs-VIT), so ambient motes drift right
  // through it — otherwise a mote's raw damage would sidestep its VIT and could kill
  // it before you engaged. Fine for a handful; a real crowd wants a spatial grid.
  auto targets = reg.view<Stats, Transform>(entt::exclude<Enemy>);
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

void resolve_creature_contacts(entt::registry& reg, float dt, std::mt19937& rng) {
  constexpr float kContactDistance = 15.0f;
  constexpr float kAttackInterval = 0.8f;              // seconds between a creature's swings
  const Fixed kEvasionPerSwing = Fixed::from_int(10);  // XP for facing a swing, dodged or not
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);

  // Creatures hit whoever they're hunting — the player OR an NPC (same prey set as
  // chase_prey: everything with Stats that isn't a creature). This runs before
  // handle_deaths, so a creature you kill this tick can still land a last "dying blow"
  // while at 0 HP — an accepted quirk, not a bug (reorder ahead of handle_deaths if
  // that's ever unwanted).
  auto creatures = reg.view<Enemy, Transform>();
  auto prey = reg.view<Stats, Transform>(entt::exclude<Enemy>);  // players + NPCs
  for (const entt::entity c : creatures) {
    Enemy& enemy = creatures.get<Enemy>(c);
    if (enemy.attack_timer > 0.0f) enemy.attack_timer -= dt;  // cooling down between swings
    const Vec2 c_pos = creatures.get<Transform>(c).position;
    for (const entt::entity p : prey) {
      if (glm::distance(prey.get<Transform>(p).position, c_pos) >= kContactDistance) continue;
      if (enemy.attack_timer > 0.0f) continue;  // in reach but mid-cooldown — no blow yet
      enemy.attack_timer = kAttackInterval;     // a committed swing — cooldown resets either way

      // Facing a swing trains Evasion -> Dexterity whether or not it lands: reading
      // attacks is how you learn to slip them (the mirror of Toughness training on the
      // hit). This is also the bootstrap — at DEX 1 you can't dodge yet, but every blow
      // faced grows the Dexterity that lets you start.
      Attributes* attrs = reg.try_get<Attributes>(p);
      if (Skills* sk = reg.try_get<Skills>(p); sk != nullptr && attrs != nullptr) {
        sk->train(SkillId::Evasion).xp += kEvasionPerSwing;
        attrs->dexterity.xp += kEvasionPerSwing;
      }

      // Dodge? A DEX-driven roll. Only draw when there's a real chance (DEX 1 = 0), so
      // an untrained world never perturbs the shared RNG stream — same sequence as before.
      const float chance = dodge_chance(attrs != nullptr ? attrs->dexterity.level : 1);
      if (chance > 0.0f && unit(rng) < chance) continue;  // slipped it — no damage taken

      const float applied = mitigate(enemy.attack_damage, defence_of(reg, p));
      Vital& health = prey.get<Stats>(p).health;
      health.current -= applied;
      if (health.current < 0.0f) health.current = 0.0f;
      train_on_damage(reg, p, applied);  // enduring a creature's blow toughens the victim
    }
  }
}

}  // namespace eng::sim
