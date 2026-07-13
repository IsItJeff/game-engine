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

// Rescuing someone you were ALREADY bonded to is more than charity — it's LOYALTY, standing by your
// own. When the rescuer's affinity toward the fallen is already at/above kBondPull (a real, prior
// bond), a rescue records this Loyalty deed too, on top of the Charity every rescue earns. standing
// weights Loyalty x3 and Charity x4, so a save of a FRIEND reads as +7 vs a stranger's +4. Gated on
// the bond that existed BEFORE this rescue's own +kRescueAffinity nudge, so a first save of a
// stranger is charity only — loyalty is for a tie you already had, not one the save itself creates.
constexpr std::int32_t kRescueLoyalty = 1;

// The affinity floor at which a directed tie counts as a REAL bond, shared by two readers: the
// bond-pull steer rung (an idle colonist drifts toward a friend at/above this) and the
// loyalty-on-rescue gate above. One "a friendship starts at +10" line, so both agree what a bond
// is.
constexpr std::int8_t kBondPull = 10;

// Landing the killing blow on a HOSTILE is a Valor deed of this size on the attacker — one atomic
// deed unit, the offensive twin of a rescue's Charity. standing weights Valor ×5, so a slain
// monster reads as +5 standing.
constexpr std::int32_t kValorKill = 1;

// KILL VIGOR: felling a foe restores this much HEALTH to the killer — combat's direct sustain axis,
// a comeback tool that rewards pressing the attack instead of turtling. Fires at the SAME kill
// transition as the Valor credit (melee and ranged both), capped at max. A kill's adrenaline, NOT
// the passive mending regenerate_vitals gates behind a full belly — a distinct source, so it heals
// even a starving killer. (Distinct from the STAMINA "second wind" in update_stamina, which IS
// starvation-gated — this is health from a kill, that is stamina from rest.) A playtest knob; small
// enough to be a lifeline, not a heal-lock.
constexpr float kKillVigor = 8.0f;

// Striking a PEACEFUL colonist is a Cruelty deed of this size on the attacker — one atomic deed
// unit, the VILLAIN mirror of Valor and the first deed that SINKS standing. standing weights
// Cruelty ×6, so one betrayal reads as -6 standing: villainy is dearer than heroism is cheap, on
// purpose.
constexpr std::int32_t kCrueltyStrike = 1;

// A cruel strike that KILLS its victim escalates from harm to VIOLENCE — the design's "unjust
// violence", the SECOND villain deed. Cruelty is the blow; Violence is the death it deals. One
// atomic unit like the others; standing weights Violence ×4, so a lethal cruel strike sinks you by
// the Cruelty ×6 AND this ×4 (−10 total) — killing a colonist is dearer than merely wounding one
// (−6). It also lands the last REACHABLE unfed ledger dimension (Honesty still waits on a truth
// event), making the dominant-Violence deed_epithet "the Brutal" reachable at last. ponytail: the
// design's "unjust" nuance (Violence counts only vs a standing>=0 victim — killing a bandit barely
// dents you) is satisfied by CONSTRUCTION, not a literal check: this branch only ever strikes a
// PEACEFUL colonist (hostiles are searched first and always win the target), so a bandit can't be
// the victim. Add an explicit victim-standing gate the day a negative-standing colonist can be
// nearest.
constexpr std::int32_t kViolenceKill = 1;

// A cruel strike also earns a personal GRUDGE — the negative mirror of a rescue's +kRescueAffinity
// bond: the struck colonist loses this much affinity TOWARD the striker (hurt someone and they
// resent you, exactly as saving someone endears you). A colonist whose affinity toward a player has
// sunk to kGrudgeThreshold or below will NOT cross the field to rescue that player — the resented
// are abandoned. One strike (-25) already clears the -20 line, so a single betrayal earns it: a
// pointed, PERSONAL consequence that lands EARLIER than the global villain-fear (which needs
// standing past the Suspect line, several strikes off). Playtest knobs.
constexpr std::int8_t kCrueltyGrudge = -25;
constexpr std::int8_t kGrudgeThreshold = -20;

// Felling a hostile near allies forges CAMARADERIE: each standing colonist within
// kCamaraderieRadius of the killer gains kCamaraderieAffinity TOWARD them — the design's "fighting
// a common foe" bond, the third way a tie forms (a rescue bonds the saved, a cruel strike grudges
// the struck, and now a shared victory bonds the witnesses). Directed witness->killer, so it feeds
// the readers that already exist: the colony clusters toward (bond-pull) and rescues from farther
// (the graded rescue reach) a champion who fights beside them. Lighter than a rescue's +20
// (witnessing < being saved), so devotion builds over several shared kills rather than one.
// Playtest knobs.
constexpr float kCamaraderieRadius = 120.0f;  // also the "how far a deed is witnessed" range below
constexpr std::int8_t kCamaraderieAffinity = 5;

// A LESSON forges GRATITUDE: the tick a student LEARNS a craft it never had from its mentor (the
// skill first appears, 0 -> level 1), the student gains this much affinity TOWARD the mentor — a
// shared-events-forge-ties bond beside camaraderie (a shared kill), admiration (a witnessed
// rescue), and the grudge (a cruel strike). Directed student->mentor, feeding the same readers (the
// apprentice clusters toward its master via bond-pull, defends it, is rescued-from-farther). A
// DISCRETE moment like those, not the per-tick XP trickle: teach fires every adjacent tick, but a
// skill is learned ONCE, so the bond forms once per NEW craft passed (a mentor who later teaches a
// SECOND craft the student lacks bonds it again). Only the first-LEARN is caught, never a rank-up:
// grant_skill_xp banks only XP, and levels are applied a step later by advance_progression, so
// within teach the sole detectable breakthrough is the skill appearing. Set just ABOVE kBondPull
// (10) so ONE lesson is a real Acquaintance tie the readers act on, yet below a rescue's +20 (a
// lesson is a smaller thing than your life saved). Unlatched, so it fades if the apprenticeship
// doesn't continue. A knob.
constexpr std::int8_t kGratitudeAffinity = 12;

// A cruel strike is WITNESSED: nearby colonists who saw it (within kCamaraderieRadius) form a small
// grudge TOWARD the striker too — the negative mirror of camaraderie, so a reputation for cruelty
// SPREADS through the community, not just the direct victim (who forms a larger grudge of their
// own). Milder than the victim's kCrueltyGrudge (-25), so ONE witnessed cruelty won't cross the
// kGrudgeThreshold rescue-abandonment line, but a PATTERN of them will (a colonist that keeps
// seeing you hurt its fellows stops trusting you). Playtest knob.
constexpr std::int8_t kWitnessGrudge = -8;

// How far a single deed nudges the actor's matching PERSONALITY axis (deed-driven DRIFT). Small and
// bounded on purpose — the design wants "the war changed him", not a wholly different person: at 2
// per deed a full ±100 swing takes ~50 deeds, so a demo's handful gives a visible-but-partial
// shift. A tuning knob.
constexpr int kDeedDriftStep = 2;

// A bereaved survivor's nerve slips this far when a bonded friend is lost (GRIEF, handle_deaths) —
// the negative MIRROR of a Valor deed's +kDeedDriftStep: fighting monsters hardens you, watching
// one of your own fall shakes you. Same small magnitude, so one lost friend costs about one brave
// deed's worth of nerve. A tuning knob.
constexpr int kGriefDrift = -kDeedDriftStep;
// The opposite bereavement: when a sworn NEMESIS falls, a survivor's nerve steadies the OTHER way
// (VINDICATION, handle_deaths) — the tormentor that cowed you is gone, so you stand a little
// taller. The positive mirror of grief (and, like grief, one deed's worth of drift). No acute twin:
// a rival's death is a quiet relief, not a shock, so there is no vindication-equivalent of the
// panic rout. A tuning knob.
constexpr int kVindicationDrift = kDeedDriftStep;
// ...and grief's ACUTE twin: for this many seconds after a bonded friend falls, the survivor PANICS
// (Panicked, read by steer_npcs, ticked down by tick_panic) — a short rout on top of the permanent
// nerve slip above. A tuning knob.
constexpr float kPanicDuration = 3.0f;

// Nudge a bounded personality axis by a signed step, clamped to [-100, 100]. The single write
// behind BOTH a deed's drift (record_deed) and a bereavement's grief (handle_deaths), so the two
// paths can never clamp differently. Pure int math (widened before the clamp so a long career can't
// overflow the int8), no RNG.
void drift_axis(std::int8_t& axis, int step) {
  int v = static_cast<int>(axis) + step;
  if (v > 100) v = 100;
  if (v < -100) v = -100;  // symmetric clamp — positive for deeds, negative for grief
  axis = static_cast<std::int8_t>(v);
}

// True if `pos` sits within ANY Hearth's radius — the shared "in the warmth" predicate. The fire
// both HEALS those who rest in it (regenerate_vitals boosts their regen) and WARDS the beasts that
// would hunt them (chase_prey skips sheltered prey), so the two must agree on the EXACT reach — one
// predicate keeps them from drifting apart. `<=` so the edge counts, matching the drink/graze reach
// tests. No hearths (or none in reach) -> false, so a hearthless world is bit-identical.
bool in_a_hearth(entt::registry& reg, Vec2 pos) {
  auto hearths = reg.view<Hearth, Transform>();
  for (const entt::entity h : hearths) {
    if (glm::distance(pos, hearths.get<Transform>(h).position) <= hearths.get<Hearth>(h).radius)
      return true;
  }
  return false;
}

