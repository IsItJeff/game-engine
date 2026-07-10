#include "engine/sim/systems.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/sim/components.hpp"
#include "engine/sim/progression/curve.hpp"

namespace eng::sim {

namespace {
// How close a living ally must be to haul a downed player up. Shared by the two halves of
// the rescue: steer_npcs runs a colonist TOWARD a fallen ally until it is within this reach,
// and handle_deaths does the actual revive at this reach — one constant so "close enough to
// run to" and "close enough to lift" can never drift apart.
constexpr float kReviveDistance = 20.0f;

// A completed rescue is a Charity deed of this size on the RESCUER — one atomic "deed unit"
// (magnitude is a tuning knob; JSON-authored weights are a later milestone). standing weights
// Charity ×4, so one save reads as +4 standing.
constexpr std::int32_t kRescueCharity = 1;

// A rescue also forms a personal BOND: the rescuer gains this much affinity TOWARD the one they
// hauled up ("I'm invested in who I saved"). The relationships seed's one forming event, the twin
// of the Charity deed above. affinity is clamped to ±100, so ~5 saves of the same ally saturate the
// tie — a playtest knob.
constexpr std::int8_t kRescueAffinity = 20;

// Landing the killing blow on a HOSTILE is a Valor deed of this size on the attacker — one atomic
// deed unit, the offensive twin of a rescue's Charity. standing weights Valor ×5, so a slain
// monster reads as +5 standing.
constexpr std::int32_t kValorKill = 1;

// Striking a PEACEFUL colonist is a Cruelty deed of this size on the attacker — one atomic deed
// unit, the VILLAIN mirror of Valor and the first deed that SINKS standing. standing weights
// Cruelty ×6, so one betrayal reads as -6 standing: villainy is dearer than heroism is cheap, on
// purpose.
constexpr std::int32_t kCrueltyStrike = 1;

// A cruel strike also earns a personal GRUDGE — the negative mirror of a rescue's +kRescueAffinity
// bond: the struck colonist loses this much affinity TOWARD the striker (hurt someone and they
// resent you, exactly as saving someone endears you). A colonist whose affinity toward a player has
// sunk to kGrudgeThreshold or below will NOT cross the field to rescue that player — the resented
// are abandoned. One strike (-25) already clears the -20 line, so a single betrayal earns it: a
// pointed, PERSONAL consequence that lands EARLIER than the global villain-fear (which needs
// standing past the Suspect line, several strikes off). Playtest knobs.
constexpr std::int8_t kCrueltyGrudge = -25;
constexpr std::int8_t kGrudgeThreshold = -20;

// How far a single deed nudges the actor's matching PERSONALITY axis (deed-driven DRIFT). Small and
// bounded on purpose — the design wants "the war changed him", not a wholly different person: at 2
// per deed a full ±100 swing takes ~50 deeds, so a demo's handful gives a visible-but-partial
// shift. A tuning knob.
constexpr int kDeedDriftStep = 2;
}  // namespace

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
  // Rescue: a colonist runs to a fallen ally from this far, at this speed. The radius is
  // wide, and speed/radius are tuned so an NPC at the edge can close it (≈(300-20)/90 ≈ 3s)
  // BEFORE the ~5s Downed timer expires — otherwise the heroism is invisible. Knobs.
  constexpr float kRescueRadius = 300.0f;
  constexpr float kRescueSpeed = 90.0f;
  // Arming up: an UNARMED colonist heads for the nearest dropped weapon it can see, so gear
  // gets picked up off the battlefield rather than only by the player. Same wide-scan shape
  // as foraging; npc_equip does the actual wield on reach. Knobs.
  constexpr float kWeaponSeekRadius = 260.0f;
  constexpr float kWeaponSeekSpeed = 85.0f;
  // Thirst: once water drops below this fraction of max, a safe NPC heads for the nearest
  // WaterSource (the drink system tops it up on arrival). Same wide-scan shape as foraging. Knobs.
  constexpr float kThirstSeekFraction = 0.6f;
  constexpr float kWaterSeekRadius = 260.0f;
  constexpr float kWaterSeekSpeed = 80.0f;
  // Rally: an IDLE colonist drifts toward a nearby renowned-enough hero (the inverted twin of the
  // top-priority villain flee). A gentle gather — lower speed than a flee or a forage, since it's
  // the lowest-urgency want. Knobs.
  constexpr float kRallyRadius = 220.0f;
  constexpr float kRallySpeed = 70.0f;
  // Bond pull: the PERSONAL twin of the public hero-rally — an idle colonist with no hero to gather
  // to drifts toward a nearby FRIEND (a positive-affinity Relationship, e.g. the one it rescued).
  // Reuses kRallySpeed. Knobs.
  constexpr float kBondRadius = 220.0f;
  constexpr std::int8_t kBondPull = 10;  // affinity floor below which a tie doesn't move you

