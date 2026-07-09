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

  // Nested loops: every NPC against every hazard / orb / fallen ally / weapon — O(n*m), fine
  // for a handful. A real crowd would query a spatial grid, the same upgrade resolve_contacts
  // wants. ponytail: no reservation — several NPCs can converge on one target and the first to
  // reach it takes it (collect_pickups / npc_equip); add claims only if the scramble looks bad.
  auto npcs = reg.view<Npc, Transform, Velocity>();
  auto hazards = reg.view<Hazard, Transform>();
  auto food = reg.view<Pickup, Transform>();
  auto downed = reg.view<Downed, Transform>();
  auto weapons = reg.view<Weapon, Transform>();
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
        npcs.get<Velocity>(n).value = (toward / len) * kRescueSpeed * move_scale;
      }
      continue;  // committed to the rescue — don't also forage
    }

    // Priority 3 — hunger: a safe but hungry colonist seeks the nearest orb (its FIRST
    // want-driven motion; until now NPCs only ever fled). A fed one, or one with nothing in
    // range, FALLS THROUGH to arming up. It eats when it arrives, in collect_pickups.
    const Stats* stats = reg.try_get<Stats>(n);
    const bool hungry =
        stats != nullptr && stats->hunger.current < stats->hunger.max * kHungerSeekFraction;
    if (hungry) {
      entt::entity meal = entt::null;
      float nearest_food = kForageRadius;
      for (const entt::entity f : food) {
        const float d = glm::distance(pos, food.get<Transform>(f).position);
        if (d < nearest_food) {
          nearest_food = d;
          meal = f;
        }
      }
      if (meal != entt::null) {
        const Vec2 toward = food.get<Transform>(meal).position - pos;
        const float len = glm::length(toward);
        if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kForageSpeed * move_scale;
        continue;  // heading for a meal — don't also go weapon-hunting
      }
    }

    // Priority 4 — arm up: an UNARMED colonist (no rescue to make, not hungry, or no food in
    // range) walks to the nearest dropped weapon. npc_equip wields it on reach. An armed NPC
    // skips this (one slot). Last acting rung — no match just leaves velocity alone (drift).
    if (gear == nullptr) {
      entt::entity blade = entt::null;
      float nearest_blade = kWeaponSeekRadius;
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
    if (s.hunger.current <= 0.0f) continue;

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

  // An enemy takes STR-vs-VIT damage to its HP — base plus Strength, softened by the
  // enemy's VIT. It is NOT destroyed here; handle_deaths reaps it at 0 HP, so a weak
  // hit only chips it and it takes several. (An Enemy always carries Stats.)
  // Damage = base swing + earned-Strength delta (compounded by the veteran multiplier) + the
  // weapon's granted +Strength (flat). Only the XP-EARNED levels compound — gear is loot, not
  // grind, so it must NOT scale with an unrelated character level (that would make one blade
  // worth wildly more on a veteran). Keeps the "multiply only what you earned" invariant honest
  // and mirrors advance_progression, which scales the earned endurance.level - 1 alone.
  const int gear_strength = gear != nullptr ? gear->strength_bonus : 0;
  const float raw = kBaseAttackDamage +
                    static_cast<float>(attrs->strength.level - 1) * kDamagePerStrength * veteran +
                    static_cast<float>(gear_strength) * kDamagePerStrength;
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
    stamp_flash(reg, target);  // the struck target blinks white
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

void handle_deaths(entt::registry& reg, Vec2 respawn_point, float dt) {
  // A zero-health entity meets one of two fates, and which one is the game's core rule
  // made concrete: a PLAYER goes DOWNED (helpless where they fell, then rescued-in-place
  // by an ally or respawned when the timer runs out); an NPC dies for good (permadeath).
  // (kReviveDistance is the file-scope constant steer_npcs also runs rescuers toward.)

  // Back on your feet WHOLE — every vital, not just health. Hunger especially must reset
  // (it never self-recovers, so reviving with an empty stomach would drop a starved player
  // straight back down); stamina too, so you don't get up mid-exhaustion.
  const auto revive = [](Stats& s, Velocity& v) {
    s.health.current = s.health.max;
    s.stamina.current = s.stamina.max;
    s.hunger.current = s.hunger.max;
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
      players.get<Velocity>(e).value = Vec2{0.0f, 0.0f};
      continue;
    }

    // Already downed. A living ally (any non-downed person) within reach hauls you up where
    // you fell — the co-op rescue. NPCs don't SEEK the downed yet, so today this fires when
    // one happens by; the forage "seek nearest X" pattern is the seed of making it deliberate.
    bool rescued = false;
    const Vec2 pos = players.get<Transform>(e).position;
    auto allies = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
    for (const entt::entity a : allies) {
      if (a == e || allies.get<Stats>(a).health.current <= 0.0f)
        continue;  // self / a corpse can't help
      if (glm::distance(pos, allies.get<Transform>(a).position) < kReviveDistance) {
        rescued = true;
        break;
      }
    }

    down->timer -= dt;
    if (rescued) {
      revive(s, players.get<Velocity>(e));  // up in place — you keep the ground you fell on
      reg.remove<Downed>(e);
    } else if (down->timer <= 0.0f) {
      // Unrescued: the humane fallback so a solo player isn't stuck down forever — respawn
      // whole at the field centre. A DELAYED, earned-back respawn, not the old instant one.
      revive(s, players.get<Velocity>(e));
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
      stamp_flash(reg, t);                               // the struck target blinks white
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
        grant_skill_xp(*sk, *attrs, SkillId::Evasion, kEvasionPerSwing,
                       reg.try_get<CharacterLevel>(p));
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
      stamp_flash(reg, p);               // the struck victim blinks white
    }
  }
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