// A BACKSTAB multiplier — kBackstabBonus if `target` (at target_pos, moving target_vel) has its
// BACK to the attacker at from_pos, i.e. it's moving roughly AWAY (heading within ~60deg of the
// from_pos->target_pos line); else 1.0. The single "don't turn your back" rule shared by BOTH
// sides: the flank a fighter lands on a fleeing creature (perform_attack) AND the one a creature
// lands on a fleeing victim (resolve_creature_contacts), so the two can never diverge. Pure
// geometry, no RNG; a STATIONARY or FACING target — or one with no Velocity — is 1.0, so a still
// target is bit-identical.
float backstab_multiplier(Vec2 from_pos, Vec2 target_pos, const Velocity* target_vel) {
  constexpr float kBackstabBonus = 1.4f;  // a hit from behind lands this much harder (a knob)...
  constexpr float kBackstabCosine =
      0.5f;  // ...when the target's heading is within ~60deg of "away"
  if (target_vel == nullptr) return 1.0f;
  const Vec2 to_target = target_pos - from_pos;
  const float speed = glm::length(target_vel->value);
  const float away = glm::length(to_target);
  if (speed > 0.0f && away > 0.0f &&
      glm::dot(target_vel->value / speed, to_target / away) > kBackstabCosine)
    return kBackstabBonus;
  return 1.0f;
}
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
  // Retreat: a WOUNDED colonist (health below this fraction) OR a CHILLED one (warmth below
  // kColdSeekFraction) falls back to the nearest Hearth — to mend faster in its warmth
  // (regenerate_vitals boosts regen there) AND to RE-WARM (drain_warmth's fire beats the cold). It
  // holds once inside the hearth's own radius. The seek-warmth want the temperature Need needed: a
  // chilled colonist now actively heads for the fire, not only once the cold has hurt it into the
  // wounded case — so warmth drives behaviour like hunger/water do. Same wide-scan shape. Knobs.
  constexpr float kRetreatFraction = 0.5f;
  constexpr float kHearthSeekRadius = 300.0f;
  constexpr float kColdSeekFraction =
      0.5f;  // below half warmth, seek the fire (like thirst seeks a well)
  constexpr float kHearthSeekSpeed = 80.0f;
  // Rally: an IDLE colonist drifts toward a nearby renowned-enough hero (the inverted twin of the
  // top-priority villain flee). A gentle gather — lower speed than a flee or a forage, since it's
  // the lowest-urgency want. Knobs.
  constexpr float kRallyRadius = 220.0f;
  constexpr float kRallySpeed = 70.0f;
  // Bond pull: the PERSONAL twin of the public hero-rally — an idle colonist with no hero to gather
  // to drifts toward a nearby FRIEND (a positive-affinity Relationship, e.g. the one it rescued).
  // Reuses kRallySpeed. Knobs.
  constexpr float kBondRadius = 220.0f;
  // kBondPull (the affinity floor at which a tie is a real bond) is shared with the
  // loyalty-on-rescue deed, so it lives with the deed constants at the top of the file rather than
  // local here.
  // Defend: the URGENT twin of the bond-pull — an idle colonist CHARGES to a bonded friend (often
  // the player it fought beside) when a CREATURE is bearing down on it, to stand and fight at its
  // side (the active slice of the design's PROTECT stance; bond-pull only drifts toward an idle
  // friend). kDefendReach is how far it will cross (bravery scales it, the rescue rung's courage
  // shape); kDefendThreatRadius is how near a creature must be to the friend to count as a threat.
  // Reuses kRescueSpeed (urgent, like a rescue). Knobs.
  constexpr float kDefendReach = 300.0f;
  constexpr float kDefendThreatRadius = 150.0f;
  // A PARTNER (the deepest bond, affinity >= kBondPartnerAt) is defended from this much FARTHER:
  // you cross the whole field for the one you're closest to, where a mere friend must be near. The
  // teeth that make the top bond TIER mean something in a fight. This keys on the discrete TIER,
  // DELIBERATELY unlike its twin the downed-rescue reach, which grades CONTINUOUSLY by raw affinity
  // (d scaled by 1 - aff/200): rescue rewards any warmth, defend rewards the Partner milestone. The
  // two land a Partner's reach near each other (~480 here vs ~500 there). A knob.
  constexpr float kPartnerDefendBoost = 1.6f;
  // Hearth gather: the peacetime want, the lowest rung of all. A truly idle SOCIABLE colonist
  // ambles to the nearest fire to gather round it, so the hearth is a social HUB — not only the
  // field hospital the wounded-retreat rung makes it. This is the RADIUS at full sociability
  // (+100); the gather range scales PROPORTIONALLY (not the rally/bond base+offset shape), so
  // 0-or-below sociability never gathers at all. Reuses kRallySpeed. A knob.
  constexpr float kHearthGatherRadius = 300.0f;
  // Avoid: the negative twin of the bond pull — an idle colonist keeps this much distance from an
  // entity it RESENTS (affinity <= kGrudgeThreshold). Smaller than the friend-gather range (a
  // personal-space bubble, not a cross-field draw). Reuses kRallySpeed. A knob.
  constexpr float kAvoidRadius = 150.0f;
  // A NEMESIS (the deepest grudge, affinity <= kBondNemesisAt) is avoided from this much FARTHER —
  // the widest berth for your worst enemy, the negative twin of kPartnerDefendBoost's defend teeth.
  // Keys on the discrete Nemesis TIER, symmetric with that positive twin. A knob.
  constexpr float kNemesisAvoidBoost = 1.6f;
  // Hunt: an idle colonist that DREAMS of battle (an Aspiration of kind Warrior) seeks the nearest
  // creature within this range and CHARGES it — the first proactive, goal-driven steer. The range
  // is wide (it spots a fight across much of the field) but the rung is LOW priority, so only a
  // content warrior — fed, watered, warm, unwounded, unafraid — goes looking; a hurting one tended
  // its needs on a rung above. The charge speed is brisk (a warrior commits), between a forage and
  // a rescue. npc_attack (which strikes the nearest creature in Strength-reach every tick) does the
  // actual fighting once the charge closes the gap; this rung only supplies the intent to close it.
  // Knobs.
  constexpr float kHuntRange = 300.0f;
  constexpr float kHuntSpeed = 85.0f;
  // Tend: a PROVIDER-aspiration colonist (the peaceful twin of the warrior) walks to the nearest
  // food plot that still has something to gather and WORKS it — npc_harvest reaps a ripe one into a
  // meal on arrival. Same wide range as the hunt (it spots a field across the map), a purposeful
  // walking pace. Knobs.
  constexpr float kTendRange = 300.0f;
  constexpr float kTendSpeed = 80.0f;
  // Study: a SCHOLAR-aspiration colonist that hasn't yet learned to cast walks to the nearest
  // Spellbook to READ it (study_spellbooks grants Spellcasting on arrival) — the knowledge twin of
  // the warrior's hunt and the provider's harvest. Same wide range (it spots a tome across the
  // map), a purposeful pace. Knobs.
  constexpr float kStudyRange = 300.0f;
  constexpr float kStudySpeed = 80.0f;

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
  auto hearths = reg.view<Hearth, Transform>();
  auto creatures = reg.view<Enemy, Transform>();      // for the DEFEND rung: a threat near a friend
  auto cold_zones = reg.view<ColdZone, Transform>();  // for the AVOID-THE-COLD rung
  auto books =
      reg.view<Spellbook, Transform>();  // for the SCHOLAR aspiration: a tome to learn from
  for (const entt::entity n : npcs) {
    const Vec2 pos = npcs.get<Transform>(n).position;

    // A wielded weapon's heft slows an NPC just as it slows the player (the bane must bite
    // both — parity). Every steer speed below is scaled by this, so an armed colonist flees,
    // rescues, and forages a touch slower. Unarmed = 1.0 (no change). STR eases the heft (carry)
    // via the same carried_move_penalty the player uses — shrunk by Strength, capped at half so the
    // bane persists; STR 1 or no Attributes -> full heft -> bit-identical.
    const Equipped* gear = reg.try_get<Equipped>(n);
    float move_scale = gear != nullptr ? 1.0f - carried_move_penalty(gear->move_penalty,
                                                                     reg.try_get<Attributes>(n))
                                       : 1.0f;
    // EXHAUSTION crawls an NPC too — parity with the player's MovePlayer crawl: a colonist that has
    // spent its stamina to 0 (by moving) slows to kExhaustedMoveScale, so the tireless-no-more rule
    // the player pays now applies to NPCs, who drain and recover stamina by the same
    // update_stamina. Stacks with the heft above (a tired, armed colonist really trudges). Stats is
    // always on an Npc; fetched once here and reused by the need/retreat rungs below. Full stamina
    // -> no crawl (the common case), so a rested colony steers exactly as before (bit-identical).
    const Stats* stats = reg.try_get<Stats>(n);
    if (stats != nullptr && stats->stamina.current <= 0.0f) move_scale *= kExhaustedMoveScale;
    // ...and STARVATION drags the legs too — the Need debuff reaches every step, not just the
    // swing. need_efficiency (the same 1.0-at-comfort -> 0.5-at-empty curve that saps combat damage
    // and greys the dot) scales move_scale, so a starving or parched colonist TRUDGES toward
    // whatever it wants — one source of truth for the whole debuff. Applied uniformly like the
    // crawl above (so even the forage/water-seek rungs are slowed: a weak body is sluggish on its
    // way to the meal too), but the 0.5 FLOOR means it always keeps moving and reaches the food,
    // never freezes. Both needs at/above a quarter -> 1.0 -> a fed colony steers exactly as before
    // (bit-identical).
    if (stats != nullptr) move_scale *= need_efficiency(*stats);

    // Perception, priority 1 — danger: the single nearest hazard within sense range. How near a
    // hazard gets before this NPC senses (and so flees) it is shaped by its BRAVERY: a coward
    // senses danger from further and bolts EARLY; a brave colonist lets a hazard get close before
    // it runs. No Personality (or bravery 0) → the base radius exactly, so this is bit-identical
    // for anyone without a leaning. ponytail: 200 is the sensitivity knob (bravery ±100 →
    // radius 0.5×..1.5× kSenseRadius). Cast the int8 to float BEFORE the divide (-Wconversion).
    const Personality* pers = reg.try_get<Personality>(n);
    const float bravery = pers != nullptr ? static_cast<float>(pers->bravery) : 0.0f;
    // WISDOM sharpens AWARENESS: a wiser colonist PERCEIVES danger from further, so it senses (and
    // flees) a hazard sooner — a distinct source from bravery's nerve. Bravery is REACTION (how
    // close you let danger get before bolting), Wisdom is PERCEPTION (how far you see it coming);
    // they COMPOSE, so a wise coward is hyper-alert (both widen the radius) while a wise but brave
    // colonist sees danger early yet holds its ground (the two oppose). WIS is trained by FORAGING,
    // so the forager who knows the land reads as the alert one — a coherent second effect for the
    // attribute. No Attributes (or WIS level 1, the common case and every flee test) -> ×1.0 ->
    // bit-identical. Cast the level to float before the multiply (-Wconversion).
    constexpr float kAwarenessPerWis =
        0.05f;  // each Wisdom level past 1 widens the sense radius...
    constexpr float kAwarenessCap =
        2.0f;  // ...up to 2x, then it caps (like the dodge/crit clamps),
               // so a lifetime of foraging can't sense the whole field.
    float awareness = 1.0f;
    if (const Attributes* wis_attrs = reg.try_get<Attributes>(n)) {
      awareness += static_cast<float>(wis_attrs->wisdom.level - 1) * kAwarenessPerWis;
      if (awareness > kAwarenessCap) awareness = kAwarenessCap;
    }
    // COURAGE IN NUMBERS: a bonded friend standing nearby STEADIES the nerve — this colonist lets a
    // hazard get closer before bolting, where a lone one would bolt early. The passive, positive
    // mirror of grief (a friend's DEATH drifts bravery DOWN) and of the DEFEND rung (charge to a
    // THREATENED friend): here a merely-PRESENT friend emboldens you. Reads the same Relationships
    // edges the defend/bond rungs do (affinity >= kBondPull, a valid non-Downed friend) within
    // kCourageRadius, and SHRINKS the sense radius by kSteadiedPerFriend per nearby friend
    // (capped), so a cluster holds ground while a straggler flees. No Relationships / no friend in
    // range -> steadied 0 -> factor 1.0 -> bit-identical. Draws no RNG. ponytail: O(edges) per NPC,
    // the same scan shape the defend rung already runs.
    constexpr float kCourageRadius = 140.0f;  // how near a friend must be to steady you
    constexpr float kSteadiedPerFriend =
        0.15f;                            // each nearby friend shrinks the flee radius 15%...
    constexpr float kSteadiedCap = 0.5f;  // ...down to half — you never ignore danger entirely
    float steadied = 0.0f;
    if (const Relationships* rel = reg.try_get<Relationships>(n)) {
      for (const Relation& edge : rel->edges) {
        if (edge.affinity < kBondPull || !reg.valid(edge.other)) continue;  // not a real bond
        if (reg.all_of<Downed>(edge.other)) continue;  // a downed friend steadies no one
        const Transform* ft = reg.try_get<Transform>(edge.other);
        if (ft != nullptr && glm::distance(pos, ft->position) < kCourageRadius)
          steadied += kSteadiedPerFriend;
      }
      if (steadied > kSteadiedCap) steadied = kSteadiedCap;
    }
    // PANIC: a colonist that just watched a bonded friend fall (Panicked, emplaced by handle_deaths
    // and counted down by tick_panic) is ROUTED for a few seconds — it senses danger from much
    // farther (kPanicSenseBoost widens the flee radius) and BOLTS faster (kPanicSpeedBoost),
    // overriding its usual nerve; it will even flee the CREATURES below, which it normally stands
    // against. The ACUTE mirror of grief's slow, permanent bravery drift. No Panicked marker
    // (anyone not freshly bereaved, every existing steer test) -> both factors 1.0 and no
    // creature-flee -> bit-identical.
    constexpr float kPanicSenseBoost = 1.8f;  // routed: flees threats from 80% farther...
    constexpr float kPanicSpeedBoost = 1.4f;  // ...and bolts 40% faster
    const bool panicked = reg.all_of<Panicked>(n);
    const float panic_sense = panicked ? kPanicSenseBoost : 1.0f;
    const float panic_speed = panicked ? kPanicSpeedBoost : 1.0f;
    float nearest =
        kSenseRadius * (1.0f - bravery / 200.0f) * awareness * (1.0f - steadied) * panic_sense;
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

    // A PANICKED colonist also flees the CREATURES themselves — normally NPCs stand their ground
    // and get chased (a creature is not a flee threat), but a rout bolts from the very monsters
    // that felled its friend. Only while Panicked, so the ordinary fight is untouched
    // (bit-identical). Reuses the `creatures` view the DEFEND rung already fetched, and the
    // panic-widened `nearest`.
    if (panicked) {
      for (const entt::entity c : creatures) {
        const Vec2 c_pos = creatures.get<Transform>(c).position;
        const float d = glm::distance(pos, c_pos);
        if (d < nearest) {
          nearest = d;
          threat = c_pos;
          sees_threat = true;
        }
      }
    }

    // Fear beats everything: flee straight away from a threat, ignoring ally and food alike. A
    // panic bolts faster (panic_speed).
    if (sees_threat) {
      const Vec2 away = pos - threat;
      const float len = glm::length(away);
      if (len > 0.0f)
        npcs.get<Velocity>(n).value = (away / len) * kFleeSpeed * move_scale * panic_speed;
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
      const std::int8_t aff = affinity_toward(reg, n, f);
      if (aff <= kGrudgeThreshold) continue;
      // The fallen's OWN STANDING drives two mirror readers here — a VILLAIN veto (skip) and a HERO
      // reach-boost (below) — so fetch it once (0 for no ledger, the common case). Only players go
      // Downed and only a player can turn villain/hero (deeds are player-gated), so both bite
      // exactly a downed PLAYER.
      const BehaviorLedger* fallen_led = reg.try_get<BehaviorLedger>(f);
      const std::int32_t fallen_standing = fallen_led != nullptr ? standing(*fallen_led) : 0;
      // ...and NOBODY crosses the field for a famous VILLAIN: marked by its deeds (standing at or
      // below -kKnownAt, the Suspect line villain-fear uses), the whole colony leaves it down — the
      // GLOBAL counterpart of the personal grudge above, kept in LOCKSTEP with the same veto in
      // handle_deaths so a colonist never crosses the field to someone it would then refuse to
      // lift.
      if (fallen_standing <= -kKnownAt) continue;
      // FRIENDSHIP grades the trek above that hard cutoff: a bonded ally (one this NPC has saved
      // before, so its affinity has grown) FEELS closer — its distance is discounted on the same
      // /200 shape the other rungs use — so the colonist crosses a LONGER real field for a dear
      // friend, while a mild dislike (still above the grudge line) feels a touch farther and is
      // dropped sooner. The discount only weights the CHOICE of whom to save; the steer below uses
      // real geometry. Neutral 0 -> real distance (bit-identical). ponytail: the /200 reach knob.
      float d = glm::distance(pos, downed.get<Transform>(f).position) *
                (1.0f - static_cast<float>(aff) / 200.0f);
      // ...and FAME grades it too: a famous HERO (standing at or above +kKnownAt, the same Known
      // line the hero-rally uses) is worth crossing the field for even by a STRANGER — its distance
      // is discounted, so the colony RUSHES to a downed champion from farther. The positive
      // standing MIRROR of the villain veto (villain: no rescue; hero: reached from farther;
      // neutral: real distance -> bit-identical) and the public-fame twin of the personal affinity
      // discount — they STACK, so a bonded hero is worth the longest trek of all. Binary at the
      // Known line, like the fear/rally reads, so standing acts consistently.
      constexpr float kHeroReachDiscount =
          0.6f;  // a hero feels this fraction as far (worth the trek)
      if (fallen_standing >= kKnownAt) d *= kHeroReachDiscount;
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

    // Priority 2.5 — DEFEND a threatened friend: an idle colonist CHARGES to a bonded friend (often
    // the player it fought beside, or an ally it rescued) when a CREATURE is bearing down on it, to
    // stand and fight at its side — npc_attack does the actual swinging once in reach. The ACTIVE,
    // urgent slice of the design's PROTECT stance: the bond-follow rung far below only *drifts*
    // toward an idle friend, but this DROPS everything (outranks its own hunger, like the rescue
    // above) to answer a friend in danger. Reads the same Relationships as bond-follow (affinity >=
    // kBondPull, a valid, non-Downed friend — a Downed one is the rescue rung's job), gated on a
    // THREAT: a creature within kDefendThreatRadius of that friend. BRAVERY scales how far this NPC
    // will cross to defend — the courage to charge into danger for a friend, the SAME
    // growing-radius shape the rescue rung uses (neutral 0 -> kDefendReach exactly), so every
    // acting rung still reads a trait. No Relationships, no bonded friend, or no creature bearing
    // down on one -> falls through, so the pre-defend world (no bond, or no creature near a friend
    // — every existing test) is bit-identical. Steers toward the FRIEND at rescue speed; picks the
    // nearest threatened one.
    if (const Relationships* rel = reg.try_get<Relationships>(n)) {
      entt::entity ward = entt::null;
      Vec2 ward_pos{0.0f, 0.0f};
      const float base_reach = kDefendReach * (1.0f + bravery / 200.0f);
      float nearest_d =
          base_reach * kPartnerDefendBoost;  // the widest a friend could be (a Partner)
      for (const Relation& edge : rel->edges) {
        if (edge.affinity < kBondPull || !reg.valid(edge.other))
          continue;                                    // not a real bond, or a stale handle
        if (reg.all_of<Downed>(edge.other)) continue;  // a downed friend is the rescue rung's job
        const Transform* ft = reg.try_get<Transform>(edge.other);
        if (ft == nullptr)
          continue;               // a friend with no position (shouldn't happen, cheap to guard)
        bool threatened = false;  // is a creature bearing down on this friend?
        for (const entt::entity c : creatures) {
          if (glm::distance(ft->position, creatures.get<Transform>(c).position) <
              kDefendThreatRadius) {
            threatened = true;
            break;
          }
        }
        if (!threatened) continue;
        // A PARTNER (the deepest bond) is defended from FARTHER (kPartnerDefendBoost x); a plain
        // friend uses the base bravery-scaled reach. Per-friend, so with no Partner bond (every
        // existing scene) all reaches are the base and the selection is bit-identical to before.
        const float reach =
            edge.affinity >= kBondPartnerAt ? base_reach * kPartnerDefendBoost : base_reach;
        const float d = glm::distance(pos, ft->position);
        if (d < reach && d < nearest_d) {  // within THIS friend's reach, and the nearest so far
          nearest_d = d;
          ward = edge.other;
          ward_pos = ft->position;
        }
      }
      if (ward != entt::null) {
        const Vec2 toward = ward_pos - pos;
        const float len = glm::length(toward);
        if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kRescueSpeed * move_scale;
        continue;  // charging to a friend's defence — don't also forage
      }
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
    // `stats` was fetched at the top (for the exhaustion crawl) and is reused here.
    const bool hungry =
        stats != nullptr && stats->hunger.current < stats->hunger.max * seek_fraction;
    // URGENCY beats rung order: if a hungry colonist is ALSO thirsty and its WATER is more depleted
    // than its food (a lower current/max fraction), defer to the water rung below — so a colonist
    // dying of thirst doesn't forage first merely because hunger is checked first. Fractions
    // compared by cross-multiply (both maxes > 0), no divide. When only one need bites it wins;
    // when both bite, the more-depleted one goes first — BUT only if that need is actionable: we
    // defer to thirst only when a WaterSource is actually in reach, else a thirst it can't act on
    // would stall the colonist on BOTH needs beside food it could have eaten (the thirst rung would
    // find no well and fall through). So an unreachable thirst never blocks a reachable meal.
    bool thirst_first = false;
    if (hungry) {  // implies stats != nullptr
      const bool thirsty = stats->water.current < stats->water.max * kThirstSeekFraction;
      const bool water_more_depleted =
          stats->water.current * stats->hunger.max < stats->hunger.current * stats->water.max;
      if (thirsty && water_more_depleted) {
        for (const entt::entity w : sources) {  // defer only if a well is genuinely reachable
          if (glm::distance(pos, sources.get<Transform>(w).position) < kWaterSeekRadius) {
            thirst_first = true;
            break;
          }
        }
      }
    }
    if (hungry && !thirst_first) {
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
    // drink system tops it up on arrival). Sits just below hunger, but the two are ordered by
    // URGENCY, not rung position: the hunger rung above defers to this one when water is the more
    // depleted need (see thirst_first), so whichever need is closer to empty is sought first.
    // Reuses the `stats` fetched for the hunger rung; a plain need threshold (no personality read
    // yet, unlike greed on forage).
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

    // Priority 3.75 — retreat to a HEARTH to heal OR warm up: a SAFE colonist that is WOUNDED
    // (health below kRetreatFraction) OR CHILLED (warmth below kColdSeekFraction) falls back to the
    // nearest hearth — to mend faster in its warmth (regenerate_vitals boosts regen there) AND to
    // re-warm (drain_warmth's fire beats the cold). Ranks below the NEEDS deliberately — a starving
    // colonist can't heal anyway (the regen gate), so it forages/drinks first, then mends/warms —
    // and above arming up (survive before you gear). Once inside the hearth's own radius it HOLDS
    // (stops, to sit in the warmth); outside, it heads in. This makes the hearth a USED landmark:
    // wounded AND chilled colonists gather at the fire. The CHILLED half is the temperature Need's
    // seek want — it closes the "huddle by the fire" loop directly, not only via the
    // freeze-into-wounded path. No hearth in range -> falls through. A colonist that is neither
    // wounded NOR chilled (warmth full, every world without a ColdZone) skips this exactly as
    // before -> bit-identical. Reuses the `stats` fetched for the hunger rung.
    if (stats != nullptr && (stats->health.current < stats->health.max * kRetreatFraction ||
                             stats->warmth.current < stats->warmth.max * kColdSeekFraction)) {
      entt::entity fire = entt::null;
      float nearest_fire = kHearthSeekRadius;
      float fire_radius = 0.0f;
      for (const entt::entity h : hearths) {
        const float d = glm::distance(pos, hearths.get<Transform>(h).position);
        if (d < nearest_fire) {
          nearest_fire = d;
          fire = h;
          fire_radius = hearths.get<Hearth>(h).radius;
        }
      }
      if (fire != entt::null) {
        const Vec2 toward = hearths.get<Transform>(fire).position - pos;
        const float len = glm::length(toward);
        if (len > fire_radius) {  // still out in the cold — head for the warmth...
          npcs.get<Velocity>(n).value = (toward / len) * kHearthSeekSpeed * move_scale;
        } else {
          npcs.get<Velocity>(n).value = Vec2{0.0f, 0.0f};  // ...within it, hold and mend
        }
        continue;  // tending its wounds — don't also go weapon-hunting
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
        // * move_scale like every other rung, so a tired colonist trudges to a weapon too (the
        // exhaustion crawl is uniform). Unarmed -> heft is 1.0, so a rested NPC is bit-identical.
        if (len > 0.0f)
          npcs.get<Velocity>(n).value = (toward / len) * kWeaponSeekSpeed * move_scale;
        continue;  // heading for a weapon — an idle colonist would have fallen through to rally
      }
    }

    // Priority 4.9 — AVOID THE COLD: an idle colonist standing in a ColdZone drifts OUT of it (away
    // from the zone's centre — radially, the shortest way to its edge), so a wanderer that has
    // blundered into the cold steps back into the warm before it chills. The PREVENTION half of the
    // temperature Need, the complement of the seek-warmth retreat above (which is RECOVERY, for one
    // ALREADY chilled — and which, being higher priority, wins: a chilled colonist heads to the
    // FIRE, not just out of the cold). LOW priority, so a colonist with any real want — flee,
    // rescue, forage, drink, mend/warm, arm — tends it first, and only an otherwise-idle one
    // bothers to step out of the chill. No ColdZone (every world without one) -> nothing to avoid
    // -> falls through -> bit-identical.
    {
      entt::entity zone = entt::null;
      Vec2 zone_pos{0.0f, 0.0f};
      float deepest =
          0.0f;  // the zone it's DEEPEST inside (radius - dist > 0 == inside, by that much)
      for (const entt::entity z : cold_zones) {
        const Vec2 zp = cold_zones.get<Transform>(z).position;
        const float depth = cold_zones.get<ColdZone>(z).radius - glm::distance(pos, zp);
        if (depth > deepest) {
          deepest = depth;
          zone = z;
          zone_pos = zp;
        }
      }
      if (zone != entt::null) {
        const Vec2 away =
            pos - zone_pos;  // radially outward = toward the nearest edge, the way out
        const float len = glm::length(away);
        if (len > 0.0f) {
          npcs.get<Velocity>(n).value = (away / len) * kRallySpeed * move_scale;
          continue;  // stepping out of the cold — skip the idle gather rungs below
        }
      }
    }

    // Priority 5 — AVOID: the negative twin of the BOND pull below. An idle colonist keeps its
    // distance from someone it RESENTS — an entity it holds a grudge toward (affinity <=
    // kGrudgeThreshold, e.g. a player who struck it), the ACTIVE completion of the grudge that
    // already makes it refuse to rescue that player (handle_deaths). It steers AWAY from the
    // nearest such entity within kAvoidRadius — the mirror of bond-pull's TOWARD. Distinct from the
    // top-of- ladder FEAR (which flees a globally VILLAINOUS player by standing): this is PERSONAL
    // (by affinity) and lands EARLIER — one cruel strike crosses kGrudgeThreshold, long before
    // standing sinks past the Suspect line — so a wronged colonist shies from you before the whole
    // colony does. LOW priority, so a hungry or endangered colonist tends its needs first and only
    // backs off when otherwise idle. BRAVERY scales the avoid RADIUS, the SAME shape the fear rung
    // uses on the danger radius (threat reactivity, now reading personal enemies too): a coward
    // keeps its distance from further, the brave let a resented one get close. Neutral 0 (or no
    // Personality) -> kAvoidRadius exactly. No Relationships -> skipped -> the pre-grudge world is
    // bit-identical, and a positive/neutral tie (affinity > kGrudgeThreshold) falls through to the
    // gather rungs below.
    if (const Relationships* rel = reg.try_get<Relationships>(n)) {
      entt::entity rival = entt::null;
      Vec2 rival_pos{0.0f, 0.0f};
      // The base personal-space bubble, bravery-scaled. A NEMESIS (the deepest grudge) widens it by
      // kNemesisAvoidBoost — the negative twin of the Partner-defend teeth: your worst enemy gets
      // the widest berth. nearest_d starts at the boosted reach so the whole loop can consider a
      // distant Nemesis, but each rival is gated by ITS OWN radius (a mere grudge still only backs
      // off inside base_radius), so a non-Nemesis rival's behaviour is bit-identical.
      const float base_radius = kAvoidRadius * (1.0f - bravery / 200.0f);
      float nearest_d = base_radius * kNemesisAvoidBoost;
      for (const Relation& edge : rel->edges) {
        if (edge.affinity > kGrudgeThreshold || !reg.valid(edge.other) ||
            reg.all_of<Downed>(edge.other))
          continue;  // not resented enough, a stale handle, or a helpless DOWNED body — you don't
                     // flee a body, you just don't help it (the rescue veto in handle_deaths covers
                     // that). This also keeps the grudge-holder-won't-rescue behaviour unchanged.
        const Transform* t = reg.try_get<Transform>(edge.other);
        if (t == nullptr) continue;  // a rival with no position (shouldn't happen, cheap to guard)
        const float radius = edge.affinity <= kBondNemesisAt
                                 ? base_radius * kNemesisAvoidBoost  // Nemesis
                                 : base_radius;                      // mere grudge
        const float d = glm::distance(pos, t->position);
        if (d < radius && d < nearest_d) {
          nearest_d = d;
          rival = edge.other;
          rival_pos = t->position;
        }
      }
      if (rival != entt::null) {
        const Vec2 away = pos - rival_pos;  // AWAY from the resented one (bond-pull's mirror)
        const float len = glm::length(away);
        if (len > 0.0f) npcs.get<Velocity>(n).value = (away / len) * kRallySpeed * move_scale;
        continue;  // keeping its distance — skip the rally/bond gather rungs below
      }
    }

    // Priority 5.5 — PURSUE AN ASPIRATION: the PROACTIVE rung. Every want above is a REACTION —
    // flee a threat, rescue a friend, feed a need, mend a wound, avoid a rival. This one is a
    // DREAM: a colonist that carries an Aspiration, having nothing to fear or need (it reached this
    // far down the ladder), goes and PURSUES it. The WARRIOR goes LOOKING for a fight — steering
    // toward the nearest creature within kHuntRange to close and engage (it does NOT fight here:
    // npc_attack strikes the nearest creature in Strength-reach every tick, so once this charge
    // closes the gap the blows land on their own; this rung only supplies the intent). Sits ABOVE
    // the idle rally/bond/gather rungs (a colonist chases its dream rather than loiter by the fire)
    // but BELOW every need and fear (a hungry, cold, or wounded one tended that first — those rungs
    // already `continue`d), so the drive is self-limiting: a hurting warrior retreats and heals
    // (the P3.75 hearth rung) and only a hale, content one hunts. Creatures aren't a flee threat
    // for an un-panicked colonist (only Panicked ones bolt from them), so a warrior that charges in
    // STANDS and trades blows — glory or death, its choice. No Aspiration (every colonist today,
    // every existing test) -> try_get is null -> skipped -> bit-identical. Reuses the `creatures`
    // view (for the Warrior) and the `plots` view (for the Provider) fetched at the top of the
    // loop. The PROVIDER is the peaceful twin (below): it works the land instead of seeking battle.
    // Which dream a colonist carries picks the drive — a `switch` so -Wswitch flags any future kind
    // that forgets a rung. Each `continue` skips the idle rally/bond/gather rungs below.
    if (const Aspiration* asp = reg.try_get<Aspiration>(n); asp != nullptr) {
      switch (asp->kind) {
        case AspirationKind::Warrior: {
          entt::entity quarry = entt::null;
          float nearest_foe = kHuntRange;
          for (const entt::entity c : creatures) {
            const float d = glm::distance(pos, creatures.get<Transform>(c).position);
            if (d < nearest_foe) {
              nearest_foe = d;
              quarry = c;
            }
          }
          if (quarry != entt::null) {
            const Vec2 toward = creatures.get<Transform>(quarry).position - pos;
            const float len = glm::length(toward);
            if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kHuntSpeed * move_scale;
            continue;  // charging the fight — skip the idle rungs below
          }
          break;
        }
        case AspirationKind::Provider: {
          // Work the land: steer toward the nearest food plot that still has stock to gather (a
          // bare patch isn't worth the walk); npc_harvest reaps a RIPE one into a meal on arrival,
          // so a provider FARMS for the colony instead of loitering — the food-economy mirror of
          // the warrior's hunt. Reuses the `plots` view fetched for the forage rung above.
          // Self-limiting like the warrior: a hungry/cold/wounded provider tended that need on a
          // rung above.
          entt::entity plot = entt::null;
          float nearest_plot = kTendRange;
          for (const entt::entity pl : plots) {
            if (plots.get<FoodSource>(pl).stock <= 0.0f)
              continue;  // a picked-bare patch isn't work
            const float d = glm::distance(pos, plots.get<Transform>(pl).position);
            if (d < nearest_plot) {
              nearest_plot = d;
              plot = pl;
            }
          }
          if (plot != entt::null) {
            const Vec2 toward = plots.get<Transform>(plot).position - pos;
            const float len = glm::length(toward);
            if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kTendSpeed * move_scale;
            continue;  // heading to the field to work — skip the idle rungs below
          }
          break;
        }
        case AspirationKind::Scholar: {
          // Dreams of magic: an idle colonist that hasn't yet LEARNED to cast walks to the nearest
          // Spellbook to study it (study_spellbooks teaches Spellcasting on arrival) — so a Scholar
          // ASPIRES to magic, seeks the tome, and EMERGES a caster (npc_cast/npc_heal then drive
          // it), the knowledge mirror of the warrior's hunt and the provider's harvest. Once it
          // carries Spellcasting the dream is FULFILLED, so it stops seeking books and falls to the
          // idle rungs (casting on its own). Self-limiting like the others: a hungry/cold/wounded
          // scholar tended that need on a rung above. A colonist with no Skills sheet counts as
          // unlearned -> keeps seeking until a tome teaches it.
          if (const Skills* sk = reg.try_get<Skills>(n);
              sk != nullptr && sk->find(SkillId::Spellcasting) != nullptr)
            break;  // already a mage — dream fulfilled, no tome to seek
          entt::entity tome = entt::null;
          float nearest_tome = kStudyRange;
          for (const entt::entity b : books) {
            const float d = glm::distance(pos, books.get<Transform>(b).position);
            if (d < nearest_tome) {
              nearest_tome = d;
              tome = b;
            }
          }
          if (tome != entt::null) {
            const Vec2 toward = books.get<Transform>(tome).position - pos;
            const float len = glm::length(toward);
            if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kStudySpeed * move_scale;
            continue;  // off to the library — skip the idle rungs below
          }
          break;
        }
      }
    }

    // Priority 6 — RALLY: the hero twin of the villain-fear at the top of the ladder. Reached only
    // by a truly IDLE colonist (nothing to flee, rescue, forage, drink, mend, arm toward, or
    // avoid), it drifts toward a nearby renowned-enough hero — a player whose deeds have earned
    // standing at or above the "Known" line (+kKnownAt), the exact mirror of the -kKnownAt villain
    // it flees at the top. The colony gathers around its champion. INVERTED from fear (toward, not
    // away) and LOWEST priority (never overrides a real need — a hungry or endangered colonist
    // ignores the hero). Player-only (only a player earns standing today); no ledger or standing <
    // kKnownAt -> no pull, so a neutral/villain player draws nobody and the pre-hero world is
    // bit-identical.
    //
    // SOCIABILITY (the fifth axis) scales the rally RADIUS, reusing industry's radius SHAPE on this
    // social want — so the rally rung, like every other acting rung, now reads a trait. A sociable
    // colonist (+) crosses the field to gather round a champion; a loner (-) rallies only to one
    // practically underfoot. Neutral 0 -> kRallyRadius exactly (bit-identical). Reuses the `pers`
    // fetched at the top of the loop; cast to float before the divide (-Wconversion).
    const float sociability = pers != nullptr ? static_cast<float>(pers->sociability) : 0.0f;
    entt::entity champion = entt::null;
    float nearest_hero = kRallyRadius * (1.0f + sociability / 200.0f);
    // Any renowned ENTITY is a champion, not just the player: an NPC earns positive standing
    // exactly as a player does (Valor on a kill, Charity on a rescue -- neither is
    // PlayerControlled-gated, see perform_attack / handle_deaths), so the design's "players are
    // symmetric subjects" cuts both ways -- the colony rallies to a famous NPC the same way it
    // rallies to a famous player. The view is now every ledgered entity (the only ones that CAN be
    // renowned); the standing < kKnownAt filter below still gates it, so a world with only a
    // renowned player is bit-identical (an NPC with no ledger or below the Known line was never a
    // champion and still isn't). The self-skip matters now that the rallier can itself be a hero:
    // without it a renowned NPC would lock onto its OWN fame (distance 0), stall on the zero-vector
    // guard, and skip its bond-follow below.
    auto heroes = reg.view<BehaviorLedger, Transform>();
    for (const entt::entity h : heroes) {
      if (h == n) continue;  // you don't rally to your own fame
      if (standing(heroes.get<BehaviorLedger>(h)) < kKnownAt)
        continue;  // not a hero worth rallying to
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

    // Priority 7 — BOND: the PERSONAL twin of the hero-rally, the first reader of the P8
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
    // `pers` fetched at the top of the loop; cast to float before the divide (-Wconversion). Every
    // acting rung reads a trait and all six axes are wired; the hearth-gather rung below reads
    // sociability a SECOND way.
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
        continue;  // following a bonded friend — don't also amble to the hearth below
      }
    }

    // Priority 8 (the LAST rung) — HEARTH GATHER: with nothing to flee, rescue, forage, drink,
    // mend, arm toward, avoid, rally to, or a bonded friend to follow, a SOCIABLE colonist ambles
    // to the nearest fire to gather round it — the hearth as a peacetime social HUB, the twin of
    // the wounded-retreat rung that makes it a field hospital. Reads SOCIABILITY a SECOND way (its
    // first is the rally radius above): the gather radius is PROPORTIONAL to sociability (reusing
    // the `sociability` fetched for the rally rung), so a very sociable colonist crosses
    // kHearthGatherRadius to the fire, a mildly sociable one only ambles over from nearby, and a
    // neutral, solitary, or Personality-less colonist has a 0-or-negative radius and so NEVER
    // gathers — the indifferent keep to themselves, which is also what keeps the pre-gather world
    // bit-identical. Once AT a fire it HOLDS (velocity 0), the twin of the wounded-retreat rung's
    // hold: steer_npcs never damps velocity and integrate_motion never decays it, so without the
    // hold a gathered colonist would carry its inbound velocity straight through the fire and out
    // the far side, then re-aim and re-enter — a perpetual oscillation that never lets it rest (no
    // stamina recovery, needs draining at the moving rate). Scaled by move_scale like every rung (a
    // tired colonist trudges to the fire too); reuses the `hearths` view and kRallySpeed.
    const float gather_radius = kHearthGatherRadius * (sociability / 100.0f);
    if (gather_radius > 0.0f) {
      if (in_a_hearth(reg, pos)) {
        npcs.get<Velocity>(n).value = Vec2{0.0f, 0.0f};  // arrived — hold by the fire, don't coast
      } else {
        Vec2 fire_pos{0.0f, 0.0f};
        bool has_fire = false;
        float nearest_fire = gather_radius;
        for (const entt::entity h : hearths) {
          const float d = glm::distance(pos, hearths.get<Transform>(h).position);
          if (d < nearest_fire) {
            nearest_fire = d;
            fire_pos = hearths.get<Transform>(h).position;
            has_fire = true;
          }
        }
        if (has_fire) {
          const Vec2 toward = fire_pos - pos;
          const float len = glm::length(toward);
          if (len > 0.0f) npcs.get<Velocity>(n).value = (toward / len) * kRallySpeed * move_scale;
        }
      }
    }
  }
}

void npc_guard(entt::registry& reg) {
  // A hardened colonist — a BULWARK (trained Endurance) — PLANTS and raises a guard when a creature
  // is upon it, rather than trading open-stance blows: it emplaces Blocking, so resolve_creature_
  // contacts SOFTENS the hit, lets it RIPOSTE, and — the point — trains the GUARDING skill. That is
  // the first path for an NPC to reach Guarding at all: until now only the player set Blocking (via
  // MovePlayer), so the skill was unreachable for colonists — a real player==NPC parity hole. Gated
  // on a veteran Endurance level a FRESH NPC (level 1 — every existing test) never meets, so no
  // Blocking is ever raised in a fresh world -> bit-identical; the guard only emerges once a
  // colonist has toughened up over a long run. Rooted while guarding (velocity 0), mirroring the
  // player's guard-root, so a bulwark holds the line rather than kiting. Drops the stance when no
  // creature is near. MUST run AFTER steer_npcs (to override the velocity it set) and before
  // integrate_motion + resolve_creature_contacts. No RNG. Only touches the Npc view, so a guarding
  // PLAYER (its own Blocking, set by MovePlayer) is never disturbed.
  constexpr int kBulwarkLevel = 3;  // the Endurance level at which a colonist will stand and tank
  constexpr float kGuardRange =
      30.0f;  // a creature within this raises the guard (just past contact)
  constexpr float kGuardHealthFloor =
      0.5f;  // ...but only while HALE: below half health it retreats to heal instead of tanking
  auto npcs = reg.view<Npc, Transform, Attributes, Velocity>();
  auto creatures = reg.view<Enemy, Transform>();
  for (const entt::entity n : npcs) {
    bool raise = false;
    // A bulwark holds the line only while HALE. A WOUNDED one (below kGuardHealthFloor) does NOT
    // guard — it lets steer_npcs' wounded-retreat rung stand, falling back to a hearth to mend (a
    // hearth wards creatures, so that retreat is a real escape). So planting-and-tanking is the
    // brave choice of a healthy veteran, not a suicidal stand at low HP — and it never overrides
    // the heal-retreat. No Stats (a bare test fixture) -> treated as hale, so it still guards.
    const Stats* st = reg.try_get<Stats>(n);
    const bool hale = st == nullptr || st->health.current > st->health.max * kGuardHealthFloor;
    if (hale && npcs.get<Attributes>(n).endurance.level >= kBulwarkLevel) {  // hardy AND unhurt?
      const Vec2 pos = npcs.get<Transform>(n).position;
      for (const entt::entity c : creatures) {
        if (glm::distance(pos, creatures.get<Transform>(c).position) < kGuardRange) {
          raise = true;  // a creature is on it — plant and block
          break;
        }
      }
    }
    if (raise) {
      reg.emplace_or_replace<Blocking>(n);
      npcs.get<Velocity>(n).value = Vec2{0.0f, 0.0f};  // hold the line while guarding
    } else {
      reg.remove<Blocking>(n);  // no threat (or too green) -> lower the guard (a no-op if none)
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
      // THE HEARTH WARDS the beast: a person standing in the fire's glow is NOT hunted — the flames
      // keep creatures at bay, so a colonist that reaches the hearth is truly SAFE (not merely
      // healing faster there). This makes the wounded-retreat-to-hearth rung (steer_npcs) a real
      // escape and gives the base-building fire a DEFENSIVE purpose, not just a recovery one.
      // Sharing in_a_hearth with regenerate_vitals means "healed by the fire" and "hidden from the
      // hunt" are the exact same reach. A sheltered person is skipped, so the creature hunts the
      // nearest one still in the open, or idles if all are sheltered (ponytail: the fire is a small
      // spot, not a fortress — needs still drain, so you can't camp it forever). No hearths ->
      // nobody skipped -> bit-identical to before.
      if (in_a_hearth(reg, prey.get<Transform>(person).position)) continue;
      const float d = glm::distance(c_pos, prey.get<Transform>(person).position);
      if (target == entt::null || d < nearest) {
        nearest = d;
        target = person;
      }
    }
    if (target == entt::null) continue;  // nobody left to hunt — keep drifting

    // A WOUNDED creature LIMPS: below kLimpThreshold of its HP — the SAME 0.3 fraction the enrage
    // rung uses — it chases at kLimpMoveScale, the creature-side mirror of the player's exhaustion
    // crawl. So the sub-30% band is a sharp risk/reward from BOTH sides: a cornered beast RAGES
    // (enrage, harder hits) yet STRUGGLES to move (this), so you commit to the finish or KITE the
    // limping brute. Reads its OWN health (every creature carries Stats, like enrage), pure sim (no
    // RNG). A full-HP creature is unchanged -> bit-identical (the common case and every chase
    // test).
    constexpr float kLimpThreshold = 0.3f;  // below this fraction of its HP a creature limps...
    constexpr float kLimpMoveScale = 0.6f;  // ...and chases at this fraction of its speed (knobs)
    float chase_scale = 1.0f;
    if (const Stats* cs = reg.try_get<Stats>(c);
        cs != nullptr && cs->health.current < cs->health.max * kLimpThreshold) {
      chase_scale = kLimpMoveScale;
    }

    // Home in at its OWN chase_speed — a brute lumbers, a swarmer sprints. All are slower
    // than the player's 320 top speed, so a fight is always kite-able.
    const Vec2 toward = prey.get<Transform>(target).position - c_pos;
    const float len = glm::length(toward);
    if (len > 0.0f)
      creatures.get<Velocity>(c).value =
          (toward / len) * creatures.get<Enemy>(c).chase_speed * chase_scale;
  }
}

namespace {
// The MOVEMENT multiplier at a position: the STICKIEST MireZone it sits inside (the smallest
// slow_factor), or 1.0 on firm ground. Picking the MIN is order-independent, so overlapping bogs
// stay deterministic. No MireZone in the world -> the view is empty -> 1.0. A file-local helper for
// integrate_motion.
float mire_factor(entt::registry& reg, Vec2 pos) {
  float factor = 1.0f;  // firm ground; the deepest mud it's standing in drags it down most
  auto mires = reg.view<MireZone, Transform>();
  for (const entt::entity z : mires) {
    const MireZone& mire = mires.get<MireZone>(z);
    if (glm::distance(pos, mires.get<Transform>(z).position) <= mire.radius &&
        mire.slow_factor < factor) {
      factor = mire.slow_factor;
    }
  }
  return factor;
}
}  // namespace

void integrate_motion(entt::registry& reg, float dt) {
  // The classic update: new position = old position + velocity * time. Runs over every entity that
  // has both a Transform and a Velocity, and no others — that automatic filtering is the ECS's core
  // convenience.
  //
  // A MIRE drags on it: a mover standing in a boggy MireZone advances at that mire's slow_factor
  // this tick — so people, creatures, AND ambient motes all crawl through the mud (parity: mud
  // doesn't care who you are; kite a brute through it, or get caught fleeing across it). Crucially
  // the drag scales the MOVEMENT (the position delta), NOT the stored Velocity. That is what makes
  // it safe for a mover NOTHING re-drives each tick — an ambient mote, or an idle loner steer_npcs
  // left alone (e.g. a sociability<=0 colonist that matched no want-rung): it keeps its drift
  // velocity and simply crawls THROUGH the mud at a steady slow_factor and exits, rather than
  // having its velocity multiplied down in place every tick to a frozen stop it never escapes. (An
  // earlier version scaled the velocity itself and had to exclude motes to dodge that compounding
  // freeze — but idle loners hit it too; scaling the delta fixes it for every mover at once.) A
  // RE-driven mover (creature/steered NPC/commanded player) travels the same distance either way
  // (to within float rounding), and every velocity-reading system downstream (update_stamina,
  // drain_hunger — both binary moving/still) sees the true heading, so a mired crawler still counts
  // as MOVING (it's exerting hard for little ground). No MireZone in the world -> mire_factor
  // is 1.0 -> position += velocity
  // * dt exactly (x * 1.0f == x, so bit-identical). No RNG.
  auto view = reg.view<Transform, Velocity>();
  for (const entt::entity e : view) {
    Transform& tf = view.get<Transform>(e);
    tf.position += view.get<Velocity>(e).value * dt * mire_factor(reg, tf.position);
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
  // A HEARTH multiplies the health regen of anyone resting within its radius — the base-building
  // recovery seed. Modest, and NOT an invincible camp even though chase_prey won't hunt you into
  // the fire: a spitter still lobs venom from range and a beast already on top of you gets its last
  // licks (only the CHASE breaks), plus you're rooted (can't kite or forage while healing) and
  // needs keep draining. So the fire buys breathing room between fights, not a place to win from.
  constexpr float kHearthRegenBoost = 2.0f;  // ponytail: playtest knob

  // view<Stats>() iterates exactly the entities that have stats — the player
  // here, not the drifting motes — so this can't touch anything without them.
  // That automatic filtering is the ECS's whole point: behaviour applies to
  // whoever has the right data, nobody else. A DOWNED player is excluded: they
  // lie at 0 HP for the whole helpless window, so a trickle of self-heal must not
  // quietly lift them off the floor — only a rescue or respawn brings them back.
  auto view = reg.view<Stats>(entt::exclude<Downed>);
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);

    // MANA regenerates steadily — the magic bar refilling between casts (magic_bolt spends it). It
    // sits BEFORE the starvation/venom gate below deliberately: that gate suppresses HEALING (a
    // fed- and-clean body mends), but magic energy isn't food, so a starving mage still recharges.
    // No one reads mp unless they can cast, so this is bit-identical for every non-caster. Downed
    // is already excluded by the view (an unconscious caster doesn't recharge).
    recover(s.mp, dt);

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
    // ...nor while POISONED: venom SUPPRESSES healing (the classic poison rule), so its chip can't
    // be clawed back by a fed character's regen — that's what makes the lingering bite a real
    // threat. tick_poison runs before this system and reaps expired Poisoned, so the flag here
    // means active venom. Hardiness (VIT) still blunts the venom — but DIRECTLY, in tick_poison
    // (which shaves the chip by Endurance), not here; this system only gates the regen off.
    if (s.hunger.current <= 0.0f || s.water.current <= 0.0f || s.warmth.current <= 0.0f ||
        reg.all_of<Poisoned>(e))
      continue;  // ...and FREEZING suppresses healing too (drain_warmth runs before this), so cold
                 // nets health strictly down like the other needs. Warmth is full unless a ColdZone
                 // exists, so this clause is dormant (bit-identical) in a world without cold.

    // Tougher characters heal faster (VIT). No Attributes -> boost 1.0 (bit-identical to
    // before), so creatures and bare entities are unchanged. Same shape as update_stamina.
    const Attributes* attrs = reg.try_get<Attributes>(e);
    float boost = attrs != nullptr ? 1.0f + static_cast<float>(attrs->endurance.level - 1) *
                                                kHealthRegenPerEndurance
                                   : 1.0f;
    // ...and faster still by a HEARTH: a colonist resting within one's radius mends quicker (stacks
    // on the VIT boost). Shares the in_a_hearth reach with chase_prey's ward, so "healed by the
    // fire" and "hidden from the hunt" are the same glow. No hearth in reach (or none exist) ->
    // x1.0, bit-identical to before. Reads the entity's own Transform; a Stats entity without one
    // skips it.
    if (const Transform* tf = reg.try_get<Transform>(e);
        tf != nullptr && in_a_hearth(reg, tf->position))
      boost *= kHearthRegenBoost;
    recover(s.health, dt, boost);
    // Health only ever ticks back up, so it recovers here. Stamina is different —
    // it's spent by moving — so it has its own system (update_stamina) instead of
    // a passive line here.
  }
}