  // Nested loops: every NPC against every hazard / orb / fallen ally / weapon — O(n*m), fine
  // for a handful. A real crowd would query a spatial grid, the same upgrade resolve_contacts
  // wants. ponytail: no reservation — several NPCs can converge on one target and the first to
  // reach it takes it (collect_pickups / npc_equip); add claims only if the scramble looks bad.
  auto npcs = reg.view<Npc, Transform, Velocity>();
  auto hazards = reg.view<Hazard, Transform>();
  auto food = reg.view<Pickup, Transform>();
  auto downed = reg.view<Downed, Transform>();
  auto weapons = reg.view<Weapon, Transform>();
  auto sources = reg.view<WaterSource, Transform>();
  auto plots = reg.view<FoodSource, Transform>();
  for (const entt::entity n : npcs) {
    const Vec2 pos = npcs.get<Transform>(n).position;

    // A wielded weapon's heft slows an NPC just as it slows the player (the bane must bite
    // both — parity). Every steer speed below is scaled by this, so an armed colonist flees,
    // rescues, and forages a touch slower. Unarmed = 1.0 (no change).
    const Equipped* gear = reg.try_get<Equipped>(n);
    const float move_scale = gear != nullptr ? 1.0f - gear->move_penalty : 1.0f;

    // Perception, priority 1 — danger: the single nearest hazard within sense range. How near a
    // hazard gets before this NPC senses (and so flees) it is shaped by its BRAVERY: a coward
    // senses danger from further and bolts EARLY; a brave colonist lets a hazard get close before
    // it runs. No Personality (or bravery 0) → the base radius exactly, so this is bit-identical
    // for anyone without a leaning. ponytail: 200 is the sensitivity knob (bravery ±100 →
    // radius 0.5×..1.5× kSenseRadius). Cast the int8 to float BEFORE the divide (-Wconversion).
    const Personality* pers = reg.try_get<Personality>(n);
    const float bravery = pers != nullptr ? static_cast<float>(pers->bravery) : 0.0f;
    float nearest = kSenseRadius * (1.0f - bravery / 200.0f);
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

    // A VILLAIN is a hazard too. This is the FIRST gameplay reader of `standing`: a player whose
    // deeds have marked them a wrong'un (standing at or below the "Suspect" line, -kKnownAt) is
    // fled from exactly like a physical threat — the colony recoiling from someone it has cause to
    // fear, the design's "a Notorious player should feel the colony's fear" in miniature. It reuses
    // the SAME bravery-modulated `nearest` radius, so it competes with hazards for the nearest
    // danger and a brave colonist lets the villain get closer before bolting. A downed villain is
    // no threat (excluded); a hero, an Unproven, or a player with no ledger has standing >
    // -kKnownAt and is skipped, so a non-villain world is bit-identical to before. ponytail:
    // player-only for now (Cruelty is player-gated, so only a player can turn villain) and a BINARY
    // flee — the design's graded perceive (wariness scaling to flight by standing + the NPC's own
    // might) is a later ring.
    auto villains = reg.view<PlayerControlled, Transform>(entt::exclude<Downed>);
    for (const entt::entity v : villains) {
      const BehaviorLedger* led = reg.try_get<BehaviorLedger>(v);
      if (led == nullptr || standing(*led) > -kKnownAt) continue;  // not (yet) a marked villain
      const Vec2 v_pos = villains.get<Transform>(v).position;
      const float d = glm::distance(pos, v_pos);
      if (d < nearest) {
        nearest = d;
        threat = v_pos;
        sees_threat = true;
      }
    }

    // Fear beats everything: flee straight away from a threat, ignoring ally and food alike.
    if (sees_threat) {
      const Vec2 away = pos - threat;
      const float len = glm::length(away);
      if (len > 0.0f) npcs.get<Velocity>(n).value = (away / len) * kFleeSpeed * move_scale;
      continue;
    }

    // Priority 2 — HEROISM: run to a fallen ally. A downed person (only players go Downed
    // today) can't save themselves, so a safe colonist sprints over; handle_deaths hauls up
    // anyone it reaches (kReviveDistance). The FIRST NPC behaviour about another *person*
    // rather than food or fear — the concrete seed of the design's PROTECT stance. Reuses the
    // same "nearest X in radius, steer toward" shape as foraging, and outranks it: you drop
    // what you're doing to save someone.
    // BRAVERY reads a SECOND time here (the same value used for the flee radius above), and this
    // is the self-preservation the flee comment used to promise: it scales how far an NPC will
    // COMMIT to a rescue. A brave colonist crosses the field to save someone (radius grows); a
    // coward won't make the risky trek and only helps an ally close by (radius shrinks). Note the
    // sign is OPPOSITE the flee radius — there, higher bravery SHRINKS the radius (holds ground);
    // here it GROWS it (commits further) — so on both rungs "braver" is the courageous choice.
    // Neutral 0 = kRescueRadius exactly (bit-identical). ponytail: still no *en-route* danger
    // check (distance is the risk proxy); a hazard-aware path cost is the richer future version.
    entt::entity fallen = entt::null;
    float nearest_fallen = kRescueRadius * (1.0f + bravery / 200.0f);
    for (const entt::entity f : downed) {
      // GRUDGE: a colonist won't cross the field to save someone it resents (affinity at or below
      // kGrudgeThreshold — e.g. a player who struck it). The abandonment half of the relationships
      // reader, the mirror of the bond-pull; neutral/liked fallen are unaffected (0 > threshold).
      if (affinity_toward(reg, n, f) <= kGrudgeThreshold) continue;
      const float d = glm::distance(pos, downed.get<Transform>(f).position);
      if (d < nearest_fallen) {
        nearest_fallen = d;
        fallen = f;
      }
    }
    if (fallen != entt::null) {
      const Vec2 toward = downed.get<Transform>(fallen).position - pos;
      const float len = glm::length(toward);
      // Only steer while still OUTSIDE revive range: an NPC already close enough must HOLD, or
      // it could nudge itself back out of range before handle_deaths (later this tick) revives.
      if (len > kReviveDistance) {
        // COMPASSION (the third axis) scales rescue SPEED — a THIRD knob-shape, distinct from
        // bravery's rescue RADIUS above: bravery decides WHETHER to cross the field, compassion
        // HOW URGENTLY once committed. The compassionate sprint to the fallen; the callous trudge
        // and, at the low end, physically can't beat the ~5s Downed timer. Neutral 0 -> unchanged
        // (bit-identical). Reuses the `pers` already fetched; int8 cast to float before the divide.
        const float compassion = pers != nullptr ? static_cast<float>(pers->compassion) : 0.0f;
        npcs.get<Velocity>(n).value =
            (toward / len) * kRescueSpeed * move_scale * (1.0f + compassion / 200.0f);
      }
      continue;  // committed to the rescue — don't also forage
    }

    // Priority 3 — hunger: a safe but hungry colonist seeks the nearest orb (its FIRST
    // want-driven motion; until now NPCs only ever fled). A fed one, or one with nothing in
    // range, FALLS THROUGH to arming up. It eats when it arrives, in collect_pickups.
    // GREED (the second personality axis) shifts the forage THRESHOLD — a differently-shaped
    // read than bravery's radius. A greedy colonist (reusing the `pers` fetched above) treats a
    // higher fraction as "hungry", so it breaks off to hoard an orb while still well-fed; a
    // selfless one lowers the bar and only forages when genuinely hungry, leaving food for
    // others. Neutral 0 -> kHungerSeekFraction exactly (bit-identical). Range [-100,+100] keeps
    // the fraction in [0.3, 0.9] — always < 1, so even a greedy NPC at FULL hunger isn't hungry.
    const float greed = pers != nullptr ? static_cast<float>(pers->greed) : 0.0f;
    const float seek_fraction = kHungerSeekFraction * (1.0f + greed / 200.0f);
    const Stats* stats = reg.try_get<Stats>(n);
    const bool hungry =
        stats != nullptr && stats->hunger.current < stats->hunger.max * seek_fraction;
    if (hungry) {
      // Head for the nearest FOOD — a scattered loot orb OR a fixed food plot with stock left,
      // whichever is closer. Track the target POSITION (not entity) so the two kinds compete on one
      // ruler; a bare plot (stock 0) isn't worth the walk, so it's skipped. Orbs are eaten in
      // collect_pickups, plots grazed in graze.
      Vec2 meal_pos{0.0f, 0.0f};
      bool has_meal = false;
      float nearest_food = kForageRadius;
      for (const entt::entity f : food) {
        const float d = glm::distance(pos, food.get<Transform>(f).position);
        if (d < nearest_food) {
          nearest_food = d;
          meal_pos = food.get<Transform>(f).position;
          has_meal = true;
        }
      }
      for (const entt::entity pl : plots) {
        if (plots.get<FoodSource>(pl).stock <= 0.0f) continue;  // a picked-bare patch isn't a meal
        const float d = glm::distance(pos, plots.get<Transform>(pl).position);
        if (d < nearest_food) {
          nearest_food = d;
          meal_pos = plots.get<Transform>(pl).position;
          has_meal = true;
        }
      }
      if (has_meal) {
        const Vec2 toward = meal_pos - pos;
        const float len = glm::length(toward);
        if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kForageSpeed * move_scale;
        continue;  // heading for a meal — don't also go weapon-hunting
      }
    }

    // Priority 3.5 — thirst: a safe NPC running low on water heads for the nearest WaterSource (the
    // drink system tops it up on arrival). Ranks just below hunger — both are survival needs; if
    // BOTH bite the same tick it forages first and seeks water the next. Reuses the `stats` fetched
    // for the hunger rung; a plain need threshold (no personality read yet, unlike greed on
    // forage).
    if (stats != nullptr && stats->water.current < stats->water.max * kThirstSeekFraction) {
      entt::entity well = entt::null;
      float nearest_water = kWaterSeekRadius;
      for (const entt::entity w : sources) {
        const float d = glm::distance(pos, sources.get<Transform>(w).position);
        if (d < nearest_water) {
          nearest_water = d;
          well = w;
        }
      }
      if (well != entt::null) {
        const Vec2 toward = sources.get<Transform>(well).position - pos;
        const float len = glm::length(toward);
        if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kWaterSeekSpeed * move_scale;
        continue;  // heading for a drink — don't also go weapon-hunting
      }
    }

    // Priority 4 — arm up: an UNARMED colonist (no rescue to make, not hungry, or no food in
    // range) walks to the nearest dropped weapon. npc_equip wields it on reach. An armed NPC
    // skips this. A match now `continue`s so the rally rung below is reached only by a truly idle
    // colonist (armed, or with no blade in range).
    if (gear == nullptr) {
      // INDUSTRY (the fourth axis) scales the arm-up seek RADIUS — the LAST unpersonalized rung, so
      // now every want in the ladder bends to who the colonist is. Reuses bravery's radius SHAPE
      // (not a new mechanism) on a new want: the industrious range across the field to loot a
      // weapon and better their kit; the idle only grab one practically underfoot. Neutral 0 ->
      // kWeaponSeekRadius exactly (bit-identical). Reuses the `pers` already fetched; cast to float
      // before the divide (-Wconversion).
      const float industry = pers != nullptr ? static_cast<float>(pers->industry) : 0.0f;
      entt::entity blade = entt::null;
      float nearest_blade = kWeaponSeekRadius * (1.0f + industry / 200.0f);
      for (const entt::entity w : weapons) {
        const float d = glm::distance(pos, weapons.get<Transform>(w).position);
        if (d < nearest_blade) {
          nearest_blade = d;
          blade = w;
        }
      }
      if (blade != entt::null) {
        const Vec2 toward = weapons.get<Transform>(blade).position - pos;
        const float len = glm::length(toward);
        if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kWeaponSeekSpeed;
        continue;  // heading for a weapon — an idle colonist would have fallen through to rally
      }
    }

    // Priority 5 — RALLY: the hero twin of the villain-fear at the top of the ladder. Reached only
    // by a truly IDLE colonist (nothing to flee, rescue, forage, drink, or arm toward), it drifts
    // toward a nearby renowned-enough hero — a player whose deeds have earned standing at or above
    // the "Known" line (+kKnownAt), the exact mirror of the -kKnownAt villain it flees at the top.
    // The colony gathers around its champion. INVERTED from fear (toward, not away) and LOWEST
    // priority (never overrides a real need — a hungry or endangered colonist ignores the hero).
    // Player-only (only a player earns standing today); no ledger or standing < kKnownAt -> no
    // pull, so a neutral/villain player draws nobody and the pre-hero world is bit-identical.
    //
    // SOCIABILITY (the fifth axis) scales the rally RADIUS, reusing industry's radius SHAPE on this
    // social want — so the rally rung, like every other acting rung, now reads a trait. A sociable
    // colonist (+) crosses the field to gather round a champion; a loner (-) rallies only to one
    // practically underfoot. Neutral 0 -> kRallyRadius exactly (bit-identical). Reuses the `pers`
    // fetched at the top of the loop; cast to float before the divide (-Wconversion).
    const float sociability = pers != nullptr ? static_cast<float>(pers->sociability) : 0.0f;
    entt::entity champion = entt::null;
    float nearest_hero = kRallyRadius * (1.0f + sociability / 200.0f);
    auto heroes = reg.view<PlayerControlled, Transform>();
    for (const entt::entity h : heroes) {
      const BehaviorLedger* led = reg.try_get<BehaviorLedger>(h);
      if (led == nullptr || standing(*led) < kKnownAt) continue;  // not a hero worth rallying to
      const float d = glm::distance(pos, heroes.get<Transform>(h).position);
      if (d < nearest_hero) {
        nearest_hero = d;
        champion = h;
      }
    }
    if (champion != entt::null) {
      const Vec2 toward = heroes.get<Transform>(champion).position - pos;
      const float len = glm::length(toward);
      if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kRallySpeed * move_scale;
      continue;  // gathered to the public hero — the personal-bond pull below is the fallback
    }

    // Priority 6 — BOND: the PERSONAL twin of the hero-rally, the first reader of the P8
    // relationships seed. Reached only when no public hero claimed the rung, so the hero-rally
    // above is byte-identical. An idle colonist with a positive-affinity tie (e.g. the player it
    // rescued) drifts toward its nearest well-liked friend — the colony clusters by BOND, not only
    // around the famous. Reuses the rally toward-vector + speed verbatim, reading affinity instead
    // of standing. MUST gate on reg.valid(other): edges store entity handles by value and ids
    // recycle, so a stale tie to a dead/reused entity is skipped, never dereferenced. No
    // Relationships -> the whole block is skipped -> the pre-relationships world is bit-identical.
    // LOYALTY (the sixth and last Personality axis) scales the bond RADIUS, the identical
    // trait-scaled-radius shape sociability gives the hero rung: a loyal colonist (+) crosses the
    // field to stay near a bonded ally, a fickle one (-) follows only a friend practically
    // underfoot. Neutral 0 (or no Personality) -> kBondRadius exactly (bit-identical). Reuses the
    // `pers` fetched at the top of the loop; cast to float before the divide (-Wconversion). With
    // this, EVERY acting steer rung reads a trait, and all six axes are wired.
    const float loyalty = pers != nullptr ? static_cast<float>(pers->loyalty) : 0.0f;
    if (const Relationships* rel = reg.try_get<Relationships>(n)) {
      entt::entity friend_e = entt::null;
      Vec2 friend_pos{0.0f, 0.0f};
      float nearest_friend = kBondRadius * (1.0f + loyalty / 200.0f);
      for (const Relation& edge : rel->edges) {
        if (edge.affinity < kBondPull || !reg.valid(edge.other))
          continue;  // weak tie, or stale handle
        const Transform* t = reg.try_get<Transform>(edge.other);
        if (t == nullptr)
          continue;  // a friend with no position (shouldn't happen, but cheap to guard)
        const float d = glm::distance(pos, t->position);
        if (d < nearest_friend) {
          nearest_friend = d;
          friend_e = edge.other;
          friend_pos = t->position;
        }
      }
      if (friend_e != entt::null) {
        const Vec2 toward = friend_pos - pos;
        const float len = glm::length(toward);
        if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kRallySpeed * move_scale;
      }
    }
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
  // exclude<Downed> too: a crumpled body is NOT prey — a creature ignores it and hunts whoever
  // is still standing, so the fight moves on rather than a creature pointlessly camping a corpse.
  // This is the shared "a Downed body is inert" invariant (regenerate_vitals, collect_pickups,
  // and handle_deaths' rescuers already hold it); the two combat-damage views below match.
  auto prey = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
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

// Move one vital toward its cap at its own rate, optionally sped by `boost` (a >=1
// multiplier from an attribute — e.g. VIT quickening health regen, mirroring how
// update_stamina speeds stamina recovery). Default 1.0 leaves the base rate untouched.
// Splitting this out keeps regenerate_vitals a single line per stat.
void recover(Vital& v, float dt, float boost = 1.0f) {
  v.current += v.regen_per_second * boost * dt;
  if (v.current > v.max) v.current = v.max;  // never past the cap
}

}  // namespace

void regenerate_vitals(entt::registry& reg, float dt) {
  // Each Endurance level past the first speeds HEALTH regen by this fraction — the mirror
  // of update_stamina's kRecoveryPerEndurance for stamina, completing VIT's "governs the
  // resources' capacity AND regen" role (it already grows the pool via advance_progression).
  // Kept a SEPARATE knob from stamina's so HP and stamina sustain tune independently.
  constexpr float kHealthRegenPerEndurance = 0.10f;  // ponytail: playtest value

  // view<Stats>() iterates exactly the entities that have stats — the player
  // here, not the drifting motes — so this can't touch anything without them.
  // That automatic filtering is the ECS's whole point: behaviour applies to
  // whoever has the right data, nobody else. A DOWNED player is excluded: they
  // lie at 0 HP for the whole helpless window, so a trickle of self-heal must not
  // quietly lift them off the floor — only a rescue or respawn brings them back.
  auto view = reg.view<Stats>(entt::exclude<Downed>);
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);