void mend_gear(entt::registry& reg, float dt) {
  // The base MENDS your kit: worn weapons and armour slowly regain durability while their bearer
  // stands in a Hearth's warmth — the "repair later" the durability comment promised, and the FIRST
  // way durability climbs instead of only wearing toward a shatter. So gear is a MANAGED resource
  // now (fight -> it wears -> mend it at the fire), not a one-way trip to breaking. Shares
  // in_a_hearth with the heal/stamina boosts and the creature-ward, so "mended by the fire" is the
  // exact reach that heals, rests, and hides. Only a WORN slot (0 < durability < max) mends: a full
  // slot is capped (no over-repair past new) and an EMPTY slot (durability 0 = no weapon/armour) is
  // left alone — the fire can't conjure gear from nothing, only maintain what you carry. No hearth
  // in reach -> untouched, so a hearthless world is bit-identical. Pure float, no RNG. A DOWNED
  // bearer is excluded (like regenerate_vitals' heal this parallels): a crumpled body on the floor
  // isn't tending its kit, and — unlike the stamina/needs a revive resets — a durability gain would
  // PERSIST past the down window, so an inert body must not repair. Keeps the "a Downed body is
  // inert" invariant whole here too.
  constexpr float kMendPerSecond = 0.5f;  // durability points the fire restores per second (a knob)
  for (const entt::entity e : reg.view<Equipped, Transform>(entt::exclude<Downed>)) {
    if (!in_a_hearth(reg, reg.get<Transform>(e).position)) continue;
    Equipped& eq = reg.get<Equipped>(e);
    if (eq.weapon_durability > 0.0f && eq.weapon_durability < kWeaponMaxDurability) {
      eq.weapon_durability += kMendPerSecond * dt;
      if (eq.weapon_durability > kWeaponMaxDurability) eq.weapon_durability = kWeaponMaxDurability;
    }
    if (eq.armour_durability > 0.0f && eq.armour_durability < kArmourMaxDurability) {
      eq.armour_durability += kMendPerSecond * dt;
      if (eq.armour_durability > kArmourMaxDurability) eq.armour_durability = kArmourMaxDurability;
    }
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
  // ...and the RECOVERY skill's OWN direct effect: each Recovery level past the first speeds the
  // second wind a little MORE, on top of the Endurance that resting also feeds it. This is the
  // design pattern where a skill matters BOTH through its main attribute AND by its own level (the
  // twin of Survivalist, which feeds Endurance yet ALSO reads its own level to relieve the fatigue
  // drain). Deliberately HALF of the Endurance rate so the two paths (Recovery -> Endurance ->
  // recovery, and Recovery -> recovery directly) compound gently, not explosively — a knob.
  constexpr float kRecoveryPerLevel = 0.05f;
  // Resting IN a Hearth's warmth recovers stamina this much faster — the stamina twin of the health
  // regen boost regenerate_vitals gives there (same 2.0 knob), so the fire is a FULL recovery spot:
  // mend AND catch your breath. A playtest knob.
  constexpr float kHearthStaminaBoost = 2.0f;

  // Stats + Velocity = things that both tire and move. Motes have Velocity but no
  // Stats, so the view skips them for free. exclude<Downed> keeps the "a Downed body is inert"
  // invariant literally true — a crumpled body neither spends nor recovers stamina (harmless in
  // practice, since revive resets stamina to max regardless, but it mustn't tick while helpless).
  // Only the player ever goes Downed, so this changes nothing for anyone standing (bit-identical).
  auto view = reg.view<Stats, Velocity>(entt::exclude<Downed>);
  for (const entt::entity e : view) {
    Stats& st = view.get<Stats>(e);
    Vital& stamina = st.stamina;
    if (glm::length(view.get<Velocity>(e).value) > 0.0f) {
      stamina.current -= kDrainPerSecond * dt;  // moving: spend it...
      // ...and a SPRINT burns it FASTER (kSprintDrainBonus on top of the base rate), so a dash is a
      // short burst that ends in the exhaustion crawl, not a free faster pace. Sprinting is set by
      // the same-tick MovePlayer command (before this system runs), exactly like Blocking below;
      // only a moving sprinter pays it, and no Sprinting stance -> the base drain -> bit-identical.
      if (reg.all_of<Sprinting>(e)) stamina.current -= kSprintDrainBonus * dt;
      if (stamina.current < 0.0f) stamina.current = 0.0f;  // ...never below empty
    } else {
      // Resting: recover, faster the tougher you are — but NOT on an empty stomach or canteen, NOR
      // while FROZEN. A starving, dehydrated, or freezing character gets no second wind: the
      // stamina twin of regenerate_vitals' heal-gate (which blocks healing on all three), so ANY
      // survival failure saps your reserves too, not just your health — a freezing rester is as
      // spent as a starving one, not mysteriously refilling its stamina by the cold. And, composed
      // with the stamina==0 exhaustion crawl, a spent character who flees tires to a crawl it can't
      // shake off — the design's "escalating inefficiency" emerging from two systems, no new
      // penalty. update_stamina runs just BEFORE drain_hunger/drain_water/drain_warmth in step(),
      // so this reads last tick's need level: a 1-frame lag, immaterial for a Need that empties
      // over minutes. Creatures default hunger/water/warmth to full (100), so they're never gated
      // here; and warmth only reaches 0 inside a ColdZone, so a world without one is bit-identical.
      if (st.hunger.current <= 0.0f || st.water.current <= 0.0f || st.warmth.current <= 0.0f)
        continue;
      // Nor behind a raised GUARD: bracing to turn blows is exertion, not rest — you get no second
      // wind while Blocking. So a guard-tank bleeds the stamina its ripostes spend (see
      // resolve_creature_contacts) and can't refill it until it lowers the guard, making a
      // prolonged hold a rhythm rather than a free stand-and-win. Only the player guards today, and
      // Blocking is set by the same-tick MovePlayer command before this system runs.
      if (reg.all_of<Blocking>(e)) continue;
      // A no-Attributes entity just uses the base rate (boost 1.0).
      const Attributes* attrs = reg.try_get<Attributes>(e);
      float boost = attrs != nullptr ? 1.0f + static_cast<float>(attrs->endurance.level - 1) *
                                                  kRecoveryPerEndurance
                                     : 1.0f;
      // The RECOVERY skill's own second wind: a practised rester catches its breath faster still,
      // read DIRECTLY from the skill level (the Survivalist pattern). No Recovery skill, or level 1
      // (the spawn default and every non-progressing entity), -> ×1.0 -> the exact rate above, so a
      // fresh world is bit-identical. Composes multiplicatively with the Endurance boost.
      if (const Skills* sk = reg.try_get<Skills>(e))
        if (const Skill* rec = sk->find(SkillId::Recovery))
          boost *= 1.0f + static_cast<float>(rec->level - 1) * kRecoveryPerLevel;
      // Worn armour's BANE: plate slows your second wind. A fraction of recovery is lost while
      // armoured. ponytail/BALANCE: this bites only while RESTING (combat is spent moving), so
      // armour can feel near-free in a straight fight — a tuning knob; if it plays as pure
      // upside, move the bane onto the drain-while-moving side instead. No/empty armour = 0. VIT
      // eases it (borne_regen_penalty): a hardy body BEARS armour better, so its Endurance shrinks
      // the penalty up to half — the armour twin of STR's weapon carry. Reuses the `attrs` fetched
      // for the recovery boost above; Endurance 1 or no Attributes -> the full bane ->
      // bit-identical.
      const Equipped* eq = reg.try_get<Equipped>(e);
      if (eq != nullptr) boost *= 1.0f - borne_regen_penalty(eq->stamina_regen_penalty, attrs);
      // A HEARTH speeds your second wind too: resting in its warmth recovers stamina faster (the
      // stamina twin of regenerate_vitals' fireside health boost), so the fire is a place to FULLY
      // recover, not just heal. Needs the rester's position (the view lacks Transform); no
      // Transform or no hearth in range -> base rate, so a hearthless world is bit-identical.
      // Shares in_a_hearth with regenerate_vitals' heal boost AND chase_prey's ward, so all three
      // fireside checks read the SAME radius — one predicate, no drift. Applied over the armour
      // bane so a plated rester still catches its breath faster by the fire (the boost and bane
      // compose).
      if (const Transform* tf = reg.try_get<Transform>(e);
          tf != nullptr && in_a_hearth(reg, tf->position))
        boost *= kHearthStaminaBoost;
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
  constexpr float kExertionDrainPerSecond = 0.3f;  // added while moving (walking)
  constexpr float kSprintNeedExertion = 0.3f;      // added AGAIN while sprinting (the top tier)
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
    // ...and a SPRINT costs extra AGAIN — the exertion tier above walking (rest < walk < sprint),
    // the need-twin of update_stamina's sprint burn, so a dash across the map arrives hungrier than
    // a walk. Sprinting is a player-set stance (MovePlayer); no Sprinting stance -> the walk drain
    // -> bit-identical. A sprinter is always moving, so this stacks on the move tier.
    if (reg.all_of<Sprinting>(e)) drain += kSprintNeedExertion;
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
  constexpr float kExertionDrainPerSecond = 0.3f;  // added while moving (walking)
  constexpr float kSprintNeedExertion = 0.3f;      // added AGAIN while sprinting (the top tier)
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
    // ...and a SPRINT costs extra AGAIN (the tier above walking) — the twin of the hunger sprint
    // drain, so exertion tiers rest < walk < sprint apply to thirst too. No Sprinting ->
    // bit-identical.
    if (reg.all_of<Sprinting>(e)) drain += kSprintNeedExertion;
    s.water.current -= drain * dt;
    if (s.water.current < 0.0f) s.water.current = 0.0f;  // never below empty

    if (s.water.current <= 0.0f) {  // dehydrating — the same death path as starving
      s.health.current -= kDehydrationPerSecond * dt;
      if (s.health.current < 0.0f) s.health.current = 0.0f;
    }
  }
}

void drain_warmth(entt::registry& reg, float dt) {
  // WARMTH — the LOCALIZED Need, the design's temperature made spatial. Unlike hunger/water (a
  // background timer) it moves only by WHERE you stand: a Hearth's fire RE-WARMS you, a ColdZone
  // CHILLS you, the open holds steady. So it drives a "flee the cold, huddle by the fire" loop
  // rather than a "feed me" clock. At 0 it FREEZES — chipping health through the same death path as
  // starving and dehydrating (regenerate_vitals also gates healing off while frozen). The fire
  // beats the cold: a hearth INSIDE a cold zone still warms. Same set as the other needs (every
  // Stats-bearer that isn't a creature). No ColdZone in the world -> nobody ever chills -> warmth
  // stays full and untouched (a hearth just re-clamps a full bar) -> bit-identical.
  constexpr float kColdDrainPerSecond = 3.0f;  // how fast a ColdZone saps you (a real, felt bite)
  constexpr float kWarmRecoverPerSecond =
      6.0f;                                  // ...and how fast a fire mends it (a haven, faster)
  constexpr float kFreezePerSecond = 12.0f;  // health lost per second once warmth hits 0
  auto view = reg.view<Stats>(entt::exclude<Enemy>);
  auto zones = reg.view<ColdZone, Transform>();
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);
    const Transform* tf = reg.try_get<Transform>(e);
    if (tf != nullptr && in_a_hearth(reg, tf->position)) {
      s.warmth.current += kWarmRecoverPerSecond * dt;  // by the fire: re-warm...
      if (s.warmth.current > s.warmth.max) s.warmth.current = s.warmth.max;
    } else if (tf != nullptr) {
      bool cold = false;  // ...else in a cold zone: chill (the open holds steady)
      for (const entt::entity z : zones) {
        if (glm::distance(tf->position, zones.get<Transform>(z).position) <
            zones.get<ColdZone>(z).radius) {
          cold = true;
          break;
        }
      }
      if (cold) {
        s.warmth.current -= kColdDrainPerSecond * dt;
        if (s.warmth.current < 0.0f) s.warmth.current = 0.0f;  // never below empty
      }
    }

    if (s.warmth.current <= 0.0f) {  // freezing — the same death path as starving/dehydrating
      s.health.current -= kFreezePerSecond * dt;
      if (s.health.current < 0.0f) s.health.current = 0.0f;
    }
  }
}