    // No mending on an empty stomach. This gate is load-bearing: drain_hunger runs BEFORE
    // this system in step(), so hunger.current is already this tick's value. Skipping heal
    // while starving is what makes "starvation always nets health DOWNWARD" a structural
    // guarantee rather than fragile arithmetic — it holds at ANY regen rate, so speeding
    // regen below can't accidentally out-pace starvation. ponytail/BALANCE: this deepens
    // starvation (a wounded starver no longer claws back the +regen), deliberately — and it
    // applies to NPCs too (parity: they run the identical rules), who lack the player's
    // Downed/revive net, so a colonist that can't reach food permadies a bit sooner. If that
    // reads too harsh in play, the knobs are food-drop rate, drain rate, and NPC regen — not a
    // special-case here (special-casing NPCs would break the player==NPC parity that's the point).
    // ...nor on an empty canteen: dehydration gates healing the same way starvation does, so both
    // needs net health strictly downward (drain_water also runs before this system in step()). One
    // `||` clause, so the two survival needs compose rather than either special-casing the other.
    // ...nor while POISONED: venom SUPPRESSES healing (the classic poison rule), so its chip lands
    // in full instead of being cancelled by a fed character's regen — that's what makes the
    // lingering bite a real threat. tick_poison runs before this system and reaps expired Poisoned,
    // so the flag here means active venom. A tougher character resists via its bigger HP POOL
    // (VIT), not regen.
    if (s.hunger.current <= 0.0f || s.water.current <= 0.0f || reg.all_of<Poisoned>(e)) continue;

    // Tougher characters heal faster (VIT). No Attributes -> boost 1.0 (bit-identical to
    // before), so creatures and bare entities are unchanged. Same shape as update_stamina.
    const Attributes* attrs = reg.try_get<Attributes>(e);
    const float boost = attrs != nullptr ? 1.0f + static_cast<float>(attrs->endurance.level - 1) *
                                                      kHealthRegenPerEndurance
                                         : 1.0f;
    recover(s.health, dt, boost);
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
      float boost = attrs != nullptr ? 1.0f + static_cast<float>(attrs->endurance.level - 1) *
                                                  kRecoveryPerEndurance
                                     : 1.0f;
      // Worn armour's BANE: plate slows your second wind. A fraction of recovery is lost while
      // armoured. ponytail/BALANCE: this bites only while RESTING (combat is spent moving), so
      // armour can feel near-free in a straight fight — a tuning knob; if it plays as pure
      // upside, move the bane onto the drain-while-moving side instead. No/empty armour = 0.
      const Equipped* eq = reg.try_get<Equipped>(e);
      if (eq != nullptr) boost *= 1.0f - eq->stamina_regen_penalty;
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
  // Health lost per second once hunger hits 0. It no longer needs to out-race the self-heal:
  // regenerate_vitals GATES healing off while starving (hunger <= 0), so starvation nets a
  // character's health strictly downward at any regen rate — a structural guarantee, not the
  // old fragile "12 must stay above 8". This value now just sets how FAST starvation kills.
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

void drain_water(entt::registry& reg, float dt) {
  // The twin of drain_hunger for the second Need — same moving/idle drain split, same 0-empties-
  // then-chips-health shape, independent knobs. Water is refilled at a WaterSource (drink), never
  // by resting, so like hunger its regen is 0 and this system only ever takes it away.
  constexpr float kDrainPerSecond = 0.3f;          // at rest
  constexpr float kExertionDrainPerSecond = 0.3f;  // added while moving
  constexpr float kDehydrationPerSecond = 12.0f;   // health lost per second once water hits 0
  // Same set as hunger: every person (Stats without Enemy), not creatures. regenerate_vitals gates
  // healing off while water is 0 (as it does for hunger), so dehydration nets health strictly down.
  auto view = reg.view<Stats>(entt::exclude<Enemy>);
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);
    float drain = kDrainPerSecond;
    if (const Velocity* v = reg.try_get<Velocity>(e);
        v != nullptr && glm::length(v->value) > 0.0f) {
      drain += kExertionDrainPerSecond;  // moving costs extra
    }
    s.water.current -= drain * dt;
    if (s.water.current < 0.0f) s.water.current = 0.0f;  // never below empty

    if (s.water.current <= 0.0f) {  // dehydrating — the same death path as starving
      s.health.current -= kDehydrationPerSecond * dt;
      if (s.health.current < 0.0f) s.health.current = 0.0f;
    }
  }
}

void drink(entt::registry& reg, float dt) {
  // How fast water refills while standing in a source — quick, so a few seconds at the well tops
  // you off, but not instant, so you must LINGER (the design's "walk to the well" spatial loop). A
  // knob.
  constexpr float kDrinkPerSecond = 40.0f;
  // Drinkers: any person (Stats + Transform) that isn't a creature or a downed body — the same
  // exclude<Enemy, Downed> as collect_pickups, so an unconscious body doesn't drink and creatures
  // (which never thirst) are skipped. The source is NOT consumed, so many can share one well.
  auto drinkers = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
  auto sources = reg.view<WaterSource, Transform>();
  for (const entt::entity d : drinkers) {
    Vital& water = drinkers.get<Stats>(d).water;
    if (water.current >= water.max) continue;  // already full — nothing to draw
    const Vec2 pos = drinkers.get<Transform>(d).position;
    for (const entt::entity s : sources) {
      if (glm::distance(pos, sources.get<Transform>(s).position) <=
          sources.get<WaterSource>(s).radius) {
        water.current += kDrinkPerSecond * dt;
        if (water.current > water.max) water.current = water.max;  // capped, no over-fill
        break;                                                     // one source is enough
      }
    }
  }
}