void tick_fatigue(entt::registry& reg, float dt) {
  // Fatigue is the ODD need: it falls while you EXERT and RECOVERS while you rest — the "you can't
  // run forever" pressure, on a slow (minutes) background timescale like hunger/water. The same
  // exertion tiers the other needs use (rest < walk < sprint), but here the base tier is RECOVERY,
  // not a drain: standing still MENDS fatigue, moving spends it, sprinting spends it faster. Gentle
  // knobs so a colonist that mixes moving with resting stays rested, and only sustained exertion
  // wears it down. Nothing READS fatigue yet — the empty-collapse consequence and the sit/sleep
  // faster-rest tiers are the next slices; this seam just makes the bar move.
  constexpr float kRestRecoverPerSecond = 0.5f;  // regained per second while standing still...
  constexpr float kMoveDrainPerSecond = 0.4f;    // ...spent per second while moving (walking)...
  constexpr float kSprintDrainPerSecond =
      0.4f;  // ...and again on top while sprinting (the top tier)
  // Rest is DEEPEST by the fire: resting in a Hearth's warmth recovers fatigue this much faster —
  // the design's "sleep fast" tier, realized through the existing safe rest spot rather than a new
  // sit/sleep stance, and the fatigue twin of the health/stamina hearth boosts (same 2.0 knob). So
  // the hearth is now a FULL recovery hub — health, stamina, gear, AND fatigue. No hearth in reach
  // -> the base rate -> bit-identical.
  constexpr float kHearthFatigueBoost = 2.0f;
  // Every PERSON tires — the player and NPCs (Stats without the Enemy marker), the same set the
  // other needs drain. Creatures are pure combat foes, no fatigue bar.
  auto view = reg.view<Stats>(entt::exclude<Enemy>);
  for (const entt::entity e : view) {
    Stats& s = view.get<Stats>(e);
    const Velocity* v = reg.try_get<Velocity>(e);
    if (v != nullptr && glm::length(v->value) > 0.0f) {
      // SURVIVALIST eases the drain — a trained survivor tires SLOWER, lengthening the fatigue
      // timer (never removing it: eased_bane caps the relief at half). This is the design's growth
      // source and the ONE thing that buffers a need — VIT/Endurance stays pure combat defence, so
      // survival stamina is its own trained thing. `relief` is the (1 - eased) multiplier: no
      // Survivalist skill (level 1, or none) -> eased_bane(1, level) = 1.0 -> the FULL drain, so
      // the two subtractions below are EXACTLY the pre-Survivalist ones (bit-identical, walkers AND
      // sprinters — applying the factor per-component keeps the old float rounding). Trained in
      // advance_progression, gated on low fatigue: you learn to endure only by pushing into
      // exhaustion.
      float relief = 1.0f;
      if (const Skills* sk = reg.try_get<Skills>(e); sk != nullptr)
        if (const Skill* surv = sk->find(SkillId::Survivalist))
          relief = eased_bane(1.0f, surv->level);
      s.fatigue.current -= kMoveDrainPerSecond * relief * dt;  // moving spends fatigue...
      if (reg.all_of<Sprinting>(e))
        s.fatigue.current -= kSprintDrainPerSecond * relief * dt;  // ...sprint more
    } else {
      float recover_rate = kRestRecoverPerSecond;  // standing still mends it (the odd need)...
      if (const Transform* t = reg.try_get<Transform>(e);
          t != nullptr && in_a_hearth(reg, t->position))
        recover_rate *= kHearthFatigueBoost;  // ...and the fire's warmth mends it faster still
      s.fatigue.current += recover_rate * dt;
    }
    if (s.fatigue.current < 0.0f) s.fatigue.current = 0.0f;                    // never below empty
    if (s.fatigue.current > s.fatigue.max) s.fatigue.current = s.fatigue.max;  // ...nor above full
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

// grant_skill_xp is a file-local helper defined further down (with attr_ref/skill_def); graze — a
// survival system that sits ABOVE them — forward-declares it here so a real graze trains Foraging
// through the SAME one funnel every other XP grant uses (skill + main attribute + character level),
// never a bypassed, drift-prone copy. In an anonymous namespace to match the definition's internal
// linkage. No default arg here (graze passes the CharacterLevel explicitly); the default lives on
// the definition.
namespace {
void grant_skill_xp(Skills& skills, Attributes& attrs, SkillId id, Fixed base,
                    CharacterLevel* character);
}  // namespace

void graze(entt::registry& reg, float dt) {
  // Hunger restored per second while grazing — and the stock the plot loses to give it. Fast enough
  // that a few seconds at a patch fills you, so it's a real "walk to the garden" destination. A
  // knob.
  constexpr float kFeedPerSecond = 40.0f;
  // WISDOM's first payoff: each Wisdom level past the first lifts a forager's YIELD by this
  // fraction (nature knowledge — you read the patch and take more). Level 1 -> 1.0 -> the base
  // rate, so a grazer with no Attributes or an untrained one eats exactly as before
  // (bit-identical). A knob.
  constexpr float kYieldPerWis = 0.05f;
  // A real graze also TRAINS Foraging -> Wisdom at this per-tick trickle (the food-plot mirror of a
  // loot grab training Scavenging -> Luck). Matches Conditioning's 20 XP/sec so grazing grows WIS
  // at the rate moving grows Endurance. (Fixed::from_ratio isn't constexpr, hence a plain const.)
  const Fixed kForagingPerTick = Fixed::from_ratio(20, 60);
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
      // A wiser forager draws more per tick — WIS scales the base rate. No Attributes (or level 1)
      // -> 1.0 -> the base rate (bit-identical). Reused for the training grant just below.
      Attributes* at = reg.try_get<Attributes>(e);
      const float wis_yield =
          at != nullptr ? 1.0f + static_cast<float>(at->wisdom.level - 1) * kYieldPerWis : 1.0f;
      float bite = kFeedPerSecond * wis_yield * dt;  // ...but no more than the eater needs or holds
      if (bite > room) bite = room;
      if (bite > fs.stock) bite = fs.stock;
      hunger.current += bite;
      fs.stock -= bite;

      // The eater LEARNED from this graze: train Foraging -> Wisdom (+ a little Character Level),
      // through the shared funnel. Only a grazer carrying the progression pair learns; a bare
      // Stats+Transform eater (every existing graze test) trains nothing and stays bit-identical.
      if (Skills* sk = reg.try_get<Skills>(e); sk != nullptr && at != nullptr) {
        grant_skill_xp(*sk, *at, SkillId::Foraging, kForagingPerTick,
                       reg.try_get<CharacterLevel>(e));
      }
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
    case AttrId::Wisdom:
      return a.wisdom;
    case AttrId::Charisma:
      return a.charisma;
    case AttrId::Intellect:
      return a.intellect;
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
  // Foraging is Scavenging's food-plot twin: a gathering skill, but WISDOM-led (nature knowledge)
  // where Scavenging is Luck-led (fortune). Main-only, no contributors — the first WIS skill.
  static const SkillDef kForaging{AttrId::Wisdom, {}};
  // Leadership is the first CHARISMA skill — the social twin of the combat skills: where Striking
  // builds Strength by hitting, Leadership builds Charisma by *inspiring* (felling a foe while
  // allies watch, bond_witnesses). Main-only, no contributors — a pure social strand, off the
  // fighter tree.
  static const SkillDef kLeadership{AttrId::Charisma, {}};
  // Teaching is Leadership's INSTRUCTIONAL twin — both build Charisma, one by public heroism, the
  // other by passing a mastered skill to a nearby novice (teach). A pure social strand, main-only.
  static const SkillDef kTeaching{AttrId::Charisma, {}};
  // Guarding is Toughness's ACTIVE twin: where Toughness builds Endurance by SURVIVING a hit,
  // Guarding builds it by TURNING one with a raised guard — the second VIT skill fed by defence,
  // both main-only. So a guard-tank grows genuinely tougher by tanking.
  static const SkillDef kGuarding{AttrId::Endurance, {}};
  // Resistance is Toughness's VENOM twin: Toughness builds Endurance by surviving a blow,
  // Resistance by enduring a poison tick — so a character that keeps shrugging off venom grows the
  // very VIT that shaves the venom (tick_poison), an immunity-through-exposure loop. Main-only, the
  // third VIT skill.
  static const SkillDef kResistance{AttrId::Endurance, {}};
  // Athletics is Conditioning's BURST twin: Conditioning builds Endurance from steady movement,
  // Athletics builds Dexterity from a SPRINT — so a character that dashes and kites a lot grows the
  // agility (DEX) that sharpens its dodge and aim. Main-only, a DEX skill beside Evasion and
  // Throwing.
  static const SkillDef kAthletics{AttrId::Dexterity, {}};
  static const SkillDef kSurvivalist{AttrId::Endurance, {}};  // a VIT skill, like Conditioning
  // Spellcasting is the first INTELLECT skill — the design's magic domain. Casting a bolt trains
  // it, and its main attr feeds the INT that scales the bolt's damage, so a mage sharpens by
  // casting the same way a thrower sharpens DEX by throwing. Main-only for now (aspects come with
  // the tree).
  static const SkillDef kSpellcasting{AttrId::Intellect, {}};
  // Healing is a WISDOM skill — the design's WIS Healing/Medicine domain, the SUPPORT twin of
  // Spellcasting's INT offence. Mending a wounded ally trains it, and its main attr feeds the WIS
  // that scales the mend, so a healer sharpens by healing the way a mage sharpens by casting.
  // Main-only for now (aspects come with the tree).
  static const SkillDef kHealing{AttrId::Wisdom, {}};
  // Cooking is a second INTELLECT skill beside Spellcasting — the design's INT Cooking domain (its
  // non-magic sibling). Preparing a meal trains it, and its main attr feeds the INT that scales the
  // meal, so a cook sharpens by cooking the way a mage sharpens by casting. Main-only for now.
  static const SkillDef kCooking{AttrId::Intellect, {}};
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
    case SkillId::Foraging:
      return kForaging;
    case SkillId::Leadership:
      return kLeadership;
    case SkillId::Guarding:
      return kGuarding;
    case SkillId::Resistance:
      return kResistance;
    case SkillId::Athletics:
      return kAthletics;
    case SkillId::Survivalist:
      return kSurvivalist;
    case SkillId::Spellcasting:
      return kSpellcasting;
    case SkillId::Teaching:
      return kTeaching;
    case SkillId::Healing:
      return kHealing;
    case SkillId::Cooking:
      return kCooking;
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

void teach(entt::registry& reg) {
  // MENTORSHIP: a colonist that has grown FAR ahead in a skill passes it to a nearby one who trails
  // — the design's "a higher-skill character teaches a lower one (boost XP)". So skills SPREAD
  // through the colony, not only from each person's own toil: a veteran's apprentice picks up the
  // craft standing beside them. The mentor grows its own TEACHING skill -> Charisma (leading by
  // instruction, the second CHA feeder beside Leadership). Gated on a skill-level GAP that CANNOT
  // exist at spawn — every skill starts at level 1, so no one is anyone's mentor until training
  // diverges over a long run; a fresh or short-run world teaches nothing -> bit-identical.
  // Player==NPC clean: any person teaches or learns (creatures have no Skills so they never
  // qualify; Downed bodies are excluded). grant_skill_xp mutates only component VALUES (xp/level, a
  // learned-skill push), never the entity's component SET, so mutating a student/mentor mid-walk
  // can't invalidate the folk view. O(folk x (their skills + folk)) — the grief loop's shape.
  // ponytail: a per-skill high-water index if the colony ever gets crowded.
  constexpr float kMentorReach = 40.0f;  // how close you stand to learn — a lesson, not a shout
  constexpr int kMentorLevel = 3;  // a mentor must be at least this good to have craft to pass
  constexpr int kMentorGap = 2;    // ...and the student at least this many levels behind it
  const Fixed kLessonPerTick = Fixed::from_ratio(15, 60);  // XP a student gains beside a mentor
  const Fixed kTeachingPerTick =
      Fixed::from_ratio(10, 60);  // ...and the mentor's own Teaching gains

  auto folk = reg.view<Skills, Attributes, Transform, CharacterLevel>(entt::exclude<Enemy, Downed>);
  for (const entt::entity mentor : folk) {
    // The mentor's BEST skill — the one it has most to pass on (which, and at what level). Teaching
    // ITSELF is skipped: you earn Teaching by DOING it (mentoring), never by being taught it, so a
    // career mentor teaches its best CRAFT, not "teaches teaching" (which would let a novice
    // acquire the skill passively, contradicting its earned-by-instruction flavour).
    SkillId best = SkillId::Conditioning;
    int best_level = 0;
    for (const auto& [id, skill] : folk.get<Skills>(mentor).owned) {
      if (id != SkillId::Teaching && skill.level > best_level) {
        best_level = skill.level;
        best = id;
      }
    }
    if (best_level < kMentorLevel) continue;  // too green to teach anyone

    // The nearest person within reach who trails the mentor in that skill by the gap (an unlearned
    // skill counts as level 1 — a total novice, the keenest to learn).
    const Vec2 mpos = folk.get<Transform>(mentor).position;
    entt::entity student = entt::null;
    float nearest = kMentorReach;
    for (const entt::entity s : folk) {
      if (s == mentor) continue;
      const float d = glm::distance(mpos, folk.get<Transform>(s).position);
      if (d >= nearest) continue;
      const Skill* has = folk.get<Skills>(s).find(best);
      const int s_level = has != nullptr ? has->level : 1;
      if (s_level > best_level - kMentorGap)
        continue;  // not far enough behind to gain from a lesson
      nearest = d;
      student = s;
    }
    if (student == entt::null) continue;  // no one nearby worth teaching this tick

    // The lesson: the student gains XP in the mentor's best skill (learning it at level 1 if new,
    // via grant_skill_xp), and the mentor grows TEACHING -> Charisma for passing it on. Note
    // whether the student ALREADY knew the craft, so a first-LEARN this tick can be detected below.
    const bool knew_craft = folk.get<Skills>(student).find(best) != nullptr;
    grant_skill_xp(folk.get<Skills>(student), folk.get<Attributes>(student), best, kLessonPerTick,
                   &folk.get<CharacterLevel>(student));
    grant_skill_xp(folk.get<Skills>(mentor), folk.get<Attributes>(mentor), SkillId::Teaching,
                   kTeachingPerTick, &folk.get<CharacterLevel>(mentor));

    // GRATITUDE: the tick a student LEARNS a craft it never had (the skill just appeared, 0 ->
    // level 1) bonds it to the mentor (kGratitudeAffinity) — the shared-events-forge-ties bond
    // beside camaraderie/admiration/grudge. Detected as first-LEARN, NOT a rank-up: grant_skill_xp
    // banks only XP, and levels are applied a step later by advance_progression, so within teach
    // the sole detectable breakthrough is the skill appearing (a rank-up would need to read after
    // that later system). Fires once per NEW craft passed (a skill is learned once); a mentor whose
    // best later becomes a SECOND craft the student lacks bonds it again. nudge_affinity is
    // view-safe: it only get_or_emplaces Relationships + writes a vector, adding a component
    // OUTSIDE the folk view's signature (Skills/Attributes/Transform/CharacterLevel) — the same
    // reason camaraderie/grudge nudge safely mid-system. A world with no first-learn (every
    // short-run fixture) forms no tie -> bit-identical.
    if (!knew_craft && folk.get<Skills>(student).find(best) != nullptr)
      nudge_affinity(reg, student, mentor, kGratitudeAffinity);
  }
}

void advance_progression(entt::registry& reg) {
  constexpr float kHealthPerEndurance = 10.0f;  // how much tougher each point makes you
  constexpr float kStaminaPerEndurance = 5.0f;
  constexpr float kMpPerEndurance =
      5.0f;  // VIT feeds the mana pool at stamina's rate (HP is the
             // survival pool, so it grows fastest; MP mirrors stamina)
  // XP earned per tick of activity. The timestep is fixed (1/60 s), so a
  // per-second rate is a constant per-tick Fixed amount — deterministic, no float
  // in the loop (20 XP/sec ÷ 60 ticks).
  const Fixed kConditioningPerTick = Fixed::from_ratio(20, 60);
  // Resting to recover spent stamina trains Recovery a touch slower than moving
  // trains Conditioning (15 XP/sec vs 20) — resting is easier than exerting.
  const Fixed kRecoveryPerTick = Fixed::from_ratio(15, 60);
  // Sprinting trains Athletics at the same rate moving trains Conditioning (20 XP/sec) — a
  // full-tilt dash is exertion in the AGILITY domain, granted ON TOP of Conditioning (you're moving
  // too).
  const Fixed kAthleticsPerTick = Fixed::from_ratio(20, 60);
  // Pushing into EXHAUSTION trains Survivalist -> Endurance at the same 20 XP/sec: once fatigue
  // drops below kExhaustionLearnAt you're learning to endure. Gated LOW so a rested colony (fatigue
  // high — every existing fixture) trains nothing, exactly like the sprint-Athletics gate
  // (bit-identical).
  const Fixed kSurvivalistPerTick = Fixed::from_ratio(20, 60);
  constexpr float kExhaustionLearnAt = 30.0f;  // fatigue below this = "pushing your limit"

  // NB: CharacterLevel is required here, so any new progression-capable entity must
  // be spawned WITH it (see world.cpp: player + NPC) — miss it and that entity
  // silently never grows, no error. Keep the progression components together.
  // exclude<Downed>: a crumpled body is INERT — it must not train while helpless. Its velocity is
  // zeroed at the down, so without this it would sit in the rest branch banking Recovery ->
  // Endurance
  // -> CharacterLevel XP every tick, and revive resets vitals but NOT skills/attributes, so that XP
  // would LEAK permanently (a knocked-out body grinding its veteran layer). The same invariant
  // regenerate_vitals / mend_gear / collect_pickups / tick_poison all enforce. Only the player ever
  // goes Downed (NPCs permadeath), so this changes nothing for anyone standing.
  auto view = reg.view<Skills, Attributes, Stats, Velocity, CharacterLevel>(entt::exclude<Downed>);
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
      // ...and a SPRINT (the burst stance, set by MovePlayer before this system runs) trains
      // Athletics -> Dexterity ON TOP: dashing and kiting build agility, the DEX mirror of steady
      // movement building Endurance. Only a SPRINTING mover trains it, so a walker — and any NPC,
      // which never sprints — is bit-identical (no Sprinting stance -> no grant), exactly like the
      // sprint stamina drain in update_stamina. Player-triggered like Throwing/Guarding.
      if (reg.all_of<Sprinting>(e))
        grant_skill_xp(view.get<Skills>(e), attrs, SkillId::Athletics, kAthleticsPerTick,
                       &character);
      // ...and pushing INTO exhaustion (fatigue below kExhaustionLearnAt) trains SURVIVALIST ->
      // Endurance: you learn to endure only by spending your reserves to the limit. Gated on LOW
      // fatigue, so a rested colony trains nothing here and its Endurance is bit-identical.
      // tick_fatigue reads the resulting Survivalist level to slow the drain (lengthening the
      // timer).
      if (view.get<Stats>(e).fatigue.current < kExhaustionLearnAt)
        grant_skill_xp(view.get<Skills>(e), attrs, SkillId::Survivalist, kSurvivalistPerTick,
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
    apply_levels(attrs.dexterity.level,
                 attrs.dexterity.xp);  // fed by Evasion (dodge), Throwing (aim), Athletics (sprint)
    apply_levels(attrs.luck.level, attrs.luck.xp);            // fed by Scavenging at the loot site
    apply_levels(attrs.wisdom.level, attrs.wisdom.xp);        // fed by Foraging at the graze site
    apply_levels(attrs.charisma.level, attrs.charisma.xp);    // fed by Leadership at bond_witnesses
    apply_levels(attrs.intellect.level, attrs.intellect.xp);  // fed by Spellcasting at magic_bolt
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
    // The design's "VIT governs HP/Stamina/MP": the mana pool grows off the SAME shape, so a
    // hardier caster carries a bigger reserve too — the third pool, no longer the one left flat. A
    // fresh character (Endurance 1 -> bonus 0) keeps mp.max at its base, so any non-caster and
    // every existing test is bit-identical (nothing spent MP off a grown pool before).
    stats.mp.max = stats.mp.base + bonus * kMpPerEndurance * veteran;
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

// CAMARADERIE: a shared victory forges a tie. When `killer` (a player or an NPC) fells a hostile,
// every standing colonist near it gains a little affinity TOWARD the killer — the design's
// "fighting a common foe" bond (see the kCamaraderie constants). Directed witness->killer so it
// feeds the existing bond-pull (cluster to a friend) and graded-rescue-reach readers. Skips the
// killer itself and Downed bodies (a crumpled colonist isn't fighting alongside). nudge_affinity
// emplaces Relationships — a component in NEITHER this scan's view nor npc_attack's view — so this
// is safe at the kill site even though npc_attack calls perform_attack mid-iteration (same reason
// the rescue bond and cruelty grudge are safe). Draws no RNG.
void bond_witnesses(entt::registry& reg, entt::entity killer, Vec2 killer_pos) {
  // CHARISMA — a charismatic champion inspires MORE devotion per shared victory. The killer's
  // Charisma scales the camaraderie each witness feels: kCamaraderieAffinity grows by
  // (1 + (CHA - 1) * kDevotionPerCharisma), capped at kDevotionCap (the ×2 ceiling the dodge/crit/
  // awareness knobs use). CHA 1 — the spawn default, and any killer with no Attributes — is ×1, so
  // every pre-Charisma camaraderie test is byte-identical. Read once here (nullable), before the
  // witness loop; no RNG, pure float→int8.
  constexpr float kDevotionPerCharisma = 0.1f;  // each Charisma level past 1 adds 10% devotion...
  constexpr float kDevotionCap = 2.0f;          // ...up to double, then it plateaus (a knob)
  Attributes* kattrs = reg.try_get<Attributes>(killer);  // also the sheet Leadership trains below
  float devotion = 1.0f;
  if (kattrs != nullptr) {
    devotion += static_cast<float>(kattrs->charisma.level - 1) * kDevotionPerCharisma;
    if (devotion > kDevotionCap) devotion = kDevotionCap;
  }
  const auto bond = static_cast<std::int8_t>(static_cast<float>(kCamaraderieAffinity) * devotion);

  // CHARISMA also widens the REACH: a charismatic champion's deeds are SEEN and admired from
  // FARTHER, so its heroism inspires onlookers a wider ring away — the "presence" half of Charisma
  // beside the "devotion" depth above (the design's Charisma compounds — more people, more deeply).
  // Reuses the same nullable `kattrs`; each level past 1 widens the camaraderie radius by
  // kReachPerCharisma, capped at kReachCap — a GENTLER ceiling than devotion's ×2, because a radius
  // grows the witnessed AREA quadratically (×1.5 reach is already ~×2.25 area). CHA 1 — the spawn
  // default, and any killer with no Attributes — is ×1, so the base radius holds and every
  // pre-Charisma camaraderie test is byte-identical. Pure float, no RNG.
  constexpr float kReachPerCharisma = 0.05f;  // each Charisma level past 1 widens the reach 5%...
  constexpr float kReachCap = 1.5f;  // ...up to ×1.5 (area ~×2.25), then plateaus (a knob)
  float reach = 1.0f;
  if (kattrs != nullptr) {
    reach += static_cast<float>(kattrs->charisma.level - 1) * kReachPerCharisma;
    if (reach > kReachCap) reach = kReachCap;
  }
  const float camaraderie_radius = kCamaraderieRadius * reach;

  int witnessed = 0;
  for (const entt::entity w : reg.view<Npc, Transform>(entt::exclude<Downed>)) {
    if (w == killer) continue;  // you don't bond with yourself for your own kill
    if (glm::distance(reg.get<Transform>(w).position, killer_pos) > camaraderie_radius) continue;
    nudge_affinity(reg, w, killer, bond);
    ++witnessed;
  }

  // LEADERSHIP — leading, felling a foe with allies WATCHING, trains the killer's Leadership skill,
  // which raises Charisma. So devotion COMPOUNDS: lead more → higher CHA → deeper bonds next time —
  // the social mirror of a striker building Strength by hitting. Trained only when at least one
  // ally saw it (a kill alone in a corner builds Striking, not a following), so `witnessed > 0`
  // gates the grant — which also keeps every LONE-kill test bit-identical (no witnesses → no XP).
  // Flows through the one grant funnel (skill + main attribute + a drip to the character level);
  // the grant runs AFTER the witness loop, so mutating the killer's Skills/Attributes can't disturb
  // the view above. A person kill always has Skills+Attributes (perform_attack requires them), but
  // guard cheaply.
  if (witnessed > 0 && kattrs != nullptr) {
    if (Skills* ks = reg.try_get<Skills>(killer); ks != nullptr) {
      const Fixed kLeadershipPerKill =
          Fixed::from_int(10);  // a witnessed kill, same grant as a hit
      grant_skill_xp(*ks, *kattrs, SkillId::Leadership, kLeadershipPerKill,
                     reg.try_get<CharacterLevel>(killer));
    }
  }
}

}  // namespace

// Stamp a fresh hit-flash on an entity that just took a blow — presentation only, so
// the renderer can blink it white (see components.hpp HitFlash). emplace_or_replace so
// a rapid second blow refreshes the flash to full rather than stacking. Called AT the
// damage sites, unconditionally on a landed hit — no roll, so it draws no RNG and the
// seeded streams stay identical. Safe mid-view: HitFlash is in no view being walked at
// any call site (the same reason Downed is safely emplaced during handle_deaths). Public
// so the systems damage sites AND the DamagePlayer command (world.cpp) share ONE
// definition of "blink on a hit" — every damage source flashes its victim, no drift.
void stamp_flash(entt::registry& reg, entt::entity e) {
  reg.emplace_or_replace<HitFlash>(e, HitFlash{kHitFlashSeconds});
}

namespace {
// Drop a now-EMPTY Equipped cache so "bare" reads as gear == nullptr everywhere — steer_npcs'
// arm-up rung and npc_equip both gate on Equipped PRESENCE, so a leftover all-zero cache would
// strand a stripped NPC from ever re-gearing. Called after a slot is cleared (a weapon OR an armour
// shatter): if BOTH slots are now zero, remove the component. Mirrors the Drop command's inline
// logic (world.cpp) — shared here by the two shatters so they can never drift.
void remove_equipped_if_empty(entt::registry& reg, entt::entity e, const Equipped& eq) {
  if (eq.strength_bonus == 0 && eq.move_penalty == 0.0f && eq.weapon_venom == 0.0f &&
      eq.crit_bonus == 0.0f && eq.weapon_durability == 0.0f && eq.defence_bonus == 0.0f &&
      eq.stamina_regen_penalty == 0.0f && eq.armour_durability == 0.0f) {
    reg.remove<Equipped>(e);
  }
}

// (Re)apply venom to `target`, shared by all THREE venom sources (a venom weapon's hit, a venom
// spit's impact, a venomous creature's bite) so they can't drift. A fresh bite always REFRESHES the
// clock to full, but it keeps the WORST potency in the blood: `dps` only raises the existing
// damage_per_second, never lowers it — so a WEAKER second bite can't dilute a stronger poison
// you're already carrying (max, not overwrite; the old per-site code overwrote, silently
// downgrading). A FIRST bite starts from a default Poisoned (dps 0), so max(0, dps) = dps and
// single-bite poison is unchanged. get_or_emplace is safe mid-iteration at every site (Poisoned is
// in none of their views).
void apply_venom(entt::registry& reg, entt::entity target, float dps) {
  Poisoned& venom = reg.get_or_emplace<Poisoned>(target);
  venom.remaining = kPoisonDuration;  // any bite re-ups the clock
  if (dps > venom.damage_per_second)
    venom.damage_per_second = dps;  // ...never downgrade the potency
}
}  // namespace

entt::entity perform_attack(entt::registry& reg, entt::entity attacker, std::mt19937& rng) {
  constexpr float kBaseReach = 45.0f;                 // a little past contact range (15)
  constexpr float kReachPerStrength = 6.0f;           // each Strength level past 1 adds reach
  constexpr float kBaseAttackDamage = 12.0f;          // a swing's raw damage before Strength
  constexpr float kDamagePerStrength = 4.0f;          // each Strength level past 1 hits harder
  const Fixed kStrikingPerHit = Fixed::from_int(10);  // XP for a strike that connects
  // A swing SPENDS stamina — the melee echo of the throw's kThrowStaminaCost, cheaper (7 vs 15)
  // since it's the faster primary attack. An exhausted fighter (stamina below the cost) can't swing
  // at all, exactly as an exhausted thrower can't throw and an exhausted mover crawls: 0 stamina =
  // disengage and recover, the "manage your wind" loop that keeps a fight from being a free
  // stand-and-win. A full 100-bar sustains ~14 swings. Knob.
  constexpr float kMeleeStaminaCost = 7.0f;
  // A POWER swing (the held power stance) hits HARDER but costs MORE stamina — the offensive twin
  // of sprint/guard, each trading the bar. 1.75x damage for the dearer 18-stamina cost (vs the base
  // 7, ~2.6x): fewer, heavier blows that fell a brute in less swings, at the price of winding you
  // faster (so it's a burst, not the pace). Knobs.
  constexpr float kPowerDamage = 1.75f;       // a powered swing's damage multiplier
  constexpr float kPowerStaminaCost = 18.0f;  // ...and its dearer stamina cost (vs the base 7)
  // ...and it SHOVES the struck foe back this far (world units) — the heavy blow's reach into
  // space. Less than a swing's base reach (45), so the foe is pushed to the edge of your range, not
  // out of it: enough to make room in a swarm without ending the fight. The ONE thing in melee that
  // repositions a target. Knob.
  constexpr float kKnockback = 30.0f;

  // A swinger needs a position (to reach from) and the progression pair to train.
  const Transform* tf = reg.try_get<Transform>(attacker);
  Attributes* attrs = reg.try_get<Attributes>(attacker);
  Skills* skills = reg.try_get<Skills>(attacker);
  if (tf == nullptr || attrs == nullptr || skills == nullptr) return entt::null;

  // Effective Strength = your trained level PLUS whatever weapon you're wielding (the
  // design's "gear grants raw +Attribute"). One number feeds BOTH reach and damage below,
  // so a heavier blade reaches further AND hits harder. Bare-handed (no Equipped) it is just
  // your level — so this is bit-identical for anyone not wielding anything.
  Equipped* gear = reg.try_get<Equipped>(attacker);  // read for reach/damage/venom; worn by a hit
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
  // A hungry or thirsty fighter hits softer — the survival Need debuff folds into this ONE raw
  // number every landing branch below shares (the hostile hit, the cruel-strike, AND the cleave all
  // read `raw`), so keeping the colony fed and watered is now a combat concern too. No Stats, or
  // full needs (the common case and every combat test), -> need_efficiency 1.0 -> bit-identical.
  Stats* atk_stats = reg.try_get<Stats>(attacker);  // read for the need debuff; healed on a kill
  const float need_eff = atk_stats != nullptr ? need_efficiency(*atk_stats) : 1.0f;
  // BERSERK: a badly-wounded fighter hits HARDER — the player/NPC-side mirror of a creature's
  // ENRAGE (resolve_creature_contacts), keyed on the SAME 0.3 HP fraction. A cornered fighter is
  // dangerous: drop below kBerserkThreshold of your max HP and your blows land kBerserkDamage x.
  // Reads the attacker's OWN Stats (atk_stats, already fetched); no Stats or above the line -> 1.0
  // -> every full-HP combat test is bit-identical. Folds into `raw` beside the need debuff (a
  // starving AND wounded fighter gets both), so it too flows to the hostile hit, cruel-strike, and
  // cleave. The comeback twin of kill vigor — a low fighter both hits harder and heals on the kill
  // — and the risk/reward mirror of enrage: leaving your OWN foe (or yourself) half-dead cuts both
  // ways.
  constexpr float kBerserkThreshold =
      0.3f;                               // below this fraction of max HP the fighter berserks...
  constexpr float kBerserkDamage = 1.5f;  // ...and its blows hit this much harder (knobs)
  const float berserk = (atk_stats != nullptr &&
                         atk_stats->health.current < atk_stats->health.max * kBerserkThreshold)
                            ? kBerserkDamage
                            : 1.0f;
  // The held POWER stance (PowerAttack marker, set from the MovePlayer command like Blocking/
  // Sprinting) makes this swing hit harder for a dearer stamina cost. No marker (every NPC, or a
  // player not holding it) -> 1.0 / the base cost -> bit-identical. Read once for BOTH the damage
  // multiplier here and the cost in too_winded just below. It multiplies raw like berserk does — a
  // fourth, choice-driven damage factor beside veteran / need_eff / berserk.
  const bool powered = reg.all_of<PowerAttack>(attacker);
  const float power = powered ? kPowerDamage : 1.0f;
  const float melee_cost = powered ? kPowerStaminaCost : kMeleeStaminaCost;
  const float raw = (kBaseAttackDamage +
                     static_cast<float>(attrs->strength.level - 1) * kDamagePerStrength * veteran +
                     static_cast<float>(gear_strength) * kDamagePerStrength) *
                    need_eff * berserk * power;

  // STAMINA — a CONNECTING swing costs it, and an empty bar can't lift the weapon. too_winded()
  // fizzles (returns true, spends nothing) when a Stats-bearing fighter is below the cost; else it
  // SPENDS the cost and returns false. It is called at each LANDING site (a hostile/mote hit, or
  // the cruel strike) — NOT on a targetless whiff, mirroring perform_throw (only a connecting throw
  // pays). Charging a whiff would be a real bug: npc_attack POLLS perform_attack every tick for
  // every NPC regardless of a target, so a per-whiff charge would drain every idle colonist ~7/tick
  // and pin the colony near-empty. A Stats-less dummy has no stamina system (returns false, swings
  // freely) — the same reason need_eff/berserk default to 1.0 without Stats. An exhausted fighter
  // fizzles the swing (no XP, no damage, no cost); 0 stamina means disengage and recover. Gates the
  // player and NPCs alike (both route through here). `atk_stats` was fetched above for the
  // need/berserk reads.
  const auto too_winded = [&]() {
    if (atk_stats == nullptr) return false;                    // no Stats -> swings freely
    if (atk_stats->stamina.current < melee_cost) return true;  // winded -> the swing dies
    atk_stats->stamina.current -= melee_cost;                  // a connecting swing spends it
    return false;
  };

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
        if (too_winded())
          return entt::null;  // too tired to land even a cruel blow (spends nothing)
        // A cruel strike lands plainly — no dodge, no crit (an unsuspecting colonist doesn't slip a
        // betrayal, and there's no "lucky" cruelty), so this branch draws NO RNG and leaves the
        // seeded stream untouched. It still trains Striking (a swing that connects) and blinks the
        // victim like any other hit; the colonist takes the same STR-vs-VIT damage and
        // handle_deaths reaps it at 0 HP (NPC = permadeath — you can thin your own colony).
        grant_skill_xp(*skills, *attrs, SkillId::Striking, kStrikingPerHit, character);
        bool lethal = false;  // did this blow drop the colonist? -> Violence on top of Cruelty
        if (Stats* st = reg.try_get<Stats>(victim); st != nullptr) {
          st->health.current -= mitigate(raw, defence_of(reg, victim));
          if (st->health.current < 0.0f) st->health.current = 0.0f;
          stamp_flash(reg, victim);
          lethal = st->health.current <= 0.0f;  // the victim was a STANDING colonist (Downed
                                                // excluded), so 0 HP here means THIS blow killed it
        }
        record_deed(reg, attacker, Deed::Cruelty, kCrueltyStrike);
        // A cruel strike that KILLS escalates to VIOLENCE — the second villain deed, the death on
        // top of the harm. Only when the blow felled the colonist, so a non-lethal cruel strike
        // stays Cruelty-only and a world with no lethal cruelty is bit-identical. Sinks standing
        // HARDER (Violence ×4 on top of Cruelty ×6). Same mid-iteration safety as the Cruelty deed
        // above (record_deed touches no iterated view).
        //
        // But only UNJUST violence counts — the design's "violence counts only vs standing >= 0
        // victims: killing bandits barely dents you". Felling an INNOCENT (a colonist at
        // neutral-or- better standing) is the villain deed; felling one who has ALREADY gone bad —
        // a below-zero standing earned through their OWN cruelty — is rough justice and adds no
        // Violence. The Cruelty of the blow still lands either way (the strike WAS cruel); only the
        // escalation is spared. `standing()` is a pure query over the victim's ledger; NO ledger ->
        // standing 0 -> counts, which is every colonist in a normal colony, so the common case is
        // bit-identical. ponytail: a victim only reaches below-zero standing once NPC-villain AI
        // (or a villainous co-op player) exists, so this is dormant wiring today — but it keeps the
        // Violence math faithful to the ledger the moment a wronged victim can be a wrongdoer
        // themselves.
        const BehaviorLedger* victim_led = reg.try_get<BehaviorLedger>(victim);
        const bool victim_was_innocent = victim_led == nullptr || standing(*victim_led) >= 0;
        if (lethal && victim_was_innocent)
          record_deed(reg, attacker, Deed::Violence, kViolenceKill);
        // The betrayal is also PERSONAL: the struck colonist forms a GRUDGE toward the striker (a
        // negative affinity edge, the mirror of a rescue's bond). A resented player won't be
        // rescued by this colonist later — a targeted consequence that lands before global
        // villain-fear does.
        nudge_affinity(reg, victim, attacker, kCrueltyGrudge);
        // ...and the cruelty is WITNESSED: nearby colonists who saw it form a SMALLER grudge toward
        // the striker too, so a reputation for cruelty spreads (the negative mirror of
        // camaraderie's bond_witnesses). Reuses the `colonists` view (Npc+Transform,
        // exclude<Downed>); skips the victim (it already formed its own larger grudge above). The
        // striker is always the PLAYER here (branch-gated) and so never appears in this Npc view —
        // no self-grudge to guard. nudge_affinity emplaces Relationships, in neither `colonists`
        // nor npc_attack's view, so it's safe mid-iteration. No RNG.
        for (const entt::entity witness : colonists) {
          if (witness == victim) continue;  // the victim's larger grudge is recorded above
          if (glm::distance(origin, colonists.get<Transform>(witness).position) >
              kCamaraderieRadius)
            continue;
          nudge_affinity(reg, witness, attacker, kWitnessGrudge);
        }
      }
    }
    return entt::null;  // a whiff (or the cruel strike above) — nothing to hand back to destroy
  }

  // A target IS in reach — this swing connects, so it spends stamina (or fizzles if the fighter is
  // winded, before any XP/damage). A targetless whiff above never reached here, so it stayed free.
  if (too_winded()) return entt::null;

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
  // A KEEN blade lifts the crit CHANCE: its crit_bonus adds to the Luck-driven crit, so a keen
  // wielder crits more often — the second named weapon trait's proc. A plain/unarmed attacker has
  // gear_crit 0, so `crit` is exactly crit_chance(luck) and every existing fight is bit-identical
  // (nothing draws that didn't before). Capped at kCritChanceCeiling so even a keen blade in a
  // lucky hand can't crit every swing — the doubled blow stays a spike, not the norm.
  constexpr float kCritChanceCeiling = 0.75f;  // Luck + keen can lift crit, but never past 3/4
  const float gear_crit = gear != nullptr ? gear->crit_bonus : 0.0f;
  float crit = crit_chance(attrs->luck.level) + gear_crit;
  if (crit > kCritChanceCeiling) crit = kCritChanceCeiling;
  const float applied =
      (crit > 0.0f && unit(rng) < crit) ? base_damage * kCritMultiplier : base_damage;

  // BACKSTAB: a strike on a creature whose back is TURNED lands harder. A creature that's moving
  // AWAY from you is chasing someone ELSE (chase_prey aims its velocity at its own prey), so it
  // never saw the blow coming — peeling a beast off a cornered ally is rewarded. The positional
  // twin of the low-HP execute: execute reads the target's HEALTH, this reads its FACING. Pure
  // geometry — the dot of the target's heading against the attacker->target direction: >
  // kBackstabCosine means the beast is fleeing roughly straight away (within ~60deg), i.e. you're
  // behind it. No RNG. A STATIONARY target (speed 0) or one closing on YOU (facing you down, dot <=
  // the cutoff) gets no bonus, so every still-foe combat test is bit-identical. Only Enemies reach
  // here (motes returned above); a target with no Velocity simply can't be flanked.
  // (The facing geometry lives in the shared backstab_multiplier — one "don't turn your back" rule,
  // the same one a creature uses on a fleeing victim in resolve_creature_contacts.)
  // Capture where the blow LANDS now — BEFORE a powered swing's knockback (below) can shove the
  // target. The CLEAVE further down centres its second-foe search on this struck spot, not on where
  // the foe was flung to; otherwise a powered hit would spare a bystander that was in the swing's
  // arc simply because the primary got knocked out of the cleave radius. For an ordinary swing
  // nothing shoves the target, so struck_pos is just its current position and the cleave is
  // unchanged (bit-identical). Reused for the backstab read below, which also wants the
  // pre-knockback spot.
  const Vec2 struck_pos = reg.get<Transform>(target).position;
  const float backstab = backstab_multiplier(origin, struck_pos, reg.try_get<Velocity>(target));

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
    float dealt =
        st->health.current < st->health.max * kExecuteThreshold ? applied * kExecuteBonus : applied;
    dealt *= backstab;  // a flank stacks on the finisher — a distracted, half-dead beast folds fast
    st->health.current -= dealt;
    if (st->health.current < 0.0f) st->health.current = 0.0f;
    stamp_flash(reg, target);  // the struck target blinks white
    // KNOCKBACK: a POWER swing also SHOVES the foe back — the heavy blow's reach into space, the
    // one thing in melee that repositions a target (make room in a swarm, or shunt a brute off a
    // cornered ally). It reads the SAME `powered` flag that made the swing heavier, so ONLY a
    // powered hit shoves; an ordinary swing (every existing fight) moves nothing -> bit-identical.
    // Pure geometry, no RNG: push the target kKnockback along the attacker->target direction.
    // Writing the target's position (a value change, not an add/remove) can't invalidate the
    // caller's view (npc_attack iterates the ATTACKERS; the target is a different entity). A foe
    // exactly ON the attacker (zero offset — only a degenerate test) has no direction to push, so
    // it stays put.
    if (powered) {
      Vec2& target_pos = reg.get<Transform>(target).position;
      const Vec2 away = target_pos - origin;
      if (const float dist = glm::length(away); dist > 0.0f)
        target_pos += (away / dist) * kKnockback;
    }
    // The killing blow on a HOSTILE is a Valor deed for the attacker — the SECOND deed through the
    // morality write-point (after a rescue's Charity), proving it generalises across dimensions.
    // Credited only on the alive->dead TRANSITION, so a second swing landing on the same foe this
    // tick (before handle_deaths reaps it) can't double-claim the kill. Motes (Hazard) returned
    // earlier and peaceful NPCs are never targeted here, so only slaying a monster counts — and
    // NPCs earn it too (npc_attack shares this perform_attack): a colonist who fells a creature is
    // brave.
    if (was_alive && st->health.current <= 0.0f) {
      record_deed(reg, attacker, Deed::Valor, kValorKill);
      // ...and the shared victory forges CAMARADERIE: nearby colonists bond to the killer (the
      // third relationship-forming event, after the rescue bond and the cruelty grudge). The
      // skirmish happens within melee reach, so the attacker's own position (tf, non-null past the
      // guard at the top) is its centre.
      bond_witnesses(reg, attacker, tf->position);
      // ...and the kill grants the killer a burst of VIGOR — a little health back (kKillVigor),
      // capped at max. Rewards finishing a fight; a full-health killer is unchanged
      // (bit-identical). Reuses `atk_stats` (the attacker's sheet); an attacker with no Stats
      // simply doesn't heal. NPCs get it too via the shared perform_attack.
      if (atk_stats != nullptr) {
        atk_stats->health.current += kKillVigor;
        if (atk_stats->health.current > atk_stats->health.max)
          atk_stats->health.current = atk_stats->health.max;
      }
    }
    // A VENOM weapon's hit also ENVENOMS the foe — the player-side mirror of a swarmer's bite,
    // reusing Poisoned + tick_poison via the shared apply_venom (refresh the clock, keep the worst
    // potency). Only a venomous blade (gear->weapon_venom > 0) does this, so a bare-handed or
    // plain-weapon swing is unchanged. `gear` is the wielder's Equipped fetched at the top.
    if (gear != nullptr && gear->weapon_venom > 0.0f) apply_venom(reg, target, gear->weapon_venom);
  }

  // CLEAVE: a wide swing catches a SECOND foe. The nearest OTHER Enemy within kCleaveRadius of the
  // one you struck takes kCleaveFraction of the (raw) blow, softened by its OWN VIT — so melee gets
  // an anti-swarm answer (the throw's close-range twin): one swing, two foes when they cluster,
  // with no new input. Reached only past the mote/dodge/whiff returns above, so `target` is a
  // just-struck Enemy. NO crit/execute/venom/Valor on the spillover — it's the blade catching a
  // bystander, not an aimed blow (the "spillover isn't a strike" call the riposte's no-Valor
  // already set). Chips through handle_deaths like any damage, flashes so it reads. No RNG.
  // View-safe: reads the `enemies` view (Enemy+Transform) and writes only Stats/HitFlash — in
  // neither that view nor npc_attack's — so it is fine even when npc_attack calls perform_attack
  // mid-iteration. ponytail: radius/fraction knobs.
  constexpr float kCleaveRadius =
      40.0f;  // a swing's width — a foe this near the struck one is caught
  constexpr float kCleaveFraction = 0.5f;  // ...for this share of the blow
  // Centre on `struck_pos` (captured at the strike, above), NOT the target's CURRENT position: a
  // powered swing has already SHOVED the target away this call, and a bystander in the swing's arc
  // must be caught by where the blow LANDED, not spared because the primary was flung out of range.
  entt::entity cleaved = entt::null;
  float nearest_other = kCleaveRadius;
  for (const entt::entity other : enemies) {
    if (other == target) continue;  // don't re-hit the one you struck
    const float d = glm::distance(struck_pos, enemies.get<Transform>(other).position);
    if (d < nearest_other) {
      nearest_other = d;
      cleaved = other;
    }
  }
  if (cleaved != entt::null) {
    if (Stats* cs = reg.try_get<Stats>(cleaved); cs != nullptr) {
      cs->health.current -= mitigate(raw * kCleaveFraction, defence_of(reg, cleaved));
      if (cs->health.current < 0.0f) cs->health.current = 0.0f;
      stamp_flash(reg, cleaved);  // the cleaved foe blinks too, so the spillover reads
    }
  }

  // WEAR: this connecting swing on a hostile dulls the blade by one. At 0 it SHATTERS — the weapon
  // slot clears (strength, heft, and venom all gone), so the wielder is unarmed until it grabs
  // another (the design's "durability now, repair later" — the item's tradeoff bane made TEMPORAL).
  // Reached only past the mote/dodge/whiff returns, so this is a real hit on a hostile — the same
  // scope venom/execute/cleave use, and the effect-less cruel strike (which returned earlier) never
  // wears. A bare hand (no Equipped) or a wear-free fixture (weapon_durability 0) is bit-identical,
  // and a fresh blade is unchanged until it actually breaks. Runs AFTER the venom/cleave above, so
  // the breaking hit still lands its full blow and proc; only the NEXT swing is unarmed.
  if (gear != nullptr && gear->weapon_durability > 0.0f) {
    gear->weapon_durability -= 1.0f;
    if (gear->weapon_durability <= 0.0f) {  // shattered — clear the weapon slot, keep any armour
      gear->strength_bonus = 0;
      gear->move_penalty = 0.0f;
      gear->weapon_venom = 0.0f;
      gear->crit_bonus =
          0.0f;  // a shattered keen blade loses its crit edge with the rest of its slot
      gear->weapon_durability = 0.0f;
      // ...and if nothing else is worn, REMOVE the now-empty cache so "bare" reads as gear ==
      // nullptr everywhere (else a shattered NPC, gated on Equipped presence, never re-arms).
      // `gear` dangles after the remove, but we return immediately below; removing a component NOT
      // in npc_attack's iterated view is view-safe, like the deed/bond emplaces above.
      remove_equipped_if_empty(reg, attacker, *gear);
    }
  }
  return entt::null;
}

void perform_throw(entt::registry& reg, entt::entity attacker) {
  // The player's RANGED option — hurl something at the nearest hostile far out of melee reach,
  // chipping an approaching swarm before it closes. What sets it apart from a melee swing: it draws
  // NO RNG (no dodge, no crit, no execute) — a plain, reliable, MODEST hit, so ranged trades
  // melee's burst potential for range and certainty. Like a swing it COSTS STAMINA
  // (kThrowStaminaCost, dearer than melee's kMeleeStaminaCost — a bigger wind-up): a standing
  // character out-regenerates a slow plink, but throwing fast, or on the move (kiting drains far
  // more than regen), empties the bar and drops you to the same exhausted crawl a sprint causes
  // (update_stamina's gate), and an empty bar can't throw at all. So the cost keeps range honest:
  // you can soften an approach but can't burst a swarm down or kite one forever for free. The hit
  // isn't dealt HERE — this LAUNCHES a homing Projectile that advance_projectiles flies to the
  // target and lands on arrival (so a throw has a visible travel time, and is wasted if the target
  // dies first). Player-only for now: there is no npc_throw, so NPCs still only melee (a
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
  // scaling and no crit — a throw is deliberately the simple, reliable option. Scaled by the same
  // need_efficiency the melee swing uses, so a starving thrower is weakened too (no ranged loophole
  // around the survival debuff); full needs -> 1.0 -> bit-identical. `stats` is the attacker's
  // sheet fetched above (non-null here).
  const float raw =
      (kBaseThrowDamage + static_cast<float>(attrs->dexterity.level - 1) * kDamagePerDexterity) *
      need_efficiency(*stats);
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
  // DEXTERITY speeds the bolt — the design's DEX "Speed" aspect: a defter thrower's shot FLIES
  // faster, so it reaches the target sooner AND is wasted less often when the target dies
  // mid-flight (advance_projectiles despawns a shot whose target died first). A reliability payoff
  // for a DEX throw build, distinct from the +damage above. DEX 1 (spawn default, every existing
  // throw test) -> 1.0 -> the flat kProjectileSpeed -> bit-identical. Capped like the
  // dodge/crit/awareness clamps. `attrs` is the thrower's sheet, non-null here (derefed for the
  // damage above). No RNG.
  constexpr float kThrowSpeedPerDexterity = 0.06f;  // each DEX level past 1 speeds the bolt 6%...
  constexpr float kThrowSpeedCap = 2.0f;            // ...up to 2x, then it plateaus
  float speed_scale =
      1.0f + static_cast<float>(attrs->dexterity.level - 1) * kThrowSpeedPerDexterity;
  if (speed_scale > kThrowSpeedCap) speed_scale = kThrowSpeedCap;
  reg.emplace<Projectile>(shot,
                          Projectile{target, attacker, applied, kProjectileSpeed * speed_scale});
}

void magic_bolt(entt::registry& reg, entt::entity caster) {
  // The player's first SPELL — the magic mirror of perform_throw: launch a homing bolt at the
  // nearest hostile in range, chipping it from afar. Two things set magic apart, both from the
  // design: it is LEARNED (only an entity that carries the Spellcasting skill can cast — a trickle
  // of mana is innate, but inert until taught), and it spends MANA, not stamina, so it draws on a
  // bar the physical fighter never touches. It reuses the very Projectile the throw flies (tinted
  // arcane), and INTELLECT scales its damage (the design's magic attribute), trained BY casting
  // (Spellcasting -> INT) so a mage sharpens by casting the way a thrower sharpens DEX by throwing.
  // Draws NO RNG — a plain, reliable bolt, like the throw. Actor-agnostic: the player casts it via
  // the Cast command, a colonist mage via npc_cast (just below) — the player==NPC parity.
  constexpr float kSpellRange = 350.0f;        // the same long reach as a throw
  constexpr float kSpellManaCost = 25.0f;      // each cast spends this (a full 100 bar -> 4 bolts)
  constexpr float kBaseSpellDamage = 14.0f;    // raw before Intellect (a touch above a throw's 8)
  constexpr float kDamagePerIntellect = 4.0f;  // each Intellect level past 1 sharpens the bolt
  const Fixed kSpellcastingPerCast = Fixed::from_int(10);

  const Transform* tf = reg.try_get<Transform>(caster);
  Attributes* attrs = reg.try_get<Attributes>(caster);
  Skills* skills = reg.try_get<Skills>(caster);
  Stats* stats = reg.try_get<Stats>(caster);
  if (tf == nullptr || attrs == nullptr || skills == nullptr || stats == nullptr) return;

  // A BODY AT 0 HP IS INERT — it can't cast. The player never reaches here at 0 HP (a 0-HP player
  // is Downed, and the Cast command guards on Downed), but an NPC never goes Downed — handle_deaths
  // permakills it, LATER in the same tick than npc_cast runs — so a mage chipped to 0 by an earlier
  // resolve_creature_contacts / tick_poison would otherwise still fling a bolt from a dead body.
  // Guarding at this shared choke point (not in npc_cast) is the root fix: it also covers the
  // player and any future caller, and every real cast is at positive HP so it's bit-identical.
  if (stats->health.current <= 0.0f) return;

  // THE LEARNED GATE: no Spellcasting skill -> you never learned to cast -> nothing happens. This
  // is what makes magic learned-not-innate, and it keeps a world of non-casters bit-identical — a
  // plain colonist with a full mana bar still can't fling a bolt.
  if (skills->find(SkillId::Spellcasting) == nullptr) return;

  // Nearest hostile in range — deterministic strict-<, Enemies only (a bolt is an anti-creature
  // tool; it never targets a colonist, so villainy stays a deliberate MELEE choice).
  const Vec2 origin = tf->position;
  entt::entity target = entt::null;
  float nearest = kSpellRange;
  auto enemies = reg.view<Enemy, Transform>();
  for (const entt::entity e : enemies) {
    const float d = glm::distance(origin, enemies.get<Transform>(e).position);
    if (d < nearest) {
      nearest = d;
      target = e;
    }
  }
  if (target == entt::null) return;  // nothing in range — a held cast: no mana spent, no XP

  // Needs mana in hand — an empty bar fizzles (nothing spent, no XP), the magic echo of the empty-
  // stamina throw. Strict <, so a cast at exactly the cost still fires.
  if (stats->mp.current < kSpellManaCost) return;
  stats->mp.current -= kSpellManaCost;

  // A connecting cast trains Spellcasting -> Intellect, so a mage grows the INT that sharpens the
  // bolt by casting it — the learn-by-doing loop, mirror of a throw training Throwing -> DEX.
  CharacterLevel* character = reg.try_get<CharacterLevel>(caster);
  grant_skill_xp(*skills, *attrs, SkillId::Spellcasting, kSpellcastingPerCast, character);

  // INT-vs-VIT damage: base + earned-Intellect delta, softened by the target's VIT and scaled by
  // the same need_efficiency every attack uses (a starving mage casts weaker too — no ranged
  // loophole). No crit/execute — a bolt is the plain reliable option, like the throw.
  const float raw =
      (kBaseSpellDamage + static_cast<float>(attrs->intellect.level - 1) * kDamagePerIntellect) *
      need_efficiency(*stats);
  const float applied = mitigate(raw, defence_of(reg, target));

  // Launch a homing Projectile carrying the (mitigated) damage — the SAME primitive the throw
  // flies, so advance_projectiles delivers it and credits the Valor on arrival with no new
  // plumbing. Tinted arcane violet so a spell reads apart from a throw's yellow bolt.
  const entt::entity shot = reg.create();
  reg.emplace<Transform>(shot, origin);
  reg.emplace<PrevTransform>(shot, origin);
  reg.emplace<RenderDot>(shot, Vec3{0.6f, 0.3f, 0.95f}, 4.0f);  // arcane violet bolt
  reg.emplace<Projectile>(shot, Projectile{target, caster, applied, kProjectileSpeed});
}