void graze(entt::registry& reg, float dt) {
  // Hunger restored per second while grazing — and the stock the plot loses to give it. Fast enough
  // that a few seconds at a patch fills you, so it's a real "walk to the garden" destination. A
  // knob.
  constexpr float kFeedPerSecond = 40.0f;
  // The food twin of drink, with one difference: the plot is FINITE. Each tick every plot first
  // REGROWS a little (capped at max_stock), then feeds nearby hungry people, DRAWING DOWN its stock
  // — so a picked-bare patch stops giving until it recovers. Eaters are the same set as drink/eat
  // (Stats + Transform, not creatures or downed bodies). Feeding order follows the view, so a small
  // plot feeds whoever the view reaches first until it runs dry — deterministic contention.
  auto grazers = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
  auto plots = reg.view<FoodSource, Transform>();
  // ponytail: no per-grazer cap ACROSS plots, so a colonist standing in two OVERLAPPING plots would
  // eat from both this tick. Only one plot exists and plots don't overlap, so it's a non-issue
  // today; add a per-grazer "already ate this tick" set if overlapping crops ever ship.
  for (const entt::entity src : plots) {
    FoodSource& fs = plots.get<FoodSource>(src);
    fs.stock += fs.regrow_per_second * dt;                 // crops regrow...
    if (fs.stock > fs.max_stock) fs.stock = fs.max_stock;  // ...up to the plot's yield
    const Vec2 src_pos = plots.get<Transform>(src).position;
    for (const entt::entity e : grazers) {
      if (fs.stock <= 0.0f) break;  // the patch is bare — nothing left to give this tick
      Vital& hunger = grazers.get<Stats>(e).hunger;
      const float room = hunger.max - hunger.current;
      if (room <= 0.0f) continue;  // already full
      if (glm::distance(grazers.get<Transform>(e).position, src_pos) > fs.radius) continue;
      float bite = kFeedPerSecond * dt;  // ...but no more than the eater needs or the plot holds
      if (bite > room) bite = room;
      if (bite > fs.stock) bite = fs.stock;
      hunger.current += bite;
      fs.stock -= bite;
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

// --- Skill -> attribute XP routing (the P2 "main + contributions" model) ---
//
// A skill grants XP to its MAIN attribute in full, and to each CONTRIBUTOR attribute a
// small fraction — so "you are what you do" cross-pollinates: a striker slowly sharpens
// (Strength a lot, a little Dexterity), not just strengthens. The mapping is DATA (a table
// of plain structs), so the eventual JSON-modded skills and gear's "+skill/+aspect" bonuses
// hang off this same seam without touching the grant sites.

// Resolve an AttrId to the live attribute field. No `default:` on purpose: add an AttrId and
// -Wswitch flags every switch that forgot a case, at compile time.
Attribute& attr_ref(Attributes& a, AttrId id) {
  switch (id) {
    case AttrId::Endurance:
      return a.endurance;
    case AttrId::Strength:
      return a.strength;
    case AttrId::Dexterity:
      return a.dexterity;
    case AttrId::Luck:
      return a.luck;
  }
  return a.endurance;  // unreachable (switch is exhaustive) — silences control-reaches-end
}

struct Contribution {
  AttrId attr;
  Fixed weight;  // fraction of the skill's XP this attribute earns (main earns the full 1.0)
};
struct SkillDef {
  AttrId main;
  std::vector<Contribution> contribs;  // usually empty; the seam trees/aspects grow from
};

// The table: which attribute(s) each skill feeds. Hardcoded first (design: "JSON at the
// modding milestone"). Five skills are main-only — bit-identical to before this refactor.
// Striking seeds the first contributor: landing blows mostly builds Strength, and a little
// Dexterity ("a fighter learns footwork"). The 1/4 weight is a playtest knob.
const SkillDef& skill_def(SkillId id) {
  static const SkillDef kConditioning{AttrId::Endurance, {}};
  static const SkillDef kToughness{AttrId::Endurance, {}};
  static const SkillDef kStriking{AttrId::Strength, {{AttrId::Dexterity, Fixed::from_ratio(1, 4)}}};
  static const SkillDef kRecovery{AttrId::Endurance, {}};
  static const SkillDef kEvasion{AttrId::Dexterity, {}};
  static const SkillDef kScavenging{AttrId::Luck, {}};
  // Throwing is Striking's mirror: aim-led (Dexterity main) with a little hurl-power (Strength
  // 1/4), where Striking is power-led (Strength main) with a little footwork (Dexterity 1/4).
  static const SkillDef kThrowing{AttrId::Dexterity, {{AttrId::Strength, Fixed::from_ratio(1, 4)}}};
  switch (id) {
    case SkillId::Conditioning:
      return kConditioning;
    case SkillId::Toughness:
      return kToughness;
    case SkillId::Striking:
      return kStriking;
    case SkillId::Recovery:
      return kRecovery;
    case SkillId::Evasion:
      return kEvasion;
    case SkillId::Scavenging:
      return kScavenging;
    case SkillId::Throwing:
      return kThrowing;
  }
  return kConditioning;  // unreachable (exhaustive) — a new SkillId is caught by -Wswitch
}

// The Character Level earns a fixed FRACTION of every activity's base XP — a quarter — so
// it climbs slower than the skill it comes from and stays the gentle "veteran" layer, not a
// second fast track. A balance knob, not a law (design: tune at playtest). It rides the
// funnel below, so EVERY activity that grants skill XP now feeds it — movement and resting
// as before, and now every combat/loot source too (Toughness, Striking, Evasion,
// Scavenging) — which is what the design's "a fraction of ALL activity" always meant.
// (Fixed::from_ratio isn't constexpr, hence a file-scope const, not constexpr.)
const Fixed kCharLevelShare = Fixed::from_ratio(1, 4);

// THE one funnel every skill-XP grant flows through: train the skill, feed its main
// attribute the full amount, each contributor its fraction, and — when the entity carries a
// CharacterLevel — a fraction of the base to that global veteran layer. Replaces six
// copy-pasted "skill.xp += X; attr.xp += X" pairs, and centralises the char-level feed so it
// can never drift per-site again. `character` is nullable: a grant on an entity without one
// simply skips the char feed (harmless). `base` for main is a plain add (never `* 1.0`), so
// the main-only skills stay bit-for-bit as they were.
void grant_skill_xp(Skills& skills, Attributes& attrs, SkillId id, Fixed base,
                    CharacterLevel* character = nullptr) {
  const SkillDef& def = skill_def(id);
  skills.train(id).xp += base;
  attr_ref(attrs, def.main).xp += base;
  for (const Contribution& c : def.contribs) attr_ref(attrs, c.attr).xp += base * c.weight;
  if (character != nullptr) character->xp += base * kCharLevelShare;
}

}  // namespace

void advance_progression(entt::registry& reg) {
  constexpr float kHealthPerEndurance = 10.0f;  // how much tougher each point makes you
  constexpr float kStaminaPerEndurance = 5.0f;
  // XP earned per tick of activity. The timestep is fixed (1/60 s), so a
  // per-second rate is a constant per-tick Fixed amount — deterministic, no float
  // in the loop (20 XP/sec ÷ 60 ticks).
  const Fixed kConditioningPerTick = Fixed::from_ratio(20, 60);
  // Resting to recover spent stamina trains Recovery a touch slower than moving
  // trains Conditioning (15 XP/sec vs 20) — resting is easier than exerting.
  const Fixed kRecoveryPerTick = Fixed::from_ratio(15, 60);

  // NB: CharacterLevel is required here, so any new progression-capable entity must
  // be spawned WITH it (see world.cpp: player + NPC) — miss it and that entity
  // silently never grows, no error. Keep the progression components together.
  auto view = reg.view<Skills, Attributes, Stats, Velocity, CharacterLevel>();
  for (const entt::entity e : view) {
    Attributes& attrs = view.get<Attributes>(e);
    Attribute& endurance = attrs.endurance;
    CharacterLevel& character = view.get<CharacterLevel>(e);

    // 1. Activity earns XP for the SKILL and its attribute(s), plus a fraction to the
    //    global Character Level — all through grant_skill_xp (pass &character and it feeds
    //    the veteran layer in lockstep). Moving trains Conditioning; resting to recover
    //    *spent* stamina trains Recovery instead — both feed Endurance. Idle at full stamina,
    //    or with no spending to recover from, trains nothing. (Combat/loot sources feed the
    //    character level the same way, from their own grant sites.)
    if (glm::length(view.get<Velocity>(e).value) > 0.0f) {
      grant_skill_xp(view.get<Skills>(e), attrs, SkillId::Conditioning, kConditioningPerTick,
                     &character);
    } else if (const Vital& stamina = view.get<Stats>(e).stamina; stamina.current < stamina.max) {
      grant_skill_xp(view.get<Skills>(e), attrs, SkillId::Recovery, kRecoveryPerTick, &character);
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
    //    heal; regen fills the new room in. (The pools aren't its only expression: the
    //    same veteran multiplier scales earned combat Strength in perform_attack.)
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
  // Enduring a blow is activity too, so it feeds the Character Level like any other grant
  // (pass the victim's CharacterLevel through the funnel). advance_progression levels the
  // trained Toughness — and the character XP fed here — next tick.
  grant_skill_xp(*skills, *attrs, SkillId::Toughness, gained, reg.try_get<CharacterLevel>(victim));
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
  const float from_vit =
      a != nullptr ? static_cast<float>(a->endurance.level - 1) * kDefencePerVit : 0.0f;
  // Worn armour adds flat defence on top of VIT — the same number, so it feeds mitigate at
  // BOTH damage sites (perform_attack, resolve_creature_contacts) with no extra plumbing. No
  // Equipped (or an empty armour slot) contributes 0, so an unarmoured world is unchanged.
  const Equipped* eq = reg.try_get<Equipped>(e);
  return from_vit + (eq != nullptr ? eq->defence_bonus : 0.0f);
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

// Stamp a fresh hit-flash on an entity that just took a blow — presentation only, so
// the renderer can blink it white (see components.hpp HitFlash). emplace_or_replace so
// a rapid second blow refreshes the flash to full rather than stacking. Called AT the
// damage sites, unconditionally on a landed hit — no roll, so it draws no RNG and the
// seeded streams stay identical. Safe mid-view: HitFlash is in no view being walked at
// any call site (the same reason Downed is safely emplaced during handle_deaths).
void stamp_flash(entt::registry& reg, entt::entity e) {
  reg.emplace_or_replace<HitFlash>(e, HitFlash{kHitFlashSeconds});
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

  // Effective Strength = your trained level PLUS whatever weapon you're wielding (the
  // design's "gear grants raw +Attribute"). One number feeds BOTH reach and damage below,
  // so a heavier blade reaches further AND hits harder. Bare-handed (no Equipped) it is just
  // your level — so this is bit-identical for anyone not wielding anything.
  const Equipped* gear = reg.try_get<Equipped>(attacker);
  const int effective_strength =
      attrs->strength.level + (gear != nullptr ? gear->strength_bonus : 0);

  // The attacker's global "veteran" multiplier, POWER(level - 1) — the SAME curve and shape
  // advance_progression uses to compound the earned HP/stamina pools. It scales only what you
  // EARNED by grinding — the trained Strength levels — on damage below, so a fighter's grind
  // finally sharpens their blade, not just their bars. Level 1 — and an attacker with no
  // CharacterLevel — is POWER(0) = 1.0, so this is bit-identical until you actually level up.
  // Fetched once here (nullable) and reused at the grant site; no RNG, pure Fixed LUT math.
  CharacterLevel* character = reg.try_get<CharacterLevel>(attacker);
  const float veteran =
      static_cast<float>(power((character != nullptr ? character->level : 1) - 1).to_double());

  const Vec2 origin = tf->position;
  // Reach is left FLAT (no veteran factor): scaling it would change which target a swing can
  // even reach — target ACQUISITION — silently shifting who gets hit. A veteran hits harder,
  // not from further; keep acquisition stable.
  const float reach = kBaseReach + static_cast<float>(effective_strength - 1) * kReachPerStrength;

  // The raw damage of THIS swing, computed once for whoever it lands on (a hostile below, or a
  // colonist in the cruel-strike branch). base + earned-Strength delta (compounded by the veteran
  // multiplier) + the weapon's granted +Strength (flat). Only the XP-EARNED levels compound — gear
  // is loot, not grind, so it must NOT scale with an unrelated character level (that would make one
  // blade worth wildly more on a veteran). Keeps the "multiply only what you earned" invariant
  // honest and mirrors advance_progression, which scales the earned endurance.level - 1 alone.
  const int gear_strength = gear != nullptr ? gear->strength_bonus : 0;
  const float raw = kBaseAttackDamage +
                    static_cast<float>(attrs->strength.level - 1) * kDamagePerStrength * veteran +
                    static_cast<float>(gear_strength) * kDamagePerStrength;

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
  if (target == entt::null) {
    // Nothing hostile in reach. A PLAYER may still choose to swing at a peaceful colonist here — a
    // deliberate act of CRUELTY, the villain mirror of Valor and the first deed that DROPS
    // standing. Gated three ways so it reads as a choice, never a slip: (1) only a player swings
    // this way — NPCs never turn on the colony (npc_attack shares this function; an NPC-villain AI
    // is a later ring), (2) hostiles were searched first and always win the target, so you can
    // reach a colonist only with nothing else to fight, (3) it must be in reach. Downed bodies are
    // excluded — no infamy for kicking a corpse.
    if (reg.all_of<PlayerControlled>(attacker)) {
      entt::entity victim = entt::null;
      float nearest_npc = reach;
      auto colonists = reg.view<Npc, Transform>(entt::exclude<Downed>);
      for (const entt::entity c : colonists) {
        const float d = glm::distance(origin, colonists.get<Transform>(c).position);
        if (d < nearest_npc) {
          nearest_npc = d;
          victim = c;
        }
      }
      if (victim != entt::null) {
        // A cruel strike lands plainly — no dodge, no crit (an unsuspecting colonist doesn't slip a
        // betrayal, and there's no "lucky" cruelty), so this branch draws NO RNG and leaves the
        // seeded stream untouched. It still trains Striking (a swing that connects) and blinks the
        // victim like any other hit; the colonist takes the same STR-vs-VIT damage and
        // handle_deaths reaps it at 0 HP (NPC = permadeath — you can thin your own colony).
        grant_skill_xp(*skills, *attrs, SkillId::Striking, kStrikingPerHit, character);
        if (Stats* st = reg.try_get<Stats>(victim); st != nullptr) {
          st->health.current -= mitigate(raw, defence_of(reg, victim));
          if (st->health.current < 0.0f) st->health.current = 0.0f;
          stamp_flash(reg, victim);
        }
        record_deed(reg, attacker, Deed::Cruelty, kCrueltyStrike);
        // The betrayal is also PERSONAL: the struck colonist forms a GRUDGE toward the striker (a
        // negative affinity edge, the mirror of a rescue's bond). A resented player won't be
        // rescued by this colonist later — a targeted consequence that lands before global
        // villain-fear does.
        nudge_affinity(reg, victim, attacker, kCrueltyGrudge);
      }
    }
    return entt::null;  // a whiff (or the cruel strike above) — nothing to hand back to destroy
  }

  // A connecting strike trains Striking -> Strength (a lot) + Dexterity (a little), whatever
  // it hits: swinging a weapon mostly builds power, and a touch of the reflex behind it.
  grant_skill_xp(*skills, *attrs, SkillId::Striking, kStrikingPerHit, character);

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

  // An enemy takes STR-vs-VIT damage to its HP — the swing's `raw` (computed above), softened by
  // the enemy's VIT. It is NOT destroyed here; handle_deaths reaps it at 0 HP, so a weak hit only
  // chips it and it takes several. (An Enemy always carries Stats.)
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

  // EXECUTE: a creature already worn below kExecuteThreshold of its HP takes MORE from the
  // finishing blow — the offensive MIRROR of enrage (resolve_creature_contacts), which is keyed on
  // the SAME 0.3 fraction. Together they make a half-dead foe a sharp risk/reward: below 30% it
  // lashes out harder (enrage) but also folds faster (execute), so you commit to the finish or you
  // pay for leaving it there. A pure comparison on the target's CURRENT fraction before this blow
  // lands — no RNG — so a low creature dies a little sooner but replay stays bit-identical.
  constexpr float kExecuteThreshold =
      0.3f;                              // below this fraction of its HP, a foe is finishable...
  constexpr float kExecuteBonus = 1.5f;  // ...and the finishing blow lands this much harder
  if (Stats* st = reg.try_get<Stats>(target); st != nullptr) {
    const bool was_alive = st->health.current > 0.0f;
    const float dealt =
        st->health.current < st->health.max * kExecuteThreshold ? applied * kExecuteBonus : applied;
    st->health.current -= dealt;
    if (st->health.current < 0.0f) st->health.current = 0.0f;
    stamp_flash(reg, target);  // the struck target blinks white
    // The killing blow on a HOSTILE is a Valor deed for the attacker — the SECOND deed through the
    // morality write-point (after a rescue's Charity), proving it generalises across dimensions.
    // Credited only on the alive->dead TRANSITION, so a second swing landing on the same foe this
    // tick (before handle_deaths reaps it) can't double-claim the kill. Motes (Hazard) returned
    // earlier and peaceful NPCs are never targeted here, so only slaying a monster counts — and
    // NPCs earn it too (npc_attack shares this perform_attack): a colonist who fells a creature is
    // brave.
    if (was_alive && st->health.current <= 0.0f)
      record_deed(reg, attacker, Deed::Valor, kValorKill);
  }
  return entt::null;
}

void perform_throw(entt::registry& reg, entt::entity attacker) {
  // The player's RANGED option — hurl something at the nearest hostile far out of melee reach,
  // chipping an approaching swarm before it closes. Two things set it apart from a melee swing:
  //   - it draws NO RNG (no dodge, no crit, no execute) — a plain, reliable, MODEST hit, so ranged
  //     trades melee's burst potential for range and certainty; and
  //   - it COSTS STAMINA. A standing character out-regenerates a slow plink, but throwing fast, or
  //   on
  //     the move (kiting drains far more than regen), empties the bar and drops you to the same
  //     exhausted crawl a sprint causes (update_stamina's gate). So the cost keeps range honest:
  //     you can soften an approach but can't burst a swarm down or kite one forever for free.
  // The hit isn't dealt HERE — this LAUNCHES a homing Projectile that advance_projectiles flies to
  // the target and lands on arrival (so a throw has a visible travel time, and is wasted if the
  // target dies first). Player-only for now: there is no npc_throw, so NPCs still only melee (a
  // creature-ranged AI, reusing the same Projectile, is a later ring).
  constexpr float kThrowRange = 350.0f;       // vastly longer than a melee swing's ~45 reach
  constexpr float kThrowStaminaCost = 15.0f;  // each connecting throw spends this
  constexpr float kBaseThrowDamage = 8.0f;  // raw before Dexterity (weaker than a melee swing's 12)
  constexpr float kDamagePerDexterity = 3.0f;  // each Dexterity level past 1 sharpens the throw
  const Fixed kThrowingPerHit = Fixed::from_int(10);

  const Transform* tf = reg.try_get<Transform>(attacker);
  Attributes* attrs = reg.try_get<Attributes>(attacker);
  Skills* skills = reg.try_get<Skills>(attacker);
  Stats* stats = reg.try_get<Stats>(attacker);
  if (tf == nullptr || attrs == nullptr || skills == nullptr || stats == nullptr) return;

  // Nearest hostile in range — deterministic strict-< tie-break. Enemies only: a throw is an
  // anti-creature tool, so it never targets a peaceful colonist (villainy stays a deliberate MELEE
  // choice, perform_attack) nor a mote.
  const Vec2 origin = tf->position;
  entt::entity target = entt::null;
  float nearest = kThrowRange;
  auto enemies = reg.view<Enemy, Transform>();
  for (const entt::entity e : enemies) {
    const float d = glm::distance(origin, enemies.get<Transform>(e).position);
    if (d < nearest) {
      nearest = d;
      target = e;
    }
  }
  if (target == entt::null) return;  // nothing in range — a held throw: no stamina spent, no XP

  // Winding up needs stamina in hand — an exhausted thrower fizzles (nothing spent, no XP), the
  // ranged echo of the empty-stamina melee crawl. Strict <, so a throw at exactly the cost still
  // lands and a throw at exactly 0 can't.
  if (stats->stamina.current < kThrowStaminaCost) return;
  stats->stamina.current -= kThrowStaminaCost;

  // A connecting throw trains Throwing -> Dexterity (a lot) + Strength (a little) — the mirror of a
  // swing training Striking -> Strength + Dexterity.
  CharacterLevel* character = reg.try_get<CharacterLevel>(attacker);
  grant_skill_xp(*skills, *attrs, SkillId::Throwing, kThrowingPerHit, character);

  // DEX-vs-VIT damage: base + earned-Dexterity delta, softened by the target's VIT. No gear/veteran
  // scaling and no crit — a throw is deliberately the simple, reliable option.
  const float raw =
      kBaseThrowDamage + static_cast<float>(attrs->dexterity.level - 1) * kDamagePerDexterity;
  const float applied = mitigate(raw, defence_of(reg, target));

  // The hit isn't dealt here — instead we LAUNCH a homing Projectile carrying that (already
  // mitigated) damage from the thrower toward the target. advance_projectiles flies it there and
  // applies the blow (and the Valor credit) on arrival, so a throw now has a visible travel time
  // and is wasted if the target dies first. The projectile carries its own render dot, so the
  // renderer draws it with no extra plumbing.
  const entt::entity shot = reg.create();
  reg.emplace<Transform>(shot, origin);
  reg.emplace<PrevTransform>(shot, origin);
  reg.emplace<RenderDot>(shot, Vec3{1.0f, 0.95f, 0.4f}, 4.0f);  // a small bright-yellow bolt
  reg.emplace<Projectile>(shot, Projectile{target, attacker, applied, kProjectileSpeed});
}

void advance_projectiles(entt::registry& reg, float dt) {
  // Fly each in-flight shot toward its homing target; on arrival apply its carried damage and reap
  // the shot. Collect-then-destroy so no view is invalidated mid-iteration (and so an impact that
  // spawns nothing still safely removes the projectile after the loop).
  std::vector<entt::entity> spent;
  auto shots = reg.view<Projectile, Transform>();
  for (const entt::entity s : shots) {
    const Projectile& p = shots.get<Projectile>(s);
    // Target gone (reaped mid-flight, or never valid) -> the throw is wasted; drop the shot.
    const Transform* tgt = reg.valid(p.target) ? reg.try_get<Transform>(p.target) : nullptr;
    if (tgt == nullptr) {
      spent.push_back(s);
      continue;
    }
    Vec2& pos = shots.get<Transform>(s).position;
    const Vec2 to = tgt->position - pos;
    const float dist = glm::length(to);
    const float step = p.speed * dt;
    if (dist <= step || dist < kProjectileHitRadius) {
      // IMPACT — apply the carried (pre-mitigated) damage, exactly as the melee/throw damage sites
      // do: chip HP, blink the target, and credit the OWNER Valor on the alive->dead transition.
      if (Stats* st = reg.try_get<Stats>(p.target); st != nullptr) {
        const bool was_alive = st->health.current > 0.0f;
        st->health.current -= p.damage;
        if (st->health.current < 0.0f) st->health.current = 0.0f;
        stamp_flash(reg, p.target);
        // Valor is for felling a HOSTILE, so credit it only when the shot killed an Enemy (a
        // player's throw). A creature's spit homes on a PERSON, so this guard keeps a spitter from
        // ever earning Valor for killing a colonist (creatures have no morality anyway).
        if (was_alive && st->health.current <= 0.0f && reg.all_of<Enemy>(p.target) &&
            reg.valid(p.owner))
          record_deed(reg, p.owner, Deed::Valor, kValorKill);
      }
      spent.push_back(s);
    } else {
      pos += (to / dist) * step;  // home in (dist > 0 here, so the divide is safe)
    }
  }
  for (const entt::entity s : spent) reg.destroy(s);
}

void creature_spit(entt::registry& reg, float dt) {
  // The RANGED creature attack, the hostile mirror of the player's throw — and the payoff of the
  // Projectile primitive. A spitter (Enemy with spit_range > 0) periodically launches a homing spit
  // at the nearest PERSON within range, reusing the exact same Projectile advance_projectiles
  // flies. So a ranged enemy fell out of the primitive with no new movement/impact code — only the
  // launch.
  constexpr float kSpitInterval = 1.5f;  // seconds between a spitter's shots (a knob)

  // Prey = people (Stats, not a creature, not downed) — the same set chase_prey homes on. A downed
  // body is spared (nothing to gain shooting a helpless target; matches the melee rule).
  auto prey = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);

  // Collect the shots to launch, THEN spawn them, so we never reg.create() while iterating the
  // Enemy/Transform view (the new projectile carries a Transform, a pool the view touches).
  struct Shot {
    Vec2 from;
    entt::entity target;
    entt::entity owner;
    float damage;
  };
  std::vector<Shot> shots;
  auto spitters = reg.view<Enemy, Transform>();
  for (const entt::entity e : spitters) {
    Enemy& enemy = spitters.get<Enemy>(e);
    if (enemy.spit_range <= 0.0f) continue;  // melee-only creatures never spit
    if (enemy.spit_timer > 0.0f) {
      enemy.spit_timer -= dt;  // still reloading
      continue;
    }
    // Loaded — find the nearest person in range. Strict < breaks ties deterministically.
    const Vec2 pos = spitters.get<Transform>(e).position;
    entt::entity victim = entt::null;
    float nearest = enemy.spit_range;
    for (const entt::entity person : prey) {
      const float d = glm::distance(pos, prey.get<Transform>(person).position);
      if (d < nearest) {
        nearest = d;
        victim = person;
      }
    }
    if (victim == entt::null) continue;  // no one in range — hold fire, stay loaded

    // Fire: reset the reload and queue a spit carrying VIT-mitigated damage (mitigated once at
    // launch, like the player's throw — the projectile then just delivers it).
    enemy.spit_timer = kSpitInterval;
    shots.push_back({pos, victim, e, mitigate(enemy.spit_damage, defence_of(reg, victim))});
  }

  // Launch the queued spits. A violet bolt, distinct from the player's bright-yellow throw, so
  // incoming fire reads at a glance.
  for (const Shot& s : shots) {
    const entt::entity bolt = reg.create();
    reg.emplace<Transform>(bolt, s.from);
    reg.emplace<PrevTransform>(bolt, s.from);
    reg.emplace<RenderDot>(bolt, Vec3{0.8f, 0.3f, 0.85f}, 4.0f);
    reg.emplace<Projectile>(bolt, Projectile{s.target, s.owner, s.damage, kProjectileSpeed});
  }
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

entt::entity equip_nearest_gear(entt::registry& reg, entt::entity wearer) {
  // Wear the nearest dropped GEAR within reach of `wearer` — a Weapon OR a piece of Armour,
  // whichever is closer — folding its mods into the matching SLOT of an Equipped cache and
  // RETURNing the item for the caller to destroy (collect-then-destroy, so no view is
  // invalidated mid-iteration). entt::null if none in reach. THE one place gear mods are
  // folded, shared by the player's Equip command and the npc_equip system, so a player and an
  // NPC can never gear up differently. The fold is NON-CLOBBERING: get_or_emplace keeps the
  // existing cache and each branch writes ONLY its own slot's pair, so grabbing armour leaves a
  // wielded weapon intact and vice-versa (two independent slots).
  constexpr float kEquipReach = 30.0f;  // a bit past contact — step near and grab it
  const Vec2 pos = reg.get<Transform>(wearer).position;
  entt::entity nearest = entt::null;
  bool nearest_is_weapon = false;
  float best = kEquipReach;  // ONE shared best, so the nearer of a weapon vs an armour wins
  auto weapons = reg.view<Weapon, Transform>();
  for (const entt::entity w : weapons) {
    const float d = glm::distance(pos, weapons.get<Transform>(w).position);
    if (d < best) {
      best = d;
      nearest = w;
      nearest_is_weapon = true;
    }
  }
  auto armours = reg.view<Armour, Transform>();
  for (const entt::entity a : armours) {
    const float d = glm::distance(pos, armours.get<Transform>(a).position);
    if (d < best) {
      best = d;
      nearest = a;
      nearest_is_weapon = false;
    }
  }
  if (nearest == entt::null) return entt::null;

  Equipped& eq = reg.get_or_emplace<Equipped>(wearer);  // keep the other slot; write only this one
  if (nearest_is_weapon) {
    const Weapon& wpn = weapons.get<Weapon>(nearest);
    eq.strength_bonus = wpn.strength_bonus;
    eq.move_penalty = wpn.move_penalty;
  } else {
    const Armour& arm = armours.get<Armour>(nearest);
    eq.defence_bonus = arm.defence_bonus;
    eq.stamina_regen_penalty = arm.stamina_regen_penalty;
  }
  return nearest;
}

void npc_equip(entt::registry& reg) {
  // Colonists gear up from the battlefield: an UNGEARED NPC within reach of dropped gear (a
  // weapon or a piece of armour) grabs it — the auto-equip-on-reach mirror of collect_pickups
  // eating an orb, and the parity twin of the player's Equip command (both fold through
  // equip_nearest_gear). steer_npcs walks the unarmed ones toward the nearest blade; this is
  // where they grab whatever gear they've reached. Emplacing Equipped is safe here — it isn't
  // part of the view<Npc, Transform> being iterated. Collect-then-destroy.
  // ponytail: the guard skips an NPC that has ANY Equipped, so an NPC grabs the FIRST gear it
  // reaches and stops — it won't hunt down the second slot (NPC gear-depth is a later
  // increment; the shared fold already gives player==NPC parity for whatever they do grab).
  // Two NPCs reaching one item the same tick both grab from it (consumed once) — a rare
  // harmless dupe, the same shape collect_pickups tolerates.
  std::vector<entt::entity> taken;
  auto npcs = reg.view<Npc, Transform>();
  for (const entt::entity n : npcs) {
    if (reg.all_of<Equipped>(n)) continue;  // already geared — grabs one piece then stops
    const entt::entity w = equip_nearest_gear(reg, n);
    if (w != entt::null) taken.push_back(w);
  }
  for (const entt::entity w : taken) {
    if (reg.valid(w)) reg.destroy(w);
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

// Spawn a Weapon on the ground at `pos` — the ONE canonical dropped-weapon entity (a
// steel-grey dot + Weapon), so a slain BRUTE's drop (handle_deaths) and a player's Drop
// command produce an identical, re-wieldable pickup. Public so both callers share this one
// definition of what a grounded weapon looks like (no drift). It spawns the DEFAULT Weapon;
// that is lossless today because the only Equipped mods come from this same default — when
// more than one Weapon def exists, Drop must pass the wielder's cached mods instead.
// ponytail: no lifetime — a dropped weapon persists (brutes are the rarer kill, so they don't
// pile up); add a fade like Pickup's if the field ever litters.
void spawn_weapon(entt::registry& reg, Vec2 pos) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.75f, 0.78f, 0.85f}, 6.0f);  // steel grey, a touch bigger
  reg.emplace<Weapon>(e);
}

// Spawn a piece of Armour on the ground at `pos` — the canonical grounded-armour entity, the
// defensive counterpart of spawn_weapon. A distinct render colour (dull bronze) so you can
// tell armour from a weapon on the field at a glance. Step near and press E to don it.
void spawn_armour(entt::registry& reg, Vec2 pos) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.72f, 0.52f, 0.24f}, 6.0f);  // dull bronze, distinct from steel
  reg.emplace<Armour>(e);
}

void record_deed(entt::registry& reg, entt::entity actor, Deed kind, std::int32_t mag) {
  // The whole morality write-path, one line: lazily give the actor a ledger on its first deed, then
  // add the magnitude to the chosen dimension. No switch — every dimension accrues identically, so
  // the array index does the routing a switch otherwise would (and a new Deed can't be forgotten,
  // it just indexes a fresh slot). get_or_emplace keeps a never-acting entity ledger-free, so the
  // absent-ledger world is bit-identical to before morality existed. Touches no registry view, so
  // it is safe to call mid-iteration (as handle_deaths does).
  reg.get_or_emplace<BehaviorLedger>(actor).dims[static_cast<std::size_t>(kind)] += mag;

  // DRIFT: a deed also nudges the actor's matching PERSONALITY axis a bounded step — the design's
  // "you are what you do" made concrete, the bridge between the two P7 halves (the earned ledger
  // reshapes the innate leaning). Fighting monsters hardens you (Valor -> bravery); hauling up the
  // fallen softens you (Charity -> compassion). And because bravery is both the TINTED axis and the
  // one steer_npcs reads twice, a fighter visibly warms and holds its ground — a character arc from
  // deeds alone. `try_get`, NEVER get_or_emplace: an entity with no Personality (the player, every
  // creature) must STAY Personality-free, or the bit-identical absent-Personality world breaks.
  // Only the two wired deeds drift; the other four wire themselves the day their deeds land. Pure
  // integer math (no RNG), clamped in int before the int8 cast so a long career can't overflow.
  if (Personality* p = reg.try_get<Personality>(actor)) {
    std::int8_t* axis = nullptr;
    if (kind == Deed::Valor) {
      axis = &p->bravery;
    } else if (kind == Deed::Charity) {
      axis = &p->compassion;
    }
    if (axis != nullptr) {
      int v = static_cast<int>(*axis) + kDeedDriftStep;
      if (v > 100) v = 100;
      if (v < -100) v = -100;  // symmetric clamp — drift is positive today, ready for future deeds
      *axis = static_cast<std::int8_t>(v);
    }
  }
}

void nudge_affinity(entt::registry& reg, entt::entity from, entt::entity toward,
                    std::int8_t delta) {
  // The whole RELATIONSHIPS write-path, the record_deed twin: lazily give `from` a Relationships on
  // its first bond, then find-or-update its directed edge toward `toward` — deepen an existing tie,
  // or append a new one — so the edge count is the number of DISTINCT partners, never per-tick
  // growth. get_or_emplace keeps a never-bonding entity edge-free (the absent-Relationships world
  // is bit-identical). Touches no registry view (get_or_emplace + a vector write), so it is safe to
  // call mid-iteration, exactly like record_deed.
  //
  // Clamp to ±100 in ONE place — BOTH the deepen and the first-append path — so the band invariant
  // holds no matter which branch stores (a future event may nudge past 100 the first time). int8
  // saturates into bond bands; it doesn't accumulate over a life toward a gate the way ledger dims
  // do, so a bounded store is the whole point (the derived bond_tier will assume it).
  const auto clamp = [](int v) {
    return static_cast<std::int8_t>(v > 100 ? 100 : (v < -100 ? -100 : v));
  };
  auto& rel = reg.get_or_emplace<Relationships>(from);
  for (Relation& e : rel.edges) {
    if (e.other == toward) {  // deepen an existing tie
      e.affinity = clamp(static_cast<int>(e.affinity) + static_cast<int>(delta));
      return;
    }
  }
  rel.edges.push_back(Relation{toward, clamp(static_cast<int>(delta))});  // first tie to `toward`
}

std::int8_t affinity_toward(const entt::registry& reg, entt::entity from, entt::entity toward) {
  // The READER counterpart of nudge_affinity — how `from` feels about `toward`, or 0 (neutral) if
  // there's no tie. Sparse-lazy: an entity that never bonded has no Relationships, so this is a
  // single null-check for the common case. A plain linear scan (edges are few). Const registry: it
  // never mutates, so both steer_npcs and handle_deaths can call it while iterating.
  if (const Relationships* rel = reg.try_get<Relationships>(from)) {
    for (const Relation& e : rel->edges) {
      if (e.other == toward) return e.affinity;
    }
  }
  return 0;
}

void handle_deaths(entt::registry& reg, Vec2 respawn_point, float dt) {
  // A zero-health entity meets one of two fates, and which one is the game's core rule
  // made concrete: a PLAYER goes DOWNED (helpless where they fell, then rescued-in-place
  // by an ally or respawned when the timer runs out); an NPC dies for good (permadeath).
  // (kReviveDistance is the file-scope constant steer_npcs also runs rescuers toward.)

  // Back on your feet WHOLE — every vital, not just health. Hunger AND water especially must reset:
  // neither self-recovers, so reviving with an empty stomach (or a dry canteen) would re-starve (or
  // re-dehydrate) the player and drop them straight back down. Stamina too, so you don't get up
  // mid-exhaustion. Active VENOM is cleared as well — else a rescue would hand you back a body that
  // can't heal (poison gates regen), re-endangering you. Any Need/lethal status added later must
  // reset HERE for the same reason.
  const auto revive = [&reg](entt::entity e, Stats& s, Velocity& v) {
    s.health.current = s.health.max;
    s.stamina.current = s.stamina.max;
    s.hunger.current = s.hunger.max;
    s.water.current = s.water.max;
    reg.remove<Poisoned>(e);  // no lingering lethal status through a revive
    v.value = Vec2{0.0f, 0.0f};
  };

  // Adding/removing Downed doesn't invalidate this view (Downed isn't one of its
  // components), so mutating it while iterating is safe.
  auto players = reg.view<Stats, PlayerControlled, Transform, Velocity>();
  for (const entt::entity e : players) {
    Stats& s = players.get<Stats>(e);
    Downed* down = reg.try_get<Downed>(e);

    if (down == nullptr) {
      if (s.health.current > 0.0f) continue;  // alive and standing — nothing to do
      // Just fell: crumple WHERE you are, helpless. NO heal, NO teleport — that free
      // escape-to-safety was the old anti-climax; now you have to survive the window.
      reg.emplace<Downed>(e);
      reg.remove<Blocking>(e);  // a crumpled body isn't guarding — drop the stance (inert body)
      players.get<Velocity>(e).value = Vec2{0.0f, 0.0f};
      continue;
    }

    // Already downed. A living ally (any non-downed person) within reach hauls you up where
    // you fell — the co-op rescue. NPCs don't SEEK the downed yet, so today this fires when
    // one happens by; the forage "seek nearest X" pattern is the seed of making it deliberate.
    entt::entity rescuer = entt::null;  // the ally who reaches them — and the deed's actor
    const Vec2 pos = players.get<Transform>(e).position;
    auto allies = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
    for (const entt::entity a : allies) {
      if (a == e || allies.get<Stats>(a).health.current <= 0.0f)
        continue;  // self / a corpse can't help
      // GRUDGE: an ally that resents the fallen (affinity <= kGrudgeThreshold, e.g. one this player
      // was cruel to) refuses the haul-up — the resented are abandoned even by a bystander in
      // reach, the completing half of the steer_npcs won't-approach rung above.
      if (affinity_toward(reg, a, e) <= kGrudgeThreshold) continue;
      if (glm::distance(pos, allies.get<Transform>(a).position) < kReviveDistance) {
        rescuer = a;  // the entity handle both proves the rescue AND names who to credit
        break;
      }
    }

    down->timer -= dt;
    if (rescuer != entt::null) {
      revive(e, s, players.get<Velocity>(e));  // up in place — you keep the ground you fell on
      reg.remove<Downed>(e);
      // The first MORAL deed: hauling up a helpless ally is Charity, credited to the RESCUER (not
      // the rescued). This closes the loop on the compassion axis — compassion sets how FAST an NPC
      // reaches a rescue; finishing one now leaves a permanent moral trace. Emplacing the rescuer's
      // ledger doesn't disturb the players view being iterated (BehaviorLedger isn't one of its
      // components), the same reason the Downed emplace above is safe here.
      record_deed(reg, rescuer, Deed::Charity, kRescueCharity);
      // ...and a personal BOND forms: the rescuer grows affinity TOWARD the one they saved (the P8
      // relationships seed's one forming event). Direction is rescuer->rescued deliberately: only
      // players go Downed, so `e` is a player that doesn't run steer_npcs — putting the edge on the
      // RESCUER (usually an NPC that DOES steer) is what lets the bond produce visible motion
      // later. (If NPCs ever go Downed, revisit that rationale.) Same view-safety as the
      // record_deed above.
      nudge_affinity(reg, rescuer, e, kRescueAffinity);
    } else if (down->timer <= 0.0f) {
      // Unrescued: the humane fallback so a solo player isn't stuck down forever — respawn
      // whole at the field centre. A DELAYED, earned-back respawn, not the old instant one.
      revive(e, s, players.get<Velocity>(e));
      players.get<Transform>(e).position = respawn_point;
      reg.remove<Downed>(e);
    }
  }

  // NPCs and creatures: permadeath. Collect the dead, then destroy them AFTER the
  // loop — reg.destroy() during iteration invalidates the view (the same collect-
  // then-destroy pattern resolve_contacts uses to consume motes). A slain Enemy
  // dies here too, the same way an NPC does — one death path for everyone non-player.
  std::vector<entt::entity> dead;
  std::vector<Vec2> orb_drops;     // where slain swarmers fell — a health orb lands at each
  std::vector<Vec2> weapon_drops;  // ...where brutes fell — a weapon instead...
  std::vector<Vec2> armour_drops;  // ...and where sentinels fell — a piece of armour
  auto npcs = reg.view<Stats, Npc>();
  for (const entt::entity e : npcs) {
    if (npcs.get<Stats>(e).health.current <= 0.0f) dead.push_back(e);
  }
  auto creatures = reg.view<Stats, Enemy>();
  for (const entt::entity e : creatures) {
    if (creatures.get<Stats>(e).health.current <= 0.0f) {
      dead.push_back(e);
      // Each archetype drops its own loot, keyed on DropKind — so the drop is deterministic (no
      // roll on the shared stream). Exhaustive switch, NO default: a new DropKind won't compile
      // until it's handled here. Capture the position BEFORE the destroy loop below.
      const Vec2 pos = reg.get<Transform>(e).position;
      switch (creatures.get<Enemy>(e).drop) {
        case DropKind::HealthOrb:
          orb_drops.push_back(pos);
          break;
        case DropKind::Weapon:
          weapon_drops.push_back(pos);
          break;
        case DropKind::Armour:
          armour_drops.push_back(pos);
          break;
      }
    }
  }
  for (const entt::entity e : dead) reg.destroy(e);
  for (const Vec2& pos : orb_drops) spawn_pickup(reg, pos);     // sustain from a swarmer
  for (const Vec2& pos : weapon_drops) spawn_weapon(reg, pos);  // offence from a brute
  for (const Vec2& pos : armour_drops) spawn_armour(reg, pos);  // defence from a sentinel
}

void collect_pickups(entt::registry& reg, float dt) {
  constexpr float kPickupDistance = 15.0f;  // same reach as a contact

  std::vector<entt::entity> taken;  // collected or faded — collect, then destroy (never mid-view)
  // Anyone with a Stats sheet who ISN'T a creature can eat an orb — the player and hungry
  // NPCs alike (the same "people, not monsters" set drain_hunger and chase_prey use). The
  // exclude<Enemy> is load-bearing: creatures also carry Stats, and letting them heal and
  // gain permanent max-HP off the very orbs they drop would break the fight. The eat body
  // below already try_get-guards Skills/Attributes, so an NPC grows from loot exactly as the
  // player does — the parity the design's "NPCs run the identical system" demands. Downed is
  // excluded too: an unconscious body doesn't forage, and (mirroring regenerate_vitals) a
  // downed player must not heal off an orb they happen to have fallen on — they stay at 0 HP
  // for the whole helpless window until a rescue or respawn brings them back.
  auto eaters = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
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
        grant_skill_xp(*sk, *a, SkillId::Scavenging, kScavengingPerGrab,
                       reg.try_get<CharacterLevel>(p));
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
  // exclude<Downed> too (the shared "a Downed body is inert" invariant): a mote drifts THROUGH a
  // crumpled body without hitting it — so it isn't consumed and, crucially, the helpless body
  // earns no free Toughness from a hazard it can't react to. A person is a target only while up.
  auto targets = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
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
      stamp_flash(reg, t);                               // the struck target blinks white
      hit = true;
    }
    if (hit) consumed.push_back(h);  // consume it once, no matter how many it hit
  }

  for (const entt::entity e : consumed) reg.destroy(e);  // safe: iteration is done
}

void resolve_creature_contacts(entt::registry& reg, float dt, std::mt19937& rng) {
  constexpr float kContactDistance = 15.0f;
  constexpr float kAttackInterval = 0.8f;   // seconds between a creature's swings
  constexpr float kPoisonDuration = 3.0f;   // how long a venomous bite lingers (a knob)
  constexpr float kEnrageThreshold = 0.3f;  // below this fraction of its HP, a creature enrages...
  constexpr float kEnrageDamage = 1.75f;    // ...and its blows hit this much harder (knobs)
  const Fixed kEvasionPerSwing = Fixed::from_int(10);  // XP for facing a swing, dodged or not
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);

  // Creatures hit whoever they're hunting — the player OR an NPC (same prey set as chase_prey:
  // Stats, not a creature, and — the shared "inert body" invariant — not Downed). A crumpled
  // body is not swung at, so a helpless victim earns no free Evasion/Toughness/CharacterLevel XP
  // and draws no dodge roll off the shared stream (that was a risk-free grind: go down beside a
  // creature and farm attributes). The "dying blow at 0 HP" quirk is untouched: handle_deaths
  // emplaces Downed AFTER this system, so the tick a victim crosses 0 it is not yet Downed and
  // still takes that last hit — the exclusion only bites from the next tick, which is consistent.
  auto creatures = reg.view<Enemy, Transform>();
  auto prey = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);  // standing players + NPCs
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
        grant_skill_xp(*sk, *attrs, SkillId::Evasion, kEvasionPerSwing,
                       reg.try_get<CharacterLevel>(p));
      }

      // Dodge? A DEX-driven roll. Only draw when there's a real chance (DEX 1 = 0), so
      // an untrained world never perturbs the shared RNG stream — same sequence as before.
      const float chance = dodge_chance(attrs != nullptr ? attrs->dexterity.level : 1);
      if (chance > 0.0f && unit(rng) < chance) continue;  // slipped it — no damage taken

      // ENRAGE: a creature worn below kEnrageThreshold of its own HP lashes out HARDER
      // (kEnrageDamage×) — a cornered-beast wrinkle that makes leaving a foe half-dead dangerous,
      // so you commit to finishing it. Reads the creature's OWN health (Stats — every creature
      // carries one), pure sim, no RNG. Mostly bites on the tanky brute/sentinel you wear down; a
      // swarmer usually dies before it enrages. Knobs.
      float attack_dmg = enemy.attack_damage;
      if (const Stats* c_stats = reg.try_get<Stats>(c);
          c_stats != nullptr && c_stats->health.current < c_stats->health.max * kEnrageThreshold) {
        attack_dmg *= kEnrageDamage;
      }
      // A raised GUARD softens the blow (before VIT mitigates it too) — the active-defence trade
      // for the mobility it costs. Applies to anyone Blocking; only the player guards today.
      if (reg.all_of<Blocking>(p)) attack_dmg *= kBlockDamageFactor;
      const float applied = mitigate(attack_dmg, defence_of(reg, p));
      Vital& health = prey.get<Stats>(p).health;
      health.current -= applied;
      if (health.current < 0.0f) health.current = 0.0f;
      train_on_damage(reg, p, applied);  // enduring a creature's blow toughens the victim
      stamp_flash(reg, p);               // the struck victim blinks white

      // A venomous archetype's landed blow LINGERS: (re)apply Poisoned so the venom keeps chipping
      // after the swarm moves on — the reason a fast swarm is scary to disengage from. Refreshed on
      // each fresh bite. get_or_emplace is safe mid-iteration (Poisoned isn't in the prey view).
      // Non-venomous creatures (poison_per_second 0) skip this, so an unpoisoned world is
      // unchanged.
      if (enemy.poison_per_second > 0.0f) {
        Poisoned& venom = reg.get_or_emplace<Poisoned>(p);
        venom.remaining = kPoisonDuration;
        venom.damage_per_second = enemy.poison_per_second;
      }
    }
  }
}