void npc_cast(entt::registry& reg) {
  // NPCs that have LEARNED to cast fling a bolt at a nearby hostile — the caster mirror of
  // npc_attack (melee) and the player's Cast command, reusing the very same magic_bolt so a
  // colonist mage casts EXACTLY as the player does (the player==NPC parity the design demands).
  // Only an Npc carrying Spellcasting casts (learned, not innate — from a spawn cantrip or a read
  // Spellbook), and only on a FULL mana bar: that throttles it to a considered bolt then a
  // recharge, so it never spams a bolt every tick or dumps its whole bar in one burst — a rhythm
  // WITHOUT a cooldown component (mana only falls by casting and refills steadily, so "full" recurs
  // about every kSpellManaCost/regen seconds). magic_bolt itself gates on a target in range AND the
  // mana it spends, so a manaless or targetless mage is a silent no-op. Collect-then-cast:
  // magic_bolt spawns a Projectile (emplacing Transform), so gather the casters first and cast
  // AFTER the walk, never mutating the pool mid-view.
  std::vector<entt::entity> casters;
  auto mages = reg.view<Npc, Transform, Attributes, Skills, Stats>();
  for (const entt::entity n : mages) {
    if (mages.get<Skills>(n).find(SkillId::Spellcasting) == nullptr) continue;  // never learned
    const Vital& mp = mages.get<Stats>(n).mp;
    if (mp.current < mp.max)
      continue;  // cast only on a full bar -> a bolt, then a recharge (no spam)
    casters.push_back(n);
  }
  for (const entt::entity n : casters) magic_bolt(reg, n);
}

void npc_harvest(entt::registry& reg) {
  // PROVIDERS work the land: an Npc carrying a Provider Aspiration reaps the nearest RIPE food plot
  // in reach into a MEAL — the peaceful mirror of npc_attack/npc_cast, and the "NPC farm behaviour"
  // the shared, actor-agnostic harvest_nearest_crop was built to serve (the SAME call the player's
  // Harvest command uses, so a colonist farms exactly as the player does — player==NPC parity). The
  // steer ladder's Provider rung walks it to the plot; this does the gathering once it's in reach.
  // harvest_nearest_crop no-ops unless a plot is ripe (stock >= kHarvestCost) AND within its
  // radius, so it's SELF-THROTTLED by the plot's slow regrow — a provider reaps a patch, then must
  // wait for it to grow back, meals trickling out to feed the colony rather than a firehose. Only a
  // Provider- aspiration Npc farms; no Aspiration (every colonist/test today) -> no-op ->
  // bit-identical. Collect-then-harvest: harvest_nearest_crop calls spawn_meal (emplacing a new
  // entity's Transform), so gather the providers FIRST and harvest AFTER, never reallocating the
  // pool mid-view.
  std::vector<entt::entity> providers;
  auto folk = reg.view<Npc, Transform, Aspiration>();
  for (const entt::entity n : folk) {
    if (folk.get<Aspiration>(n).kind == AspirationKind::Provider) providers.push_back(n);
  }
  for (const entt::entity n : providers) harvest_nearest_crop(reg, n);
}

void heal_spell(entt::registry& reg, entt::entity caster) {
  // The SUPPORT twin of magic_bolt — a learned caster MENDS the nearest wounded ally instead of
  // bolting a foe, spending the same mana bar. Where the bolt reaches out to a hostile, the mend
  // reaches to a FRIEND: the first co-op-native spell (an embodied colony heals its own). It reuses
  // magic_bolt's shape (the learned Spellcasting gate + a mana cost + a stat-scaled effect trained
  // by doing) but lands INSTANTLY on the ally (no Projectile — a mending word, not a flying bolt)
  // and grows a NEW skill: Healing -> WISDOM (the design's support domain), so a healer sharpens by
  // healing the way a mage sharpens by casting. Draws NO RNG.
  constexpr float kHealRange = 300.0f;  // a touch shorter than the bolt's 350 — you tend the near
  constexpr float kHealManaCost = 25.0f;  // the same cost as a bolt (a full 100 bar -> 4 mends)
  constexpr float kBaseHeal = 18.0f;      // HP restored before Wisdom (a touch above a bolt's 14)
  constexpr float kHealPerWisdom = 5.0f;  // each Wisdom level past 1 mends more
  const Fixed kHealingPerCast = Fixed::from_int(10);

  const Transform* tf = reg.try_get<Transform>(caster);
  Attributes* attrs = reg.try_get<Attributes>(caster);
  Skills* skills = reg.try_get<Skills>(caster);
  Stats* stats = reg.try_get<Stats>(caster);
  if (tf == nullptr || attrs == nullptr || skills == nullptr || stats == nullptr) return;

  // A BODY AT 0 HP IS INERT — it can't mend either (the support twin of the bolt's guard). An NPC
  // healer chipped to 0 HP by an earlier resolve_creature_contacts / tick_poison would otherwise
  // still raise a living ally's HP from beyond the grave in the window before handle_deaths reaps
  // it — the cleanest form of the parity break, a persistent wrong output on a body a player
  // provably can't drive (a 0-HP player is Downed; the CastHeal command guards on Downed).
  // Bit-identical: every real mend is at positive HP.
  if (stats->health.current <= 0.0f) return;

  // THE LEARNED GATE: shares Spellcasting with the bolt — reading the arcane tome teaches BOTH the
  // offensive bolt and this mending word, so a caster can do either. (A dedicated Healing tome /
  // learn-path is a follow-up; the point here is the SPELL, and gating it on the same learned skill
  // keeps a world of non-casters bit-identical — a plain colonist can't mend.)
  if (skills->find(SkillId::Spellcasting) == nullptr) return;

  // Nearest WOUNDED ally in range — a PERSON (Stats), not a creature, not a downed body, not the
  // caster itself, and actually hurt (health below max), so a full-health ally is never targeted
  // and no mana is wasted topping up the hale. Deterministic strict-<.
  const Vec2 origin = tf->position;
  entt::entity patient = entt::null;
  float nearest = kHealRange;
  auto allies = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
  for (const entt::entity a : allies) {
    if (a == caster) continue;  // mend an ALLY, not yourself (self-heal is regenerate_vitals' job)
    const Vital& h = allies.get<Stats>(a).health;
    if (h.current >= h.max) continue;  // hale — nothing to mend
    const float d = glm::distance(origin, allies.get<Transform>(a).position);
    if (d < nearest) {
      nearest = d;
      patient = a;
    }
  }
  if (patient == entt::null) return;  // no one to mend — a held cast: no mana spent, no XP

  // Needs mana in hand — an empty bar fizzles (nothing spent, no XP), like the bolt.
  if (stats->mp.current < kHealManaCost) return;
  stats->mp.current -= kHealManaCost;

  // A connecting mend trains Healing -> Wisdom, so a healer grows the WIS that sharpens the mend by
  // casting it — the learn-by-doing loop, mirror of a bolt training Spellcasting -> INT.
  CharacterLevel* character = reg.try_get<CharacterLevel>(caster);
  grant_skill_xp(*skills, *attrs, SkillId::Healing, kHealingPerCast, character);

  // The mend: base + earned-Wisdom delta, scaled by the caster's need_efficiency (a starving healer
  // mends weaker too — no support loophole, matching the bolt). Added to the ally, CLAMPED at max
  // (no over-heal). The patient's Stats isn't the caster's, and this is a VALUE write, so it's
  // view-safe.
  Vital& h = reg.get<Stats>(patient).health;
  const float mend = (kBaseHeal + static_cast<float>(attrs->wisdom.level - 1) * kHealPerWisdom) *
                     need_efficiency(*stats);
  h.current += mend;
  if (h.current > h.max) h.current = h.max;
}

void npc_heal(entt::registry& reg) {
  // NPCs that have LEARNED to cast MEND a nearby wounded ally — the support mirror of npc_cast,
  // reusing the very same heal_spell so a colonist healer mends EXACTLY as the player does (the
  // player==NPC parity). Only an Npc carrying Spellcasting (the shared learned gate) mends, and
  // only on a FULL mana bar — the same throttle npc_cast uses, so a healer mends then recharges
  // rather than spamming one every tick. heal_spell itself gates on a WOUNDED ally in range AND the
  // mana it spends, so a healer with no one hurt (or no mana) is a silent no-op. Because npc_heal
  // runs AFTER npc_cast and both need a FULL bar, a battle-mage naturally BOLTS a threat first
  // (spending its mana) and only MENDS in a lull when no bolt fired — offence-then-support falls
  // out of the order, no priority flag needed. heal_spell mutates only component VALUES (never the
  // entity set), so it needs no collect-then-cast, but we mirror npc_cast's shape for symmetry.
  std::vector<entt::entity> healers;
  auto mages = reg.view<Npc, Transform, Attributes, Skills, Stats>();
  for (const entt::entity n : mages) {
    if (mages.get<Skills>(n).find(SkillId::Spellcasting) == nullptr) continue;  // never learned
    const Vital& mp = mages.get<Stats>(n).mp;
    if (mp.current < mp.max) continue;  // mend only on a full bar -> a mend, then a recharge
    healers.push_back(n);
  }
  for (const entt::entity n : healers) heal_spell(reg, n);
}

void npc_shield(entt::registry& reg) {
  // NPCs that have LEARNED to cast raise a BARRIER on themselves when a creature is CLOSING — the
  // defensive third of the caster's kit (npc_cast bolts a threat, npc_heal mends an ally,
  // npc_shield wards), reusing the very same shield_spell so a colonist mage wards EXACTLY as the
  // player does (the player==NPC parity). Gated like the others plus two of its own: Spellcasting
  // (learned) + a FULL mana bar (the no-spam throttle) + a creature within kShieldThreatRange (a
  // real threat to ward against — a mage at peace doesn't waste mana) + NOT already Shielded (so it
  // wards ONCE when first threatened, then fights UNDER the barrier and re-wards only after it
  // lapses, never re-casting every recharge while the ward still holds). Runs BEFORE npc_cast in
  // the schedule, so a threatened mage WARDS first and then bolts under the barrier on later full
  // bars. shield_spell only emplaces the caster's own Shielded (no spawn), so no collect-then-act
  // is strictly needed, but we mirror npc_cast's shape. Draws no RNG.
  // ponytail: the "wards AND still bolts" balance relies on kShieldDuration (5s) OUTLASTING the
  // mana refill (~2.5s at cost 25 / regen 10), so npc_cast gets a full bar while the ward holds. If
  // either is retuned so the ward is SHORTER than the refill, this (running first) would re-grab
  // every full bar and starve offence — gate npc_shield behind "not casting a bolt this tick" if
  // that happens. A creature this close is worth warding against (knob). Wider than npc_guard's
  // contact-tight kGuardRange (30): a guard is a free per-tick toggle that reacts at the moment of
  // impact, but a ward is a mana-costed 5s pre-cast, so it wants LEAD TIME to be up before the blow
  // lands.
  constexpr float kShieldThreatRange = 120.0f;
  std::vector<entt::entity> warders;
  auto mages = reg.view<Npc, Transform, Attributes, Skills, Stats>();
  auto creatures = reg.view<Enemy, Transform>();
  for (const entt::entity n : mages) {
    if (mages.get<Skills>(n).find(SkillId::Spellcasting) == nullptr) continue;  // never learned
    const Vital& mp = mages.get<Stats>(n).mp;
    if (mp.current < mp.max) continue;  // ward only on a full bar -> a ward, then a recharge
    if (reg.all_of<Shielded>(n))
      continue;  // already warded — don't re-cast while the barrier holds
    const Vec2 pos = mages.get<Transform>(n).position;
    bool threatened = false;
    for (const entt::entity c : creatures) {
      if (glm::distance(pos, creatures.get<Transform>(c).position) < kShieldThreatRange) {
        threatened = true;
        break;
      }
    }
    if (threatened) warders.push_back(n);
  }
  for (const entt::entity n : warders) shield_spell(reg, n);
}

void shield_spell(entt::registry& reg, entt::entity caster) {
  // The DEFENSIVE spell of the trio (bolt = offence, mend = support, shield = defence) — a learned
  // caster raises a BARRIER on ITSELF, spending the same mana bar, that soaks part of each creature
  // blow for a few seconds. It's the design's "ward"-role spell, named Shield here so it doesn't
  // collide with warded ARMOUR's thorns. The game's FIRST timed BUFF: where the bolt reaches out to
  // a foe and the mend reaches to a friend, the shield turns inward. It shares the bolt's shape
  // (the learned Spellcasting gate + a mana cost + an INTELLECT-scaled effect trained by casting ->
  // Spellcasting -> INT) but instead of a Projectile or an ally-heal it (re)emplaces the caster's
  // own Shielded, which tick_shield ages and resolve_creature_contacts reads. Draws NO RNG. No
  // Transform needed (a self-ward has no position to aim), unlike the bolt/mend. Actor-agnostic
  // like the other two: the player casts it via CastShield, a colonist mage via npc_shield (just
  // above, a threat-triggered self-ward) — the player==NPC parity, closed for all three spells.
  constexpr float kShieldDuration = 5.0f;      // seconds the barrier holds (a knob)
  constexpr float kShieldManaCost = 25.0f;     // the same cost as a bolt/mend (a full 100 bar -> 4)
  constexpr float kBaseAbsorb = 6.0f;          // damage soaked per blow before Intellect
  constexpr float kAbsorbPerIntellect = 2.0f;  // each Intellect level past 1 thickens the barrier
  const Fixed kSpellcastingPerCast = Fixed::from_int(10);

  Attributes* attrs = reg.try_get<Attributes>(caster);
  Skills* skills = reg.try_get<Skills>(caster);
  Stats* stats = reg.try_get<Stats>(caster);
  if (attrs == nullptr || skills == nullptr || stats == nullptr) return;

  // A body at 0 HP is inert — it can't cast (the same caster-guard the bolt/mend carry; a dying NPC
  // never goes Downed, so guard on health, not the player-only Downed).
  if (stats->health.current <= 0.0f) return;

  // THE LEARNED GATE: shares Spellcasting with the bolt/mend — no skill, no ward (keeps a
  // non-caster world bit-identical, a plain colonist can't shield).
  if (skills->find(SkillId::Spellcasting) == nullptr) return;

  // Needs mana in hand — an empty bar fizzles (nothing spent, no XP), like the bolt/mend.
  if (stats->mp.current < kShieldManaCost) return;
  stats->mp.current -= kShieldManaCost;

  // Casting the ward trains Spellcasting -> Intellect (INT is both the bolt's damage AND this
  // barrier's thickness), so a mage hardens its shield by using it, the learn-by-doing loop the
  // whole magic tree runs on. Unlike the bolt/mend (which fizzle with no target/patient and grant
  // no XP), a shield ALWAYS connects — it wards YOU — so this trains every cast. A safe-corner
  // grind of INT is therefore possible, but MANA-throttled and curve-slowed to the same
  // self-training shape as sprinting growing Athletics or moving growing Conditioning
  // (resource-gated, not risk-gated); gate it on a nearby threat if it ever matters — a tuning
  // knob, not a redesign.
  CharacterLevel* character = reg.try_get<CharacterLevel>(caster);
  grant_skill_xp(*skills, *attrs, SkillId::Spellcasting, kSpellcastingPerCast, character);

  // Raise (or refresh) the barrier: base + earned-Intellect delta. get_or_emplace so a RECAST
  // re-ups the clock and re-reads INT (monotonic, never weaker) rather than stacking a second
  // component.
  const float absorb =
      kBaseAbsorb + static_cast<float>(attrs->intellect.level - 1) * kAbsorbPerIntellect;
  Shielded& shield = reg.get_or_emplace<Shielded>(caster);
  shield.remaining = kShieldDuration;
  shield.absorb = absorb;
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
        // A cast SHIELD soaks a RANGED hit too, exactly as it soaks a melee blow
        // (resolve_creature_contacts): `absorb` comes off the carried damage, floored at 0. The
        // ward is a GENERAL damage buffer, not melee-only -- npc_shield raises it when a creature
        // closes, and a spitter IS that creature, so its venom bolt must be soaked or the mana was
        // spent for nothing against the one ranged threat. Unshielded (no Shielded -> every
        // existing throw and spit) takes the full p.damage, bit-identical. Read HERE, before the
        // chip, so the alive->dead transition below (and its Valor/vigor credit) weighs the SOAKED
        // damage. Like the melee ward it stops DAMAGE, not CONTACT: the venom below still lands (a
        // poison-ward is a separate spell -- see resolve_creature_contacts).
        float dealt = p.damage;
        if (const Shielded* shield = reg.try_get<Shielded>(p.target); shield != nullptr) {
          dealt -= shield->absorb;
          if (dealt < 0.0f) dealt = 0.0f;
        }
        st->health.current -= dealt;
        if (st->health.current < 0.0f) st->health.current = 0.0f;
        stamp_flash(reg, p.target);
        // A VENOM spit ENVENOMS its target — the ranged echo of a swarmer's bite, reusing Poisoned
        // + tick_poison via the shared apply_venom (refresh the clock, keep the worst potency).
        // Only a venom-carrying shot does this; the player's plain throw carries poison 0, so it
        // stays unchanged. Harmless on a just-felled target, like the venom weapon.
        if (p.poison_per_second > 0.0f) apply_venom(reg, p.target, p.poison_per_second);
        // Valor is for felling a HOSTILE, so credit it only when the shot killed an Enemy (a
        // player's throw). A creature's spit homes on a PERSON, so this guard keeps a spitter from
        // ever earning Valor for killing a colonist (creatures have no morality anyway).
        if (was_alive && st->health.current <= 0.0f && reg.all_of<Enemy>(p.target) &&
            reg.valid(p.owner)) {
          record_deed(reg, p.owner, Deed::Valor, kValorKill);
          // ...and, like a melee kill, the shot forges CAMARADERIE — the RANGED half of
          // bond_witnesses (the follow-up the melee bond noted). Centred on the OWNER (the one who
          // fought), not the distant impact, so it bonds those who stood with the shooter. Guard
          // the owner's Transform (it's valid here, but may be a positionless entity in theory).
          if (const Transform* owner_tf = reg.try_get<Transform>(p.owner)) {
            bond_witnesses(reg, p.owner, owner_tf->position);
          }
          // KILL VIGOR on a ranged kill too — the same kKillVigor health-back as a melee kill
          // (parity), healing the OWNER (the one who fought), capped at max. A full-health owner is
          // unchanged (bit-identical). p.owner's Stats isn't in the shots view, so this is
          // view-safe.
          if (Stats* owner_stats = reg.try_get<Stats>(p.owner); owner_stats != nullptr) {
            owner_stats->health.current += kKillVigor;
            if (owner_stats->health.current > owner_stats->health.max)
              owner_stats->health.current = owner_stats->health.max;
          }
        }
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
    float poison;  // the spit's venom (0 for a non-venomous spitter)
  };
  std::vector<Shot> shots;
  auto spitters = reg.view<Enemy, Transform>();
  for (const entt::entity e : spitters) {
    Enemy& enemy = spitters.get<Enemy>(e);
    if (enemy.spit_range <= 0.0f) continue;  // melee-only creatures never spit
    // A spitter chipped to 0 HP earlier this tick (tick_poison venom, or a riposte/thorns) is a
    // corpse pending handle_deaths — it can't fling a fresh spit from the grave. The ranged echo of
    // the leech's drink guard and the caster spells' 0-HP guard: 0 HP is inert. Every real spitter
    // carries Stats (make_creature), so try_get is defensive; a positive-HP spitter is unchanged
    // (bit-identical). Unlike a melee dying blow (already in motion, so it lands), a spit is a NEW
    // projectile launched here — a corpse launches nothing.
    if (const Stats* st = reg.try_get<Stats>(e); st != nullptr && st->health.current <= 0.0f)
      continue;
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
    // The spit carries the spitter's own venom (poison_per_second) as its payload, alongside the
    // VIT-mitigated impact — a VENOM spitter poisons on hit, a plain one (poison 0) just chips.
    shots.push_back({pos, victim, e, mitigate(enemy.spit_damage, defence_of(reg, victim)),
                     enemy.poison_per_second});
  }

  // Launch the queued spits. A violet bolt, distinct from the player's bright-yellow throw, so
  // incoming fire reads at a glance.
  for (const Shot& s : shots) {
    const entt::entity bolt = reg.create();
    reg.emplace<Transform>(bolt, s.from);
    reg.emplace<PrevTransform>(bolt, s.from);
    reg.emplace<RenderDot>(bolt, Vec3{0.8f, 0.3f, 0.85f}, 4.0f);
    reg.emplace<Projectile>(bolt,
                            Projectile{s.target, s.owner, s.damage, kProjectileSpeed, s.poison});
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
    // QUALITY scales the BOON only: a finer blade folds in more +Strength, a crude one less; the
    // BANE (heft) below stays full. quality 1.0 (every blade spawned today) -> the exact old value,
    // so this is bit-identical. ponytail: the int truncation (e.g. +4 at q1.25 -> int(5.0) = 5, but
    // +4 at q1.1 -> int(4.4) = 4) means fractional quality on a small bonus can round away — a
    // named ceiling; swap to std::lround if fine-grained tiers ever need it.
    eq.strength_bonus = static_cast<int>(static_cast<float>(wpn.strength_bonus) * wpn.quality);
    eq.move_penalty = wpn.move_penalty;
    eq.weapon_venom = wpn.venom_per_second;  // a venom blade folds its proc in with its other stats
    eq.crit_bonus = wpn.crit_bonus;          // a keen blade folds its crit chance in the same way
    eq.weapon_durability = wpn.durability;  // a fresh blade starts with its full life; hits wear it
  } else {
    const Armour& arm = armours.get<Armour>(nearest);
    eq.defence_bonus =
        arm.defence_bonus * arm.quality;  // finer plate softens more; bane stays full
    eq.stamina_regen_penalty = arm.stamina_regen_penalty;
    eq.armour_durability = arm.durability;  // fresh plate starts with its full life; blows wear it
    eq.armour_thorns = arm.thorns_per_hit;  // a warded plate folds its spikes in (raw, like venom)
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

// Spawn a MEAL on the ground at `pos` — a harvested-crop food, the payoff of the food economy. It
// is a Pickup like a loot orb, but PURE food: it fills more hunger (kMealFood, > an orb's 50 and >
// the ~60 you'd graze from the stock it costs) and, being a prepared meal rather than monster loot,
// grants NONE of the orb's combat rewards (no heal, no permanent max-HP). A warm orange dot so it
// reads as food, not treasure. The base sits BELOW the hunger cap (100) on purpose: that headroom
// is what lets a skilled COOK's `food_scale` surplus actually LAND on a famished eater (a full
// belly can't hold more of either) rather than being clamped away.
void spawn_meal(entt::registry& reg, Vec2 pos, float food_scale = 1.0f) {
  constexpr float kMealFood =
      70.0f;  // a solid meal — beats an orb (50) and grazing the stock. Knob.
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.9f, 0.6f, 0.2f}, 4.0f);  // small warm-orange meal
  Pickup& pk = reg.emplace<Pickup>(e);
  pk.heal = 0.0f;                    // a meal is food, not combat loot — no restorative heal...
  pk.bonus_max_hp = 0.0f;            // ...and no permanent HP bump (that's the monster-drop reward)
  pk.food = kMealFood * food_scale;  // a better COOK stretches the same crop further (food_scale)
}

}  // namespace