void tick_poison(entt::registry& reg, float dt) {
  // Chip each poisoned entity's health by its venom, age the timer, and reap the status when spent.
  // Collect-then-remove: removing a component mid-view-walk invalidates the iterator (the ECS trap
  // decay_flashes/handle_deaths avoid). The chip routes through handle_deaths (reaps at 0), exactly
  // like starvation/dehydration — so venom can be lethal, but a tougher character's regen (VIT)
  // partly offsets it: an emergent resistance, no special case.
  std::vector<entt::entity> cured;
  auto view =
      reg.view<Poisoned, Stats>(entt::exclude<Downed>);  // a downed body is inert (invariant)
  for (const entt::entity e : view) {
    Poisoned& venom = view.get<Poisoned>(e);
    Vital& health = view.get<Stats>(e).health;
    health.current -= venom.damage_per_second * dt;
    if (health.current < 0.0f) health.current = 0.0f;
    venom.remaining -= dt;
    if (venom.remaining <= 0.0f) cured.push_back(e);
  }
  for (const entt::entity e : cured) reg.remove<Poisoned>(e);
}

void decay_flashes(entt::registry& reg, float dt) {
  // Age every hit-flash and drop the ones that have burned out. Pure presentation
  // upkeep — no rule reads HitFlash, so this only affects how the renderer draws.
  // Collect-then-remove: removing during a view walk invalidates the iterator (the
  // same ECS trap the destroy loops above avoid).
  std::vector<entt::entity> spent;
  for (auto [e, flash] : reg.view<HitFlash>().each()) {
    flash.remaining -= dt;
    if (flash.remaining <= 0.0f) spent.push_back(e);
  }
  for (const entt::entity e : spent) reg.remove<HitFlash>(e);
}

}  // namespace eng::sim