// Gather the nearest RIPE food plot within reach into a meal at the harvester's feet — the seam of
// the food economy. A patch you'd otherwise graze bite-by-bite is instead PREPARED: it spends a
// chunk of the plot's stock (kHarvestCost) to drop a single MEAL worth more hunger than that stock
// grazed raw (spawn_meal's kMealFood), so working the land beats grubbing at it. Returns whether
// anything was harvested. Public and actor-agnostic so it is the ONE definition both the player's
// Harvest command and (later) an NPC farm behaviour call — the player==NPC parity the design wants.
// A plot below the cost isn't ripe enough to bother (no half-meals), so a bare or barely-grown
// patch yields nothing. ponytail: nearest ripe plot only, no "best yield" search — one plot exists
// and they don't cluster; revisit if crops ever pack in.
bool harvest_nearest_crop(entt::registry& reg, entt::entity harvester) {
  constexpr float kHarvestCost =
      60.0f;  // plot stock a harvest spends — a patch must be fairly ripe
  const Transform* ht = reg.try_get<Transform>(harvester);
  if (ht == nullptr) return false;
  const Vec2 pos = ht->position;  // capture BEFORE spawn_meal, which reallocates the Transform pool

  entt::entity best = entt::null;
  float best_dist = 0.0f;
  auto plots = reg.view<FoodSource, Transform>();
  for (const entt::entity src : plots) {
    if (plots.get<FoodSource>(src).stock < kHarvestCost) continue;  // not ripe enough to gather
    const float d = glm::distance(pos, plots.get<Transform>(src).position);
    if (d > plots.get<FoodSource>(src).radius) continue;  // out of reach (same range you'd graze)
    if (best == entt::null || d < best_dist) {
      best = src;
      best_dist = d;
    }
  }
  if (best == entt::null) return false;               // nothing ripe within reach
  plots.get<FoodSource>(best).stock -= kHarvestCost;  // spend the plot's growth...

  // COOKING scales the meal: a better cook prepares the same crop into MORE food. An unlearned cook
  // (or none) is level 1 -> food_scale ×1.0 -> the exact base meal, so a fresh world is
  // bit-identical. Read the LEVEL (a value copy) BEFORE spawn_meal — which creates an entity and
  // reallocates pools — then RE-FETCH the component pointers AFTER for the training, so no
  // component handle crosses the realloc (the discipline the `pos` capture above already follows).
  constexpr float kCookingYieldPerLevel =
      0.10f;  // each Cooking level past 1 stretches the meal 10%
  float food_scale = 1.0f;
  if (const Skills* sk = reg.try_get<Skills>(harvester); sk != nullptr)
    if (const Skill* cook = sk->find(SkillId::Cooking))
      food_scale = 1.0f + static_cast<float>(cook->level - 1) * kCookingYieldPerLevel;
  spawn_meal(reg, pos, food_scale);  // ...for a prepared meal where the harvester stands

  // Preparing a meal TRAINS Cooking -> Intellect (learns it at level 1 on the first harvest) — the
  // learn-by-doing loop, so a colonist that farms a lot becomes the colony's cook. Gated on the
  // progression pair; a harvester without it just doesn't grow, so it's bit-identical for anyone
  // not progressing. Pointers RE-FETCHED here, after spawn_meal's realloc.
  const Fixed kCookingPerHarvest =
      Fixed::from_int(10);  // XP for preparing a meal (matches a swing)
  if (Skills* sk = reg.try_get<Skills>(harvester); sk != nullptr)
    if (Attributes* attrs = reg.try_get<Attributes>(harvester); attrs != nullptr)
      grant_skill_xp(*sk, *attrs, SkillId::Cooking, kCookingPerHarvest,
                     reg.try_get<CharacterLevel>(harvester));
  return true;
}

// Sow a crop seedling where the planter stands — the front of the food chain. A FoodSource like the
// wild garden, but it starts EMPTY (stock 0): it feeds no one until the same regrow that recovers a
// grazed patch (graze) grows it up to ripe (~half a minute at the default 2/stock/s), at which
// point harvest_nearest_crop turns it into a meal. So planting reuses the growth and harvest
// machinery already there — the whole new mechanic is "spawn one that starts bare". A dull
// young-green dot so a seedling reads as not-yet-food next to the bright wild garden. ponytail:
// free to plant, no seed cost or per-player cap — a sandbox seam; add scarcity (a seed item, a plot
// limit) when the economy needs it to matter.
entt::entity plant_crop(entt::registry& reg, entt::entity planter) {
  constexpr float kCropRadius = 45.0f;  // a tilled patch's reach — a knob
  const Transform* pt = reg.try_get<Transform>(planter);
  if (pt == nullptr) return entt::null;  // nowhere to sow — no phantom crop at the origin
  const Vec2 pos = pt->position;         // copy BEFORE create reallocates the Transform pool
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.35f, 0.45f, 0.15f}, kCropRadius);  // a young, dull-green sprout
  FoodSource fs{};
  fs.stock = 0.0f;  // a SEEDLING — nothing to eat yet; the regrow must grow it before it ripens
  fs.radius = kCropRadius;
  reg.emplace<FoodSource>(e, fs);
  return e;
}

// Spawn a Weapon on the ground at `pos` — the ONE canonical dropped-weapon entity (a
// steel-grey dot + Weapon), so a slain BRUTE's drop (handle_deaths) and a player's Drop
// command produce an identical, re-wieldable pickup. Public so both callers share this one
// definition of what a grounded weapon looks like (no drift). It spawns the DEFAULT Weapon;
// that is lossless today because the only Equipped mods come from this same default — when
// more than one Weapon def exists, Drop must pass the wielder's cached mods instead.
// ponytail: no lifetime — a dropped weapon persists (brutes are the rarer kill, so they don't
// pile up); add a fade like Pickup's if the field ever litters.
void spawn_weapon(entt::registry& reg, Vec2 pos, float quality) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.75f, 0.78f, 0.85f}, 6.0f);  // steel grey, a touch bigger
  reg.emplace<Weapon>(e).quality = quality;  // finer than baseline when a tough kill drops it
}

// Spawn a piece of Armour on the ground at `pos` — the canonical grounded-armour entity, the
// defensive counterpart of spawn_weapon. A distinct render colour (dull bronze) so you can
// tell armour from a weapon on the field at a glance. Step near and press E to don it.
void spawn_armour(entt::registry& reg, Vec2 pos, float quality) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.72f, 0.52f, 0.24f}, 6.0f);  // dull bronze, distinct from steel
  reg.emplace<Armour>(e).quality = quality;  // finer than baseline when a tough kill drops it
}

// Spawn a VENOM blade on the ground — the second weapon TYPE. Lighter than the default steel blade
// (less +Strength, less heft) but its hits ENVENOM the foe (perform_attack applies Poisoned,
// reusing tick_poison) — trading raw power for a lingering chip and mobility (the design's "gear
// grants a +aspect, with a bane"). A sickly venom-green dot to tell it from steel. One definition
// shared by the opening scene and a slain spitter's drop, so a looted fang is identical to the
// seeded one. Like spawn_weapon it spawns the DEFAULT venom Weapon — lossless while its mods are
// the only source of Equipped's weapon_venom.
void spawn_venom_weapon(entt::registry& reg, Vec2 pos) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.4f, 0.8f, 0.35f}, 6.0f);  // venom green
  Weapon& w = reg.emplace<Weapon>(e);
  w.strength_bonus = 2;       // lighter/weaker than the steel blade's +4 — the trade for the proc
  w.move_penalty = 0.15f;     // and nimbler than the steel blade's 0.25 heft
  w.venom_per_second = 6.0f;  // its hits poison (health/sec); a knob
}

// A steel blade that rolled VENOMOUS (the first named equipment TRAIT). Mirrors spawn_weapon's
// steel — full +4-base Strength scaled by quality, the SAME 0.25 heft bane — but knocks the base
// Strength down one notch (kVenomousStrength) and turns on venom (kVenomousVenomPerSecond). So it's
// a HEAVY poison blade: less nimble and less venomous than the spitter's fang, but hitting harder —
// a NAMED intra-item trade (poison build vs raw power), the qualitative variety the flat boon/bane
// pairs lacked, and never pure-upside (the +venom is paid for in -Strength, atop steel's unchanged
// heft). Reuses the whole shipped venom path: the equip fold copies venom_per_second into
// Equipped.weapon_venom, perform_attack applies Poisoned, tick_poison chips it — NOTHING new in
// equip or combat. A distinct venom-tinted-steel dot so it reads apart from plain steel and the
// green fang.
void spawn_venomous_steel(entt::registry& reg, Vec2 pos, float quality) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.55f, 0.72f, 0.45f},
                         6.0f);  // venom-tinted steel (between the two)
  Weapon& w = reg.emplace<Weapon>(e);
  w.strength_bonus = kVenomousStrength;  // steel's +4 down one notch — the paired -STR trade
  w.venom_per_second =
      kVenomousVenomPerSecond;  // ...bought with a modest envenoming proc (the boon)
  // move_penalty keeps its default 0.25 (steel's full heft — the bane is untouched, quality lifts
  // only the upside), and quality scales the reduced +Strength boon exactly like any fine steel.
  w.quality = quality;
}

// A steel blade that rolled KEEN (the SECOND named weapon trait). The crit sibling of venomous
// steel: same full 0.25 heft, same one-notch Strength trade (kKeenStrength), but the boon is a CRIT
// chance bonus (kKeenCritBonus) instead of venom. perform_attack folds crit_bonus into the Luck
// crit, so a keen blade lands the doubled blow more often — a distinct proc (crit vs poison)
// feeding a different (Luck) build, never pure-upside (the +crit is paid for in -Strength atop
// steel's heft). A distinct pale-gold "honed edge" dot so it reads apart from plain steel, the
// venom fang and venomous steel.
void spawn_keen_steel(entt::registry& reg, Vec2 pos, float quality) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.85f, 0.82f, 0.55f},
                         6.0f);  // pale-gold honed edge (its own tint)
  Weapon& w = reg.emplace<Weapon>(e);
  w.strength_bonus = kKeenStrength;  // steel's +4 down one notch — the paired -STR trade
  w.crit_bonus = kKeenCritBonus;     // ...bought with a sharper edge: more crit (the boon)
  // move_penalty keeps its default 0.25 (full heft, bane untouched), and quality scales the reduced
  // +Strength boon exactly like any fine steel.
  w.quality = quality;
}

// A plate that rolled WARDED (spiked) — armour's FIRST flavourful trait, the defensive twin of
// venomous/keen steel. It trades a notch of raw defence (kWardedDefence, below plain plate's 6) for
// THORNS: every creature blow it absorbs reflects kWardedThorns back onto the attacker
// (resolve_creature_contacts), so a warded tank punishes a swarm for hitting it — a distinct payoff
// (chip-back vs a blade's proc) feeding a stand-and-tank build, never pure-upside (the spikes are
// paid for in -defence, atop plate's unchanged stamina-regen bane). Reuses the whole shipped armour
// path: the equip fold copies thorns_per_hit into Equipped.armour_thorns, resolve_creature_contacts
// reflects it — NOTHING new in equip. A distinct cold spiked-iron tint so it reads apart from plain
// bronze plate.
void spawn_warded_armour(entt::registry& reg, Vec2 pos, float quality) {
  const entt::entity e = reg.create();
  reg.emplace<Transform>(e, pos);
  reg.emplace<PrevTransform>(e, pos);
  reg.emplace<RenderDot>(e, Vec3{0.5f, 0.55f, 0.6f}, 6.0f);  // cold spiked-iron grey (its own tint)
  Armour& a = reg.emplace<Armour>(e);
  a.defence_bonus = kWardedDefence;  // plain plate's 6 down two — the paired -defence trade
  a.thorns_per_hit = kWardedThorns;  // ...bought with spikes: chip back per blow soaked (the boon)
  // stamina_regen_penalty keeps its default (full bane), and quality scales the reduced defence
  // boon exactly like any fine plate.
  a.quality = quality;
}

void decay_standing(entt::registry& reg) {
  // Leaky moral DECAY, the slow counter-current to record_deed: every kDecayPeriod ticks each
  // nonzero deed dimension creeps ONE step toward 0, so a reputation FADES if it isn't renewed —
  // redemption and corruption for free (a villain who stops being cruel climbs back toward neutral;
  // a hero who rests on old glory dims). Symmetric: both signs creep to 0. An EXACT per-ledger
  // integer tick count (no float -> bit-exact), so a short-run world is unchanged until a whole
  // period elapses. ponytail/BALANCE: kDecayPeriod is a fast knob for now — the design's "~44-day
  // leak" is much slower; tune at playtest. Only actors with a ledger (they've done a deed) are
  // iterated, so the never-acting stay untouched, and it touches no other component
  // (own-BehaviorLedger writes only).
  constexpr std::int32_t kDecayPeriod = 3600;  // ~60 game-seconds per step toward 0 (60Hz * 60s)
  auto view = reg.view<BehaviorLedger>();
  for (const entt::entity e : view) {
    BehaviorLedger& led = view.get<BehaviorLedger>(e);
    if (++led.decay_ticks < kDecayPeriod) continue;  // still accruing — no step this tick
    led.decay_ticks = 0;
    for (std::int32_t& d : led.dims) {
      if (d > 0)
        --d;
      else if (d < 0)
        ++d;  // toward 0 from either side — heroes fade, villains redeem, at the same rate
    }
  }
}

void decay_bonds(entt::registry& reg) {
  // The relationships twin of decay_standing: every kBondDecayPeriod ticks each UNLATCHED edge's
  // affinity creeps one step toward 0, so a tie cools if it isn't renewed. A Partner/Nemesis latch
  // (bond_latched) resists, so the strongest ties — a devoted partner or a sworn nemesis — PERSIST
  // where casual ties fade: the design's "bonds latch, resist decay". Exact integer counter per
  // Relationships (no float -> bit-exact), so a short run is unchanged until a whole period
  // elapses. ponytail/BALANCE: kBondDecayPeriod is a fast knob for now (bonds could well be
  // stickier than reputation — tune at playtest). Touches only its own edges, no other component.
  constexpr std::int32_t kBondDecayPeriod = 3600;  // ~60 game-seconds per step toward 0 (a knob)
  auto view = reg.view<Relationships>();
  for (const entt::entity e : view) {
    Relationships& rel = view.get<Relationships>(e);
    if (++rel.decay_ticks < kBondDecayPeriod) continue;  // still accruing — no step this tick
    rel.decay_ticks = 0;
    for (Relation& edge : rel.edges) {
      if (bond_latched(edge.affinity)) continue;  // a deep bond or grudge holds fast
      if (edge.affinity > 0)
        --edge.affinity;
      else if (edge.affinity < 0)
        ++edge.affinity;  // toward 0 from either side, at the same rate
    }
  }
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
  // fallen softens you (Charity -> compassion), and its MIRROR, striking the helpless, hardens you
  // right back (Cruelty -> compassion DOWN). And because bravery is both the TINTED axis and the
  // one steer_npcs reads twice, a fighter visibly warms and holds its ground — a character arc from
  // deeds alone. `try_get`, NEVER get_or_emplace: an entity with no Personality (every creature,
  // and any bare test entity) drifts nothing and STAYS Personality-free, so the absent-Personality
  // world is preserved. The PLAYER now carries a neutral Personality (build_scene), so its own
  // deeds reshape it here too — and because no sim system reads the player's Personality, that
  // stays a render-only character arc, not a mechanics change. FIVE deeds drift now; only Honesty
  // (which has no event yet) waits. Pure integer math (no RNG), clamped in int before the int8 cast
  // so a long career can't overflow. Most deeds LIFT their axis; a VILLAIN deed lowers it (a
  // negative step), so the ledger's hero/villain split shows up in the personality too.
  if (Personality* p = reg.try_get<Personality>(actor)) {
    std::int8_t* axis = nullptr;
    int step = kDeedDriftStep;  // the default: a deed strengthens its leaning
    if (kind == Deed::Valor) {
      axis = &p->bravery;
    } else if (kind == Deed::Charity) {
      axis = &p->compassion;
    } else if (kind == Deed::Loyalty) {
      axis =
          &p->loyalty;  // standing by your own hardens the loyalty leaning — "you are what you do"
    } else if (kind == Deed::Cruelty) {
      axis = &p->compassion;   // the villain MIRROR of Charity: striking the helpless HARDENS you,
      step = -kDeedDriftStep;  // drifting compassion DOWN toward callous ("the war changed him")
    } else if (kind == Deed::Violence) {
      axis =
          &p->bravery;  // a KILLER grows desensitized — nerve UP (the default lifting step).
                        // Bravery is NERVE, not goodness; the villainy is tracked by standing, so
                        // a murderer reads as a BOLD one: shrunk small by infamy (renown_scale)
                        // yet warmed by nerve (the tint). A lethal cruel strike thus reshapes TWO
                        // axes at once — Cruelty cools compassion, Violence steels the nerve — so
                        // a KILL hardens you more, and differently, than a mere wound.
    }
    if (axis != nullptr) drift_axis(*axis, step);  // shared clamp — grief uses it too
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

int allies_of(const entt::registry& reg, entt::entity e) {
  // Count the entities that regard `e` as a FRIEND — an incoming bond at/above kBondPull — the
  // mirror of affinity_toward (which reads ONE directed tie). Scans every entity that carries a
  // Relationships (only the bonded do, so a fresh world scans nothing) for an edge pointing at `e`.
  // Skips `e` itself (no self-ally). A pure read used by the HUD; the sim never calls it. ponytail:
  // O(bonded entities * their edges), fine for a colony — a reverse index only if a huge roster
  // ever needs it.
  int count = 0;
  for (auto [other, rel] : reg.view<const Relationships>().each()) {
    if (other == e) continue;  // an entity isn't its own ally
    for (const Relation& edge : rel.edges) {
      if (edge.other == e && edge.affinity >= kBondPull) {
        ++count;  // one ally, however many edges it holds toward e (it holds at most one)
        break;
      }
    }
  }
  return count;
}

void handle_deaths(entt::registry& reg, Vec2 respawn_point, float dt, std::mt19937& rng) {
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
    s.fatigue.current = s.fatigue.max;  // and rested — else an EXHAUSTION collapse drops you again
    s.warmth.current = s.warmth.max;  // and warm — else you revive still freezing (a re-death loop)
    reg.remove<Poisoned>(e);          // no lingering lethal status through a revive
    v.value = Vec2{0.0f, 0.0f};
  };

  // Adding/removing Downed doesn't invalidate this view (Downed isn't one of its
  // components), so mutating it while iterating is safe.
  auto players = reg.view<Stats, PlayerControlled, Transform, Velocity>();
  for (const entt::entity e : players) {
    Stats& s = players.get<Stats>(e);
    Downed* down = reg.try_get<Downed>(e);

    if (down == nullptr) {
      // TWO ways to fall: HP to 0 (a mortal blow / starvation / venom) OR fatigue to 0 (EXHAUSTION
      // — the third need's consequence, the design's "empty -> Downed"). Either drops you, and the
      // SAME Downed window follows: an exhausted player crumples even at full health, rescued or
      // respawned exactly like a wounded one (revive restores fatigue too, so you don't drop
      // straight back down). Standing AND rested -> nothing to do.
      if (s.health.current > 0.0f && s.fatigue.current > 0.0f) continue;
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
    // GLOBAL villain veto: the colony abandons a famous VILLAIN. If the fallen's own deeds have
    // marked them (standing at or below -kKnownAt, the SAME "Suspect" line villain-fear flees) then
    // NOBODY hauls them up — the global counterpart of the personal grudge-veto below: a grudge is
    // one colonist you wronged refusing you, this is the WHOLE colony turning its back on infamy,
    // even those with no personal grudge. Read the DOWNED player's OWN standing once (it doesn't
    // depend on the rescuer). No ledger, or standing above the villain line (every non-villain, and
    // the common case) -> not shunned -> rescued exactly as before, so a non-villain world is
    // bit-identical. The mirror of the hero being reached from FARTHER (the graded rescue reach): a
    // hero is worth crossing the field for, a villain not worth crossing the room.
    const BehaviorLedger* fallen_led = reg.try_get<BehaviorLedger>(e);
    const bool shunned_villain = fallen_led != nullptr && standing(*fallen_led) <= -kKnownAt;
    auto allies = reg.view<Stats, Transform>(entt::exclude<Enemy, Downed>);
    for (const entt::entity a : allies) {
      if (shunned_villain) break;  // a marked villain is abandoned — no ally will haul it up
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
      // ...and if the rescuer was ALREADY bonded to the one they saved (affinity at/above the
      // shared kBondPull bond floor), the save is also LOYALTY — standing by your own, not mere
      // charity to a stranger. Read affinity BEFORE the +kRescueAffinity nudge below, so this
      // counts the tie that existed when the rescuer CHOSE to help; a first save of a stranger
      // (affinity 0) is charity only. Same mid-iteration safety as the Charity deed (record_deed
      // touches no iterated view, affinity_toward is a const read). This lands the dormant Loyalty
      // dimension of standing.
      if (affinity_toward(reg, rescuer, e) >= kBondPull)
        record_deed(reg, rescuer, Deed::Loyalty, kRescueLoyalty);
      // ...and a personal BOND forms — MUTUALLY: hauling someone off the ground ties you both. The
      // rescuer->rescued edge is what drives visible MOTION (the rescuer is usually an NPC that
      // runs steer_npcs, so it can later drift back toward the ally it saved via the bond-pull
      // rung); the rescued->rescuer edge is the other half of the same felt tie — a player forms NO
      // OTHER outgoing bond (every other event puts the edge on someone else: camaraderie bonds
      // witnesses TO a killer, a grudge points a victim AT their attacker), so being saved is the
      // one thing that finally fills the player's own "closest bond" readout with the ally who
      // saved their life. Both use kRescueAffinity — a rescue is felt equally on both sides. The
      // rescued `e` is always a player (this view is PlayerControlled-gated) and so is never
      // steered, which is why its edge is inert in the sim (HUD/data only) and adds no motion — the
      // original one-way note deferred it for exactly that reason; this lands it now for the
      // readout. Emplacing a Relationships on `e` is the same view-safety as the Downed/ledger
      // emplaces above (it's not one of this view's components).
      nudge_affinity(reg, rescuer, e, kRescueAffinity);
      nudge_affinity(reg, e, rescuer, kRescueAffinity);
      // ...and if the rescue was SEEN, the rescuer earns wider ADMIRATION: bond_witnesses bonds
      // nearby colonists to the hero — the SAME "a public heroic act bonds those who watched"
      // machinery a killing blow uses (camaraderie), now completing the witnessed-event set: a
      // cruel strike spreads grudges, and a kill AND a rescue both spread bonds (the one heroism
      // that used to go unwitnessed no longer does). Reusing bond_witnesses wholesale means a
      // charismatic rescuer inspires MORE devotion (Charisma scales it) and a witnessed rescue
      // trains the rescuer's Leadership -> Charisma too — leadership is public heroism of ANY kind,
      // saving a life as much as felling a foe. Centred on the rescue spot (`pos`); bond_witnesses
      // excludes the rescuer itself (w == killer) and the just-revived player isn't in its Npc
      // view, so only true bystanders bond. No onlookers nearby -> a no-op, so a minimal rescue
      // (rescuer + downed only, as every existing rescue test is) stays bit-identical.
      bond_witnesses(reg, rescuer, pos);
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
  std::vector<Vec2> weapon_drops;  // ...where brutes fell — a steel weapon instead...
  std::vector<Vec2> armour_drops;  // ...where sentinels fell — a piece of armour...
  std::vector<Vec2> venom_drops;  // ...and where spitters fell — a venom fang (poison-build blade)
  std::vector<Vec2> kit_weapon_drops;  // where an ARMED colonist fell — its wielded blade lands...
  std::vector<Vec2> kit_armour_drops;  // ...and where an ARMOURED one fell — its worn plate
  auto npcs = reg.view<Stats, Npc>();
  for (const entt::entity e : npcs) {
    if (npcs.get<Stats>(e).health.current > 0.0f) continue;
    dead.push_back(e);
    // A fallen colonist DROPS THE KIT it earned — the gear it was wielding/wearing lands where it
    // fell, so an ally's blade and plate are RECOVERABLE (the equipment economy's death end: gear
    // outlives its bearer, the twin of a slain brute paying out a weapon). Only a real slot drops
    // (an unarmed, bare NPC — every existing death test — leaves nothing, bit-identical), and it
    // drops PLAIN: baseline quality, no trait, NO rng draw, matching the Drop command's "a dropped
    // weapon is a plain one" simplification, so the drop_rng_ stream stays byte-aligned. The
    // non-empty-slot checks mirror the Drop command's exactly. Position captured HERE (the corpse
    // is still valid; the destroy loop is below).
    if (const Equipped* eq = reg.try_get<Equipped>(e); eq != nullptr) {
      const Vec2 pos = reg.get<Transform>(e).position;
      if (eq->strength_bonus != 0 || eq->move_penalty != 0.0f || eq->weapon_venom != 0.0f ||
          eq->crit_bonus != 0.0f)
        kit_weapon_drops.push_back(pos);  // it was armed -> its weapon drops
      if (eq->defence_bonus != 0.0f || eq->stamina_regen_penalty != 0.0f)
        kit_armour_drops.push_back(pos);  // it was armoured -> its plate drops
    }
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
        case DropKind::VenomWeapon:
          venom_drops.push_back(pos);
          break;
      }
    }
  }
  // GRIEF and VINDICATION: permadeath lands on the LIVING too. Before the dead are swept away (they
  // are still valid here — destroy is the next line), a survivor's nerve drifts when someone they
  // had strong FEELINGS about falls — and which WAY it drifts depends on the bond:
  //   * a lost FRIEND (affinity >= kBondFriendAt) -> GRIEF: bravery DOWN a step (kGriefDrift), the
  //     negative mirror of a Valor deed's bravery-UP, PLUS an acute panic rout — watching one of
  //     your own fall shakes you.
  //   * a slain NEMESIS (affinity <= kBondNemesisAt, a sworn latched grudge) -> VINDICATION:
  //   bravery
  //     UP a step (kVindicationDrift) and NO panic — the tormentor that cowed you is gone, so you
  //     stand a little taller. The quiet mirror of grief.
  // A rattled mourner flees sooner, a vindicated one holds its ground (steer_npcs reads bravery),
  // so a death leaves a visible mark on those left behind — the "NPCs are people, and people
  // change" pillar reaching past the one who died, in BOTH directions. Only strong ties stir: a
  // mere acquaintance or a passing rival (between the two floors) is neither mourned nor
  // celebrated. The corpse must be a PERSON (Npc + health <= 0), never a slain creature — no one
  // mourns OR gloats over a monster (and creatures never accrue affinity anyway, but the Npc guard
  // makes it explicit). Runs only on a death tick; pure int math, no RNG. No
  // Personality/Relationships, or no strong bond to a fresh corpse, keeps the pre-bond world
  // bit-identical (a Nemesis-tier grudge can't exist at spawn — it takes sustained cruelty to sink
  // an edge to kBondNemesisAt). ponytail: O(survivors x their few edges) but only when someone
  // dies; a reverse bond-index is the upgrade if death ticks ever get crowded.
  auto mourners = reg.view<Personality, Relationships>();
  for (const entt::entity m : mourners) {
    if (const Stats* ms = reg.try_get<Stats>(m); ms != nullptr && ms->health.current <= 0.0f)
      continue;  // a corpse (or a mourner falling this same tick) neither grieves nor gloats
    for (const Relation& edge : mourners.get<Relationships>(m).edges) {
      if (!reg.valid(edge.other)) continue;  // a stale handle to a reused id
      const bool friend_bond = edge.affinity >= kBondFriendAt;
      const bool nemesis_bond = edge.affinity <= kBondNemesisAt;
      if (!friend_bond && !nemesis_bond) continue;  // a lukewarm tie stirs nothing when it falls
      const Stats* es = reg.try_get<Stats>(edge.other);
      if (es == nullptr || es->health.current > 0.0f || !reg.all_of<Npc>(edge.other))
        continue;  // not a fallen PERSON
      if (friend_bond) {
        drift_axis(mourners.get<Personality>(m).bravery, kGriefDrift);  // a lost friend a blow...
        // ...and an acute ROUT right now: the survivor panics for kPanicDuration seconds
        // (steer_npcs flees harder + flees creatures, tick_panic counts it down).
        // emplace_or_replace so losing a SECOND friend the same tick just refreshes the timer.
        // Panicked is not in the `mourners` view (Personality+Relationships), so emplacing it
        // mid-iteration is safe.
        reg.emplace_or_replace<Panicked>(m, Panicked{kPanicDuration});
      } else {  // nemesis_bond -> the sworn enemy is gone
        drift_axis(mourners.get<Personality>(m).bravery,
                   kVindicationDrift);  // ...a slain foe a lift
      }
    }
  }
  // FINER loot from a TOUGHER kill: a brute's dropped steel and a sentinel's plate come out ABOVE
  // baseline quality (a bigger boon), so felling a hard foe pays out better gear than you start
  // with — the first thing item quality expresses in play. The venom fang stays baseline (its small
  // +Strength would round the scale away, and it already trades raw power for venom).
  //
  // Now the fine quality is ROLLED, not flat: each fine drop draws its own quality from the
  // [kFineQualityMin, kFineQualityMax) band, so two brute kills yield subtly different steel and
  // looting stays interesting past the first drop. Drawn from the DEDICATED `rng` (World's
  // drop_rng_), so these rolls never touch the creature/combat stream — every wave, dodge and spawn
  // is bit-identical; only the dropped quality varies. The draw happens per fine drop in a fixed
  // order (weapons then armour, each vector already in deterministic entity order), so the sequence
  // replays identically. Orb + venom drops stay baseline 1.0 (no draw). ponytail: one band for both
  // fine kinds; a per-archetype band is the refinement if a sentinel should out-roll a brute.
  //
  // A fine STEEL drop also rolls a named TRAIT. That decision is a single RAW drop_rng_ draw taken
  // BEFORE the quality draw (see the inline note below for why order matters) — a portable mt19937
  // uint compare, NOT a uniform_real, so which drops come out venomous/keen is identical on every
  // stdlib, unlike the quality band. ~15% become a heavy poison blade (spawn_venomous_steel:
  // +venom, -1 Strength) and another ~15% a razor-keen blade (spawn_keen_steel: +crit, -1
  // Strength); the rest are plain. Both reuse shipped mechanics (venom -> Poisoned; the Luck crit),
  // so equip/combat need almost nothing new. Only STEEL rolls a trait — the fang is already
  // venomous, orb/armour are other kinds.
  std::uniform_real_distribution<float> fine_quality{kFineQualityMin, kFineQualityMax};
  for (const entt::entity e : dead) reg.destroy(e);
  for (const Vec2& pos : orb_drops) spawn_pickup(reg, pos);  // swarmer sustain
  for (const Vec2& pos : weapon_drops) {
    // Roll the TRAIT first, as a raw mt19937 uint compare: raw engine output is bit-PORTABLE across
    // stdlibs, so which drops come out venomous/keen is identical on every platform (the
    // uniform_real quality draw that follows is NOT portable — it's only ever band-tested). Drawing
    // it first also lets a fresh-seeded test pin the variant deterministically. The two traits are
    // MUTUALLY EXCLUSIVE — a drop is plain OR venomous OR keen, never both — so no two traits stack
    // on one item and no traits[] list is needed: one draw splits the range into a venomous band, a
    // keen band just past it, then plain (the ~70% remainder).
    // `auto`, NOT uint32_t: std::mt19937::result_type is uint_fast32_t, which is 64-bit on some
    // stdlibs (libstdc++) — assigning it to a uint32_t narrows and trips -Wshorten-64-to-32
    // -Werror. The VALUE is always in [0, 2^32), so the threshold compares below are identical on
    // every platform regardless of the storage width.
    const auto roll = rng();
    const float q = fine_quality(rng);  // then the rolled quality (band-tested, not exact)
    if (roll < kVenomousDropThreshold)
      spawn_venomous_steel(reg, pos, q);  // heavy poison blade (-STR, +venom)
    else if (roll < kVenomousDropThreshold + kKeenDropThreshold)
      spawn_keen_steel(reg, pos, q);  // razor blade (-STR, +crit)
    else
      spawn_weapon(reg, pos, q);  // plain steel (today's path)
  }
  for (const Vec2& pos : armour_drops) spawn_armour(reg, pos, fine_quality(rng));  // rolled defence
  for (const Vec2& pos : venom_drops) spawn_venom_weapon(reg, pos);  // poison build (baseline)
  // A fallen colonist's KIT — a PLAIN blade / plate at baseline quality (1.0), no rng draw:
  // recovered gear is the ordinary kind, not the finer loot a tough KILL rolls. Spawned after the
  // destroy loop like every other drop, from positions captured while the corpse was still valid.
  for (const Vec2& pos : kit_weapon_drops) spawn_weapon(reg, pos, 1.0f);
  for (const Vec2& pos : kit_armour_drops) spawn_armour(reg, pos, 1.0f);
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
      // FORTUNE stretches a find: Luck scales how much HEALTH an orb restores — the design's LCK =
      // "quality / richer finds", a SECOND effect for the fortune stat beside the crit it already
      // rolls (perform_attack). 1 + (LCK - 1) * kLuckYieldPerLevel, capped at kLuckYieldCap (x2 —
      // matching the WIS-awareness ceiling; dodge/crit cap lower, at 0.50). Reads the OWN
      // Attributes (fetched here and reused for the Scavenging grant below); no Attributes, or LCK
      // 1 (the spawn default and every existing collect test), -> x1 -> the orb heals exactly as
      // before (bit-identical). Only the restorative heal is made richer — the permanent max-HP
      // bump and the food stay flat. Pure float off the level, no RNG.
      Attributes* a = reg.try_get<Attributes>(p);
      constexpr float kLuckYieldPerLevel = 0.1f;  // each Luck level past 1 draws 10% more heal...
      constexpr float kLuckYieldCap = 2.0f;       // ...up to double (a knob)
      float luck_yield = 1.0f;
      if (a != nullptr) {
        luck_yield += static_cast<float>(a->luck.level - 1) * kLuckYieldPerLevel;
        if (luck_yield > kLuckYieldCap) luck_yield = kLuckYieldCap;
      }
      // A permanent max-HP bump: grow base (which advance_progression keeps max in step
      // with each tick) and max now too — the direct max bump is also the only growth
      // for a collector without Attributes, whose max is never recomputed.
      health.base += pk.bonus_max_hp;
      health.max += pk.bonus_max_hp;
      health.current += pk.heal * luck_yield;                        // a lucky scavenger mends more
      if (health.current > health.max) health.current = health.max;  // capped, no overheal

      // Eating it refills hunger — every Pickup carries its own food value, so the same
      // fight -> orb -> grab loop keeps you fed AND a harvested MEAL (pk.food set higher by
      // spawn_meal) fills you more than the loot you scavenge. An orb's default is 50 (the old
      // flat rate), so this is bit-identical for orbs.
      stats.hunger.current += pk.food;
      if (stats.hunger.current > stats.hunger.max) stats.hunger.current = stats.hunger.max;

      // Grabbing loot trains Scavenging -> Luck (deterministic XP, no RNG), so foraging
      // the field is itself a build: more Luck -> more crits (perform_attack). Guard on the
      // pair — a collector without Skills/Attributes (see the max-HP note above) still gets
      // the heal, it just doesn't grow Luck.
      Skills* sk = reg.try_get<Skills>(p);
      if (sk != nullptr && a != nullptr) {  // `a` fetched above for the Luck heal scaling
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

void study_spellbooks(entt::registry& reg) {
  // The design's "magic is LEARNED" made real: a person standing on a Spellbook READS it and gains
  // the Spellcasting skill, so casting is EARNED by finding a tome rather than innate. The learning
  // twin of collect_pickups (walk over a grounded item to gain from it). A Spellbook is a PERMANENT
  // LIBRARY, not a one-shot scroll: reading it does NOT consume it, so a whole colony can learn
  // from a single tome over time — the supply the Scholar aspiration needs (every greedy
  // reinforcement that seeks it can become a mage), and the player no longer "steals" the only book
  // by reaching it first. ANY person with a Skills sheet reads — the player AND an NPC (the
  // player==NPC parity: a colonist that finds a tome becomes a mage too, which npc_cast then lets
  // cast). Creatures carry no Skills, so they never reach here; Downed bodies are excluded. Gated
  // on NOT already knowing the spell (a caster leaves the tome), so a re-read is a no-op and the
  // lectern teaches each newcomer once. Mutates only component VALUES (a learned-skill push) and
  // destroys nothing, so no view is invalidated mid-walk — no collect-then-act needed.
  constexpr float kStudyReach = 15.0f;  // same reach as a pickup grab
  auto books = reg.view<Spellbook, Transform>();
  auto learners = reg.view<Skills, Transform>(entt::exclude<Downed>);
  for (const entt::entity book : books) {
    const Vec2 book_pos = books.get<Transform>(book).position;
    for (const entt::entity p : learners) {
      if (glm::distance(learners.get<Transform>(p).position, book_pos) >= kStudyReach) continue;
      Skills& sk = learners.get<Skills>(p);
      if (sk.find(SkillId::Spellcasting) != nullptr) continue;  // already a caster — leave the tome
      sk.train(SkillId::Spellcasting);                          // READ it: learn to cast
      break;  // one newcomer studies the lectern per tick; the book stays for the next
    }
  }
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
  constexpr float kEnrageThreshold = 0.3f;  // below this fraction of its HP, a creature enrages...
  constexpr float kEnrageDamage = 1.75f;    // ...and its blows hit this much harder (knobs)
  constexpr float kRiposteDamage =
      4.0f;  // flat chip a guarding defender deals BACK per blow turned
  constexpr float kRiposteStaminaCost = 15.0f;  // ...and the vigor each riposte spends (knob)
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
      // for the mobility it costs. Applies to anyone Blocking — the player (MovePlayer) and a
      // hardened NPC bulwark (npc_guard) alike, so both soak, riposte, and train Guarding.
      if (reg.all_of<Blocking>(p)) {
        // Only kBlockDamageFactor of the blow gets through a raised guard. GUARDING eases even
        // that: a trained guard TURNS MORE of it (less through), via the same half-floor mastery
        // helper the STR-carry / fatigue banes use — so the skill that until now only fed Endurance
        // XP finally sharpens the block it is earned by. Level 1, or no Guarding skill (a bare
        // Blocking victim),
        // -> relief 0 -> the flat 0.4 -> bit-identical. eased_bane caps the relief at half, so a
        // master still takes a fifth of the blow — a guard is softened, never a wall. Fetch Skills
        // once here; the training grant just below reuses it.
        Skills* gsk = reg.try_get<Skills>(p);
        float block_factor = kBlockDamageFactor;
        if (gsk != nullptr)
          if (const Skill* guard = gsk->find(SkillId::Guarding))
            block_factor = eased_bane(block_factor, guard->level);
        attack_dmg *= block_factor;
        // Turning a blow TRAINS you to guard: a raised guard under fire builds the Guarding skill
        // -> Endurance (the design's VIT skill, Toughness's active twin — Toughness grows Endurance
        // by SURVIVING a hit, Guarding by TURNING one). So blocking, the ONE combat action that
        // used to train nothing, now grows a defensive build like every other — and Endurance is
        // well spent: VIT already buys bigger pools AND softer blows (defence_of) AND
        // poison-resist, so a guard-tank sharpens across the board. Reuses `attrs` (fetched for the
        // dodge roll above) and `gsk` (fetched for the block ease above) — a Blocking entity
        // without the progression pair (none today but the player) just doesn't grow, so it's
        // bit-identical for anyone not progressing. No RNG.
        const Fixed kGuardingPerBlock =
            Fixed::from_int(10);  // XP per blow turned (matches a swing)
        if (gsk != nullptr && attrs != nullptr) {
          grant_skill_xp(*gsk, *attrs, SkillId::Guarding, kGuardingPerBlock,
                         reg.try_get<CharacterLevel>(p));
        }
        // RIPOSTE: turning a blow bites back — but it's an EXERTION that spends
        // kRiposteStaminaCost, so it fires only while you have the vigor. A WINDED guard still
        // SOFTENS but can't riposte; and because a raised guard gives no second wind
        // (update_stamina skips recovery while Blocking), a prolonged hold bleeds stamina it can't
        // refill — so guard-tanking a lone foe is a RHYTHM (turn blows till winded, then lower the
        // guard to recover, exposed), not a risk-free stand-and-win. Pure sim (no RNG). It routes
        // through the creature's own Stats, so a riposte that lands the last hit reaps it via
        // handle_deaths like any other damage (loot still drops). It credits NO Valor, though:
        // Valor rewards an ACTIVE strike, and a riposte is the creature breaking itself on your
        // guard, not a blow you threw.
        Vital& p_stamina = prey.get<Stats>(p).stamina;
        Stats* cs = reg.try_get<Stats>(c);
        if (p_stamina.current >= kRiposteStaminaCost && cs != nullptr) {
          p_stamina.current -= kRiposteStaminaCost;  // turning the blow tires you
          cs->health.current -= kRiposteDamage;
          if (cs->health.current < 0.0f) cs->health.current = 0.0f;
          stamp_flash(reg, c);  // the recoiling creature blinks too, so the riposte reads
        }
      }
      // DON'T TURN YOUR BACK: a victim FLEEING this creature — moving away, its back turned — takes
      // the flank harder (kBackstabBonus), the exact MIRROR of the backstab a fighter lands on a
      // fleeing creature (perform_attack), through the SAME shared backstab_multiplier. So running
      // from a beast is a real risk, not a free escape — a colonist the flee rung is carrying away
      // from a hazard pays for its exposed back. Applied AFTER mitigate, exactly as perform_attack
      // scales its final `dealt` — a flank finds the vital spot, so armour/VIT don't blunt the
      // bonus, and the two sides stay symmetric. A STANDING or TURNING-TO-FIGHT victim (facing the
      // creature, or with no Velocity) -> x1, so every still-victim contact test is bit-identical.
      // No RNG.
      float applied = mitigate(attack_dmg, defence_of(reg, p));
      applied *=
          backstab_multiplier(c_pos, prey.get<Transform>(p).position, reg.try_get<Velocity>(p));
      // A cast SHIELD soaks the FINAL blow: a barrier (Shielded, from shield_spell) absorbs
      // `absorb` off the damage that got through armour and the flank — the last line of defence, a
      // temporary mana-bought buffer on top of static VIT/armour. Floored at 0: unlike mitigate's
      // PERMANENT 10% chip floor, a timed, mana-costed, EXPIRING barrier is allowed to fully eat a
      // weak blow — that's the point of raising it, and you paid mana while it ticks away. Read
      // AFTER the flank so even a backstab is soaked (a barrier doesn't care about the angle).
      // Unshielded (no Shielded, every existing contact) -> unchanged, bit-identical. The
      // contact-consequences below (venom, lifesteal, armour-wear, thorns) STILL fire on the LANDED
      // blow regardless — the shield stops DAMAGE, not CONTACT: the fang still breaks skin, the
      // plate still scuffs. (A poison-ward that also blocks venom is a separate spell — a
      // follow-up.)
      if (const Shielded* shield = reg.try_get<Shielded>(p); shield != nullptr) {
        applied -= shield->absorb;
        if (applied < 0.0f) applied = 0.0f;
      }
      Vital& health = prey.get<Stats>(p).health;
      health.current -= applied;
      if (health.current < 0.0f) health.current = 0.0f;
      train_on_damage(reg, p, applied);  // enduring a creature's blow toughens the victim
      stamp_flash(reg, p);               // the struck victim blinks white

      // A venomous archetype's landed blow LINGERS: (re)apply Poisoned via the shared apply_venom
      // so the venom keeps chipping after the swarm moves on — the reason a fast swarm is scary to
      // disengage from (refresh the clock, keep the worst potency). Non-venomous creatures
      // (poison_per_second 0) skip this, so an unpoisoned world is unchanged.
      if (enemy.poison_per_second > 0.0f) apply_venom(reg, p, enemy.poison_per_second);

      // LIFESTEAL: a LEECH archetype DRINKS on a landed blow — it heals lifesteal_per_hit (capped
      // at its own max), the ONLY creature self-heal in the game (creatures otherwise never regen).
      // So a leech REVERSES the wear-down: chip it slowly and it out-sustains you, feeding on every
      // hit it lands, so you must BURST it or DENY its bites (kite it) rather than trade blows.
      // "Procs as data": a non-leech creature (lifesteal_per_hit 0, every archetype today) heals
      // nothing, so an unchanged world is bit-identical. Routes through the creature's own Stats
      // (its own block-scope `cs`, like the thorns/riposte reads); a full-health leech is unchanged
      // (capped). No RNG. A DEAD leech can't drink (`health.current > 0`): a leech clamped to
      // exactly 0 HP earlier this tick — by the player's perform_attack (the tick's top), or a
      // guard's riposte above (thorns is BELOW this drink, so it can't be the same-swing cause) —
      // still lands this last dying swing (the intended quirk), but WITHOUT this guard its
      // lifesteal would heal it back above 0, so handle_deaths (later this tick) would never reap
      // it and the kill would be undone. 0 HP is inert: the drink is the one creature self-heal, so
      // this is the one place the wear-down could be reversed from the grave.
      if (enemy.lifesteal_per_hit > 0.0f) {
        if (Stats* cs = reg.try_get<Stats>(c); cs != nullptr && cs->health.current > 0.0f) {
          cs->health.current += enemy.lifesteal_per_hit;
          if (cs->health.current > cs->health.max) cs->health.current = cs->health.max;
        }
      }

      // ARMOUR WEAR: the plate that just softened this blow (defence_of above) wears by one — the
      // defensive twin of a blade dulling on a swing. At 0 it SHATTERS: the armour slot clears and
      // the wearer is bare again (and the cache is dropped if no weapon remains, so an NPC re-seeks
      // gear). Only a victim actually WEARING armour (armour_durability > 0) wears, so a bare or
      // weapon-only victim is bit-identical; and the mitigation above already used the full
      // defence, so the breaking blow was still softened. Equipped isn't in the prey view, so this
      // is view-safe like the Poisoned emplace above.
      if (Equipped* pg = reg.try_get<Equipped>(p); pg != nullptr && pg->armour_durability > 0.0f) {
        // WARDED (thorns): a spiked plate REFLECTS a flat chip back onto the creature that just
        // struck it — armour's first flavourful trait, the defensive twin of a venom blade's proc.
        // It routes through the creature's OWN Stats (like the guard's riposte), so a thorns hit
        // that lands the last blow reaps it via handle_deaths (loot still drops); it credits NO
        // Valor (passive — the beast breaks itself on your spikes, not a blow you threw). A plain
        // plate (armour_thorns 0, every plate spawned today) reflects nothing, so an unwarded world
        // is bit-identical. Reuses the `pg` fetched for wear and the creature `c` in scope; no RNG.
        if (pg->armour_thorns > 0.0f) {
          if (Stats* cs = reg.try_get<Stats>(c); cs != nullptr) {
            cs->health.current -= pg->armour_thorns;
            if (cs->health.current < 0.0f) cs->health.current = 0.0f;
            stamp_flash(reg, c);  // the pricked creature blinks too, so the thorns read
          }
        }
        pg->armour_durability -= 1.0f;
        if (pg->armour_durability <=
            0.0f) {  // plate shattered — clear the armour slot, keep a weapon
          pg->defence_bonus = 0.0f;
          pg->stamina_regen_penalty = 0.0f;
          pg->armour_durability = 0.0f;
          pg->armour_thorns = 0.0f;  // the spikes go with the shattered plate
          remove_equipped_if_empty(reg, p, *pg);
        }
      }
    }
  }
}

void tick_poison(entt::registry& reg, float dt) {
  // Chip each poisoned entity's health by its venom, age the timer, and reap the status when spent.
  // Collect-then-remove: removing a component mid-view-walk invalidates the iterator (the ECS trap
  // decay_flashes/handle_deaths avoid). The chip routes through handle_deaths (reaps at 0), exactly
  // like starvation/dehydration — so venom can be lethal. Regen can't claw it back
  // (regenerate_vitals gates healing OFF while poisoned, like starvation), so hardiness resists
  // venom DIRECTLY: VIT (Endurance) shaves the chip — the DoT counterpart of how it softens a BLOW
  // via defence_of, so a tough constitution shrugs off both a hit and a poison.
  constexpr float kResistPerVit = 0.05f;  // 5% less venom per Endurance level past the first...
  constexpr float kResistCap = 0.75f;  // ...capped so a hardy body still takes at least a quarter
  std::vector<entt::entity> cured;
  auto view =
      reg.view<Poisoned, Stats>(entt::exclude<Downed>);  // a downed body is inert (invariant)
  for (const entt::entity e : view) {
    Poisoned& venom = view.get<Poisoned>(e);
    Vital& health = view.get<Stats>(e).health;
    // VIT resistance: level 1 (or no Attributes) resists 0, so an untrained or bare entity chips
    // EXACTLY as before (bit-identical). Capped so venom is never fully negated — the DoT mirror of
    // mitigate's 10% chip floor.
    Attributes* attrs = reg.try_get<Attributes>(e);  // read for the resist, reused to train below
    float resist = 0.0f;
    if (attrs != nullptr) {
      resist = static_cast<float>(attrs->endurance.level - 1) * kResistPerVit;
      if (resist > kResistCap) resist = kResistCap;
    }
    health.current -= venom.damage_per_second * (1.0f - resist) * dt;
    if (health.current < 0.0f) health.current = 0.0f;
    // ENDURING venom trains RESISTANCE -> Endurance, the poison twin of Toughness (a survived HIT
    // -> Toughness): a character that keeps shrugging off venom grows the very VIT that shaves it
    // (the resist above) — immunity through exposure. Guarded on Skills/Attributes (a bare or
    // no-progression poisoned entity trains nothing -> bit-identical); a flat per-tick grant, so a
    // longer or stronger venom trains MORE only by lasting more ticks. Player AND NPC both get
    // poisoned, so both build it. No RNG.
    if (Skills* sk = reg.try_get<Skills>(e); sk != nullptr && attrs != nullptr) {
      const Fixed kResistancePerTick =
          Fixed::from_ratio(10, 60);  // ~10 XP/sec while poisoned (knob)
      grant_skill_xp(*sk, *attrs, SkillId::Resistance, kResistancePerTick,
                     reg.try_get<CharacterLevel>(e));
    }
    venom.remaining -= dt;
    if (venom.remaining <= 0.0f) cured.push_back(e);
  }
  for (const entt::entity e : cured) reg.remove<Poisoned>(e);
}

void tick_shield(entt::registry& reg, float dt) {
  // Age each cast barrier and reap it when spent — the BUFF twin of tick_poison's DoT decay (same
  // collect-then-remove shape: removing a component mid-view-walk invalidates the iterator, the ECS
  // trap). No exclude<Downed>: a shield on a body that goes Downed just keeps counting down and
  // lapses harmlessly — a Downed body takes no creature blows anyway (resolve_creature_contacts
  // excludes it), so the barrier has nothing to soak; letting it expire is cleaner than special-
  // casing the down. Runs AFTER resolve_creature_contacts, so a barrier still soaks the blows of
  // the tick it's cast before this ages it (apply-at-cast, use-at-contact, decay-here — poison's
  // rhythm).
  std::vector<entt::entity> spent;
  auto view = reg.view<Shielded>();
  for (const entt::entity e : view) {
    Shielded& shield = view.get<Shielded>(e);
    shield.remaining -= dt;
    if (shield.remaining <= 0.0f) spent.push_back(e);
  }
  for (const entt::entity e : spent) reg.remove<Shielded>(e);
}

void tick_panic(entt::registry& reg, float dt) {
  // Count down each routed colonist's PANIC and let it pass — the acute grief reaction wears off
  // after a few seconds and the colonist recovers its nerve (steer_npcs stops boosting its flee).
  // The timer twin of tick_poison: collect-then-remove so a reaped marker never invalidates the
  // view mid-walk. No Panicked anywhere (every world without a fresh bereavement) -> the view is
  // empty -> a no-op, bit-identical. Draws no RNG.
  std::vector<entt::entity> recovered;
  auto routed = reg.view<Panicked>();
  for (const entt::entity e : routed) {
    Panicked& p = routed.get<Panicked>(e);
    p.remaining -= dt;
    if (p.remaining <= 0.0f) recovered.push_back(e);
  }
  for (const entt::entity e : recovered) reg.remove<Panicked>(e);
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
