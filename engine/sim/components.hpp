#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <entt/entity/fwd.hpp>  // entt::entity (a component stores one — the projectile's target/owner)

#include "engine/core/fixed.hpp"
#include "engine/core/math.hpp"
#include "engine/sim/types.hpp"

// Components: the DATA that entities are made of.
//
// The engine uses an ECS (Entity Component System, via the EnTT library). The
// core idea, and the reason we do NOT use classes like `class Enemy : public
// Actor` (code-design-rules rule 1: "an entity is an ID"):
//
//   - An ENTITY is just an id (a number). It has no data or behaviour itself.
//   - A COMPONENT is a plain struct of data (below). An entity "has" a
//     component if the world stores one for that id.
//   - A SYSTEM (see systems.hpp) is a free function that runs over every entity
//     that has a given set of components.
//
// You build a "kind of thing" by giving an entity a COMBINATION of components,
// not by subclassing. A player is an entity with Transform + Velocity +
// PlayerControlled + RenderDot. A drifting mote is the same minus
// PlayerControlled. Adding a new capability later = a new component + a system,
// touching nothing that already exists. That composability is why engines use
// this pattern (handbook: "Composition over inheritance").
//
// Keep components small and copyable — they get iterated tightly and, later,
// serialized for network/saves.

namespace eng::sim {

// Where an entity is, in world units.
struct Transform {
  Vec2 position{0.0f, 0.0f};
};

// The entity's position as of the PREVIOUS tick. The simulation steps at a fixed
// rate but the screen refreshes faster, so the renderer draws somewhere between
// the last two ticks — interpolating PrevTransform -> Transform — to look smooth
// (see simulation.hpp). Updated at the start of every step.
struct PrevTransform {
  Vec2 position{0.0f, 0.0f};
};

// How fast it is moving, in world units per second.
struct Velocity {
  Vec2 value{0.0f, 0.0f};
};

// Marks an entity as driven by a player's input. The player id lets the command
// funnel route "player 1 pushed left" to the right entity — trivial in
// single-player, essential once several players share a world.
struct PlayerControlled {
  PlayerId player = kLocalPlayer;
  // Top speed while an input is held, in world units per second. The command
  // funnel sets velocity = clamped_input_direction * move_speed directly — there
  // is no acceleration or inertia yet, so the entity starts and stops instantly.
  float move_speed = 300.0f;
};

// A character's PERSONALITY — innate leanings that BEHAVIOURS read to make decisions (the P7 seed),
// and that the character's own DEEDS slowly reshape (record_deed drifts the matching axis — "you
// are what you do"; see morality). The design's model has six int8 axes in [-100, +100]: bravery,
// compassion, industry, loyalty, greed, sociability. ALL SIX are now wired to a behaviour. Neutral
// 0 = "no leaning", so an entity with all-zero axes — or no Personality at all — behaves exactly as
// it did before this existed (bit-identical).
struct Personality {
  std::int8_t bravery = 0;      // [-100 coward .. +100 brave]; shapes how near a hazard gets before
                                // an NPC flees (steer_npcs). A coward bolts early, the brave hold.
  std::int8_t greed = 0;        // [-100 selfless .. +100 greedy]; shapes how hungry an NPC must get
                                // before it forages (a NEED THRESHOLD, not bravery's radius — a
                                // second, differently-shaped read). Greedy hoards while well-fed.
  std::int8_t compassion = 0;   // [-100 callous .. +100 compassionate]; shapes rescue SPEED (a
                                // third knob-shape) — the compassionate SPRINT to a fallen ally,
                                // the callous trudge and may not beat the Downed timer at all.
  std::int8_t industry = 0;     // [-100 idle .. +100 industrious]; shapes how far an NPC ranges
                                // to ARM itself: the industrious cross the field to loot a weapon,
                                // the idle grab one underfoot.
  std::int8_t sociability = 0;  // [-100 loner .. +100 sociable]; shapes how far an idle NPC ranges
                                // to RALLY to a hero (the steer ladder's last rung): the sociable
                                // cross the field to gather round a champion, the loner keep to
                                // themselves. Reuses industry's radius SHAPE on a social want.
  std::int8_t loyalty = 0;      // [-100 fickle .. +100 loyal]; shapes how far an idle NPC
                                // ranges to FOLLOW a bonded friend (the relationships
                                // bond-pull): the loyal cross the field to stay near an
                                // ally they rescued, the fickle follow only one underfoot.
};

// The KINDS of moral deed a character can accrue — the design's six behaviour-ledger dimensions.
// Charity and Valor are the hero signals; Cruelty and (unjust) Violence the villain signals;
// Honesty and Loyalty round them out. `Count` is a sentinel that SIZES the ledger array below and
// MUST stay last — adding a real dimension before it automatically grows the array, so the two can
// never desync. Every deed the sim records is one of these, funnelled through record_deed
// (systems.hpp).
enum class Deed : std::uint8_t { Violence, Honesty, Loyalty, Charity, Cruelty, Valor, Count };

// A character's EARNED moral history: how much of each Deed kind they have accumulated over a life.
// The directly-accumulated counterpart of the innate, only-slowly-drifting Personality — different
// in nature and lifetime, so it is a SEPARATE component. It is added LAZILY: an entity earns a
// ledger only on its FIRST deed (record_deed does the get_or_emplace), so anyone who never acts has
// none and replays exactly as before this existed (bit-identical). int32 (not Personality's int8)
// because deeds ACCUMULATE over a life and must clear the design's hero gate (standing > +500),
// well past int8's ±127; an int32 array element also lets `dims[i] += mag` add without a cast under
// -Wconversion.
struct BehaviorLedger {
  std::array<std::int32_t, static_cast<std::size_t>(Deed::Count)> dims{};
  // A leaky-decay tick counter (read/written only by decay_standing): every kDecayPeriod ticks each
  // nonzero dim creeps ONE step toward 0, so a reputation FADES if it isn't renewed — the design's
  // redemption/corruption "for free". An EXACT integer count (no float, so bit-exact and rounding-
  // free), 0 on a fresh ledger. Not read by standing(), so a short-run world is bit-identical until
  // a whole period elapses.
  std::int32_t decay_ticks = 0;
};

// The one derived scalar the whole morality system collapses to: positive = heroic repute,
// negative = villainous. A PURE function of the ledger (reads no sim state), so it is unit-testable
// and recomputed on demand rather than stored — the design's "multi-dimension ledger -> one derived
// standing". Weights are the design's exact .8/1.0/.6/.6/-1.2/-.8 scaled ×5 — the smallest scale
// that makes every one an integer (each is a multiple of 0.2) — so NO float enters the sim and
// replay stays bit-identical; the unit is thus "fifths of a design-point". Charity/Valor lift you,
// Cruelty/Violence sink you. Five deeds feed it so far — Charity, Valor and Loyalty (rescuing a
// bonded ally) lift, Cruelty (the first villain deed) sinks it below zero, and Violence (a cruel
// strike that KILLS) sinks it further — proving the signed formula in both directions; only Honesty
// stays 0 until a truth/deceit deed lands. The formula was locked whole from the start, so each new
// deed is a one-line add here, never a reshape.
inline std::int32_t standing(const BehaviorLedger& led) {
  const auto d = [&](Deed k) { return led.dims[static_cast<std::size_t>(k)]; };
  return d(Deed::Charity) * 4 + d(Deed::Valor) * 5 + d(Deed::Honesty) * 3 + d(Deed::Loyalty) * 3 -
         d(Deed::Cruelty) * 6 - d(Deed::Violence) * 4;
}

// One directed tie: THIS entity -> `other`. The seed of the design's P8 RELATIONSHIPS (directed,
// sparse, small event-deltas). int8 like a Personality axis — affinity SATURATES into bond bands,
// it doesn't accumulate over a life toward a gate the way ledger dims do — so no wide int and no
// float ever enters the sim. Only `affinity` is FED for now (the design's R1 slice); the bond
// ladder (Acquaintance -> Friend -> Partner / Rival -> Nemesis) has ALREADY landed as the DERIVED
// band `bond_tier(affinity)` below — a pure query, never a stored slot, exactly the standing ->
// standing_title split. The one still-future sibling is `trust`: a one-field append the day its own
// event lands, with NO reshape.
struct Relation {
  entt::entity other = entt::null;  // directed: A->B is a separate edge from B->A
  std::int8_t affinity = 0;         // [-100 dislike .. +100 like]
};

// A character's felt ties — how THIS entity regards others. Lazy + sparse + append-ordered, the
// Skills-vector pattern: deterministic iteration (a hash map's is not) and no heap churn until a
// real bond forms. Emplaced only on an entity's FIRST bond (nudge_affinity does the
// get_or_emplace), so a never-bonding entity — and the whole pre-relationships world — carries
// nothing and replays bit-identically, exactly as BehaviorLedger stays absent for the never-acting.
// Stored by value: entity ids RECYCLE, so every READER must gate on reg.valid(other) before
// touching the target.
struct Relationships {
  // ponytail: unbounded edge list. Fine while ties are rare (they form only on a rescue, only
  // toward a downed player); cap-N + evict-weakest-affinity only if a colonist ever bonds with
  // hundreds.
  std::vector<Relation> edges;
  // A leaky-decay tick counter (read/written only by decay_bonds) — the affinity twin of
  // BehaviorLedger::decay_ticks: every kBondDecayPeriod ticks each UNLATCHED edge creeps one step
  // toward 0, so a tie COOLS if it isn't renewed. Exact integer (no float), 0 on a fresh set; no
  // behaviour reads it, so a short-run world is bit-identical until a whole period elapses.
  std::int32_t decay_ticks = 0;
};

// The affinity band EDGES, named once and shared by `bond_tier` and `bond_latched`. Aligned to the
// behavioural thresholds so a band's NAME matches what the sim already DOES at that affinity:
// Acquaintance begins at +10 (`kBondPull` — a tie strong enough to pull you toward a friend), Rival
// at -20 (`kGrudgeThreshold` — a grudge deep enough to abandon the resented). Partner/Nemesis are
// the DEEP bands that LATCH (resist the leaky bond decay).
inline constexpr std::int8_t kBondPartnerAt = 80;
inline constexpr std::int8_t kBondFriendAt = 40;
inline constexpr std::int8_t kBondAcquaintanceAt = 10;  // == kBondPull
inline constexpr std::int8_t kBondRivalAt = -20;        // == kGrudgeThreshold
inline constexpr std::int8_t kBondNemesisAt = -60;

// The design's bond LADDER as a DERIVED band — the relationships twin of `standing_title`: a pure
// query naming where an `affinity` value falls (Nemesis .. Rival .. Neutral .. Acquaintance ..
// Friend .. Partner), never a stored slot, so it's always in sync with the number behind it.
inline const char* bond_tier(std::int8_t affinity) {
  if (affinity >= kBondPartnerAt) return "Partner";
  if (affinity >= kBondFriendAt) return "Friend";
  if (affinity >= kBondAcquaintanceAt)
    return "Acquaintance";  // a real bond that pulls you (kBondPull)
  if (affinity <= kBondNemesisAt) return "Nemesis";
  if (affinity <= kBondRivalAt) return "Rival";  // a grudge that abandons (kGrudgeThreshold)
  return "Neutral";
}

// Whether a tie is LATCHED — a deep bond (Partner) or a deep grudge (Nemesis) — so it RESISTS the
// leaky bond decay (`decay_bonds`): the strongest ties, for good or ill, PERSIST rather than
// quietly fading. The design's "bonds latch, resist decay". Reuses bond_tier's deep-band edges so
// the two can never drift.
inline bool bond_latched(std::int8_t affinity) {
  return affinity >= kBondPartnerAt || affinity <= kBondNemesisAt;
}

// Marks an entity as a non-player character. Empty for now — its whole job is to
// answer "is this a person the world runs, rather than the player?" so systems
// can treat the two differently (most importantly: an NPC that dies is destroyed,
// not respawned — permadeath, the game's core rule). This is the seed of the
// tiered NPC identity from the master plan: later, "Named" NPCs gain more
// components (a name, relationships, skills) while this marker stays the floor.
struct Npc {};

// Marks a player who has dropped to 0 HP but isn't gone yet — crumpled WHERE they fell,
// helpless (movement input ignored, no self-heal), for `timer` seconds. A living ally who
// reaches them hauls them up in place; if the timer runs out first they respawn at the
// field centre instead. This is the design's faithful death beat — "player -> Downed:
// ally-rescuable / expiry respawns / (hardcore) permadeath" — replacing the old instant
// teleport-to-safety-plus-full-heal. Only PLAYERS get Downed; an NPC at 0 HP is permadeath.
struct Downed {
  float timer = 5.0f;  // seconds of helplessness before an unrescued respawn (a knob)
};

// A colonist ROUTED by grief: it just watched a bonded friend fall (handle_deaths emplaces this),
// and for `remaining` seconds it PANICS — steer_npcs makes it sense danger from much farther, flee
// even the CREATURES it would normally stand against, and bolt faster. The ACUTE morale beat on top
// of grief's slow, permanent bravery drift — a friend's death shakes you for good AND panics you
// now. tick_panic counts it down and removes it. No marker (the pre-bond world, and any survivor
// who has not just lost a friend) -> steer_npcs is unchanged, so it is bit-identical.
struct Panicked {
  float remaining = 0.0f;  // seconds of rout left; tick_panic decrements, reaps at 0
};

// Presentation-only: how the debug renderer should draw this entity. The
// simulation never reads this — it exists so the client has something to show.
// Colours are 0..1 RGB.
struct RenderDot {
  Vec3 color{0.9f, 0.9f, 0.9f};
  float radius = 6.0f;
};

// How long a hit-flash lasts, in seconds (~9 ticks at 60Hz). A knob: bigger =
// blows linger longer on screen. Shared because the sim STAMPS it (systems.cpp)
// and the renderer READS it to fade the flash (main.cpp).
inline constexpr float kHitFlashSeconds = 0.15f;

// Presentation-only, like RenderDot: a brief white blink so a blow REGISTERS on
// screen. `remaining` counts down from kHitFlashSeconds; the renderer whitens the
// dot in proportion, so a fresh hit is near-white and fades. It is sim-side state
// only so it stays deterministic and testable (stamped at the damage sites, decayed
// by the fixed dt) — but the simulation MUST NOT branch on it for any rule. Only the
// renderer reads it. The moment a system reads HitFlash for a decision, it stops
// being presentation and this invariant is broken.
struct HitFlash {
  float remaining = 0.0f;
};

// How dark a near-dead dot gets. A floor (not 0) so a wounded entity dims to an ember
// but never vanishes — you can still see the fight it's losing. A presentation knob.
inline constexpr float kWoundedFloor = 0.35f;

// Presentation-only, the steady twin of HitFlash: how bright to draw an entity given its
// health, so the renderer can DIM a wounded dot (HitFlash shows the blow; this shows the
// accumulated toll). Returns a 0..1 multiplier for the dot colour — full health = 1.0
// (drawn exactly as authored), scaling down to kWoundedFloor at 0 HP. A pure function of
// health, so it's unit-testable and reads no sim state itself; the renderer calls it.
// `max <= 0` (an entity with no real health bar) returns 1.0 — unharmed, and no divide.
inline float wounded_brightness(float current, float max) {
  if (max <= 0.0f) return 1.0f;
  float frac = current / max;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f)
    frac = 1.0f;  // clamp: over-heal or transient overshoot never brightens past full
  return kWoundedFloor + (1.0f - kWoundedFloor) * frac;
}

// Presentation-only: how strongly to tint a POISONED dot toward sickly green — the visual for the
// venom subsystem (swarmer bite, venom blade, venom spit), so you can SEE who's envenomed. Returns
// a 0..cap mix factor that GROWS with the venom's `damage_per_second` (a nastier dose glows
// greener), linearly to kPoisonTintPerDps, capped at kPoisonTintCap so the dot never fully greens
// out and stays readable. A pure function of the dps (the renderer reads the Poisoned component and
// passes it in), so it's unit-testable and the sim never reads colour. 0 dps -> 0 (no tint).
inline constexpr float kPoisonTintPerDps = 0.06f;  // green mix added per point of venom/second...
inline constexpr float kPoisonTintCap = 0.6f;      // ...capped, so even a heavy dose stays legible
inline float poison_tint_strength(float damage_per_second) {
  const float s = damage_per_second * kPoisonTintPerDps;
  if (s < 0.0f) return 0.0f;  // no negative venom, but guard the mix factor anyway
  return s < kPoisonTintCap ? s : kPoisonTintCap;
}

// How saturated the personality tint gets at the ±100 extremes. A presentation KNOB — the
// warm/cool ramp reads differently against the green base, enemy-red, player-blue, and the
// window background, so it wants eyeballing in the live renderer, not just a unit test.
inline constexpr float kPersonalityTintStrength = 0.4f;

// Presentation-only, like wounded_brightness: a colour MULTIPLIER that tints a dot by its
// BRAVERY so a colonist's nerve reads on screen — build_scene spawns a brave/coward spread you
// otherwise couldn't tell apart. Warms the brave toward yellow (more red, less blue) and cools
// the coward toward teal (less red, more blue), leaving GREEN untouched so a tinted NPC stays
// green-dominant — never mistaken for enemy-red or player-blue. Bravery 0 (or no Personality)
// returns exactly {1,1,1}: no tint, bit-identical. Pure and unit-testable; the renderer
// multiplies the dot's colour by it. Only bravery for now (the most behaviourally loaded axis,
// read twice); ponytail: a second-axis cue (e.g. greed) would need a channel this doesn't use.
inline Vec3 personality_tint(std::int8_t bravery) {
  const float t = static_cast<float>(bravery) / 100.0f;  // [-1 coward .. +1 brave]
  return Vec3{1.0f + t * kPersonalityTintStrength, 1.0f, 1.0f - t * kPersonalityTintStrength};
}

// How much a fully-renowned dot swells / a fully-infamous one shrinks, and the standing it takes to
// reach either — presentation KNOBS. STANDING is shown as PRESENCE (size): the channel the bravery
// tint (colour) and wounded dimming (brightness) leave free, so the three cues never fight. A hero
// looms, a villain draws small and shunned, neutral is authored size. ~10 kills / ~13 rescues swell
// to the cap; ~8 betrayals (Cruelty x6) shrink to the floor.
inline constexpr float kRenownMaxScale = 0.3f;     // +30% radius at full renown
inline constexpr std::int32_t kRenownFullAt = 50;  // |standing| at which the size change caps
inline constexpr float kInfamyMaxShrink = 0.3f;    // -30% radius at full infamy (floored, never 0)

// Presentation-only, the morality twin of personality_tint: how much to scale a dot's radius for a
// character's STANDING (its derived morality scalar). SYMMETRIC about neutral — 1.0 at standing 0,
// rising linearly to 1 + kRenownMaxScale at +kRenownFullAt (a hero looms) and falling to
// 1 - kInfamyMaxShrink at -kRenownFullAt (a villain shrinks to a shunned husk), CAPPING at either
// end so nobody balloons or vanishes. A pure function of the scalar (the renderer computes
// standing() and passes it in), so it's unit-testable and the sim never reads it.
inline float renown_scale(std::int32_t standing_value) {
  const float cap = static_cast<float>(kRenownFullAt);
  const float s = static_cast<float>(standing_value);
  if (standing_value >= 0) {
    const float t = standing_value >= kRenownFullAt ? 1.0f : s / cap;
    return 1.0f + t * kRenownMaxScale;  // heroes loom
  }
  const float t = standing_value <= -kRenownFullAt ? 1.0f : -s / cap;
  return 1.0f - t * kInfamyMaxShrink;  // villains shrink to a shunned husk
}

// How much brighter a fine item glints per point of quality above baseline, and the ceiling so an
// exceptional item catches the eye without blowing out to a featureless white blob. Presentation
// KNOBS — tuned against the steel-grey / bronze gear dots in the live renderer.
inline constexpr float kQualitySheenPerPoint = 0.8f;  // +80% of the quality surplus, as brightness
inline constexpr float kQualitySheenCap = 1.4f;  // ...but never brighter than +40%, stays a dot

// Presentation-only, a sibling of wounded_brightness: how bright to draw a GROUNDED item given its
// QUALITY, so a FINER drop (a tough kill's loot, quality > 1.0) glints against a baseline one and
// you can SEE at a glance which pickup is worth the grab — the visible half of per-source loot
// quality (the numbers already differ; this makes the eye read it). Returns a colour multiplier
// >= 1.0: baseline quality 1.0 (and any shoddy sub-1.0 item) -> exactly 1.0 (drawn as authored,
// bit- identical), rising with the quality SURPLUS to kQualitySheenCap. A pure function of quality
// (the renderer reads the Weapon/Armour component and passes its `quality` in), so it's
// unit-testable and the sim never reads colour. ponytail: BRIGHTENS only; a sub-1.0 "shoddy" DIM is
// the follow-up cue if worse-than-baseline gear ever ships (nothing drops below 1.0 today).
inline float quality_sheen(float quality) {
  if (quality <= 1.0f) return 1.0f;  // baseline (or shoddy) draws as authored — bit-identical
  const float sheen = 1.0f + (quality - 1.0f) * kQualitySheenPerPoint;
  return sheen < kQualitySheenCap ? sheen : kQualitySheenCap;
}

// The lower title threshold — cross it (in either direction) and you stop being anonymous. The
// upper one reuses kRenownFullAt, so the title flips to "Renowned" exactly when the renown dot-size
// caps.
inline constexpr std::int32_t kKnownAt = 15;

// A character's DEED-derived TITLE — the design's "derived recognition": a pure query over
// `standing`, never a stored slot, so it's always in sync with the deeds behind it. Five bands,
// symmetric about neutral (villain titles are ready but unreachable until a villain deed exists).
// Its build-derived twin is `build_title` (below), which reads what you've TRAINED rather than what
// you've DONE; the richer ones (Master Smith, Dragonslayer, the Butcher — from gear and specific
// skills) layer on the same pure-query idea. Unit-testable; the HUD shows it, the sim never reads
// it.
inline const char* standing_title(std::int32_t standing_value) {
  if (standing_value >= kRenownFullAt) return "Renowned";
  if (standing_value >= kKnownAt) return "Known";
  if (standing_value <= -kRenownFullAt) return "Notorious";
  if (standing_value <= -kKnownAt) return "Suspect";
  return "Unproven";
}

// How many of a SINGLE deed kind you must accrue before it becomes the thing you're KNOWN FOR —
// you've done a thing OFTEN enough to be named for it, not just once by fluke. Each record_deed
// adds 1, so this is "three of the same kind". Deliberately smaller than kKnownAt (which gates the
// signed, weighted standing scalar) because this counts RAW repetitions of ONE dimension, not a
// signed sum.
inline constexpr std::int32_t kEpithetAt = 3;

// A DEED-derived EPITHET — the THIRD axis of the design's derived recognition, orthogonal to its
// two siblings above: standing_title says how GOOD or BAD you are (the collapsed signed scalar),
// build_title says what kind of FIGHTER you are (your dominant trained attribute), and this says
// what you are KNOWN FOR (your most-repeated DEED). The ledger has always tracked all six Deed
// kinds SEPARATELY, but until now only the collapsed standing() scalar read them — this is the
// first reader of a single dimension, surfacing exactly the "the Butcher" the standing_title
// comment promised. A pure query over the ledger (never a stored slot), so it always matches the
// deeds behind it; the HUD shows it and the sim never reads it, so it can't affect replay. Returns
// nullptr when no single kind has reached kEpithetAt — an epithet is a badge you've earned or you
// haven't, unlike the always-present band titles, so the HUD simply omits the line. A never-acting
// entity carries no ledger at all and is handled at the call site with the same try_get the
// standing readout uses. Ties break in fixed Deed-enum order (the earliest-declared kind wins), so
// an equal tally of Cruelty and Valor brands you the Butcher over the Slayer — infamy sticks — and
// the pick is deterministic.
inline const char* deed_epithet(const BehaviorLedger& led) {
  std::size_t top = 0;  // index of the dominant deed kind so far; strict-> below keeps the earliest
  for (std::size_t i = 1; i < led.dims.size(); ++i) {
    if (led.dims[i] > led.dims[top]) top = i;
  }
  if (led.dims[top] < kEpithetAt) return nullptr;  // no kind repeated enough to earn a name yet
  switch (static_cast<Deed>(top)) {
    case Deed::Valor:
      return "the Slayer";  // many hostiles felled
    case Deed::Charity:
      return "the Savior";  // many downed allies hauled back up
    case Deed::Loyalty:
      return "the Faithful";  // stood by bonded allies when it counted
    case Deed::Cruelty:
      return "the Butcher";  // many peaceful colonists cut down
    case Deed::Violence:
      return "the Brutal";  // a lethal cruel strike (fed by kViolenceKill)
    case Deed::Honesty:
      return "the Honest";  // truth kept (band ready; its deed is still unfed)
    case Deed::Count:
      break;  // sentinel, never a real dimension
  }
  return nullptr;  // unreachable — top always indexes a real dim; satisfies -Wreturn-type
}

// A ROLE — the design's HERO / VILLAIN, the recognition that CONJOINS fame and deeds where each
// sibling title reads only one: standing_title says how famous you are (the signed scalar),
// deed_epithet what you're KNOWN FOR (your dominant deed), and this fuses them into the design's
// named role. A CHAMPION is Renowned AND dominantly a Slayer (standing >= kRenownFullAt ∧ Valor the
// top deed); a FIEND is the mirror, Notorious AND dominantly a Butcher (standing <= -kRenownFullAt
// ∧ Cruelty the top deed). So the label demands BOTH a reputation AND that it was earned the
// heroic/villainous way — "famous FOR heroism", not merely famous: a Renowned *Savior*
// (Charity-led) is celebrated but not a Champion, a martial pole distinct from the epithet's
// nuance. Returns nullptr for everyone else (the common case). A pure query over the ledger (never
// stored); the HUD shows it, the sim never reads it, so it can't touch replay. A fresh ledger ->
// standing 0, no dominant deed past kEpithetAt -> nullptr -> bit-identical. Uses the SAME
// dominant-deed argmax + earliest-enum tie-break as deed_epithet (so a Champion's epithet is always
// "the Slayer", a Fiend's "the Butcher"); duplicated rather than shared for a 3-line scan.
inline const char* hero_role(const BehaviorLedger& led) {
  const std::int32_t s = standing(led);
  std::size_t top = 0;  // the dominant deed kind (strict-> keeps the earliest on a tie)
  for (std::size_t i = 1; i < led.dims.size(); ++i)
    if (led.dims[i] > led.dims[top]) top = i;
  if (led.dims[top] < kEpithetAt) return nullptr;  // no deed repeated enough to have earned a name
  if (s >= kRenownFullAt && static_cast<Deed>(top) == Deed::Valor) return "Champion";
  if (s <= -kRenownFullAt && static_cast<Deed>(top) == Deed::Cruelty) return "Fiend";
  return nullptr;  // famous but not FOR heroism/villainy, or not famous enough
}

// Marks an entity as dangerous to touch. An entity with a Hazard deals `damage`
// to any player it overlaps and is then consumed — destroyed (see the
// resolve_contacts system). The drifting motes have this: touch one, take a hit,
// and it's gone.
struct Hazard {
  float damage = 20.0f;
};

// Marks a hostile creature — a real fight, not a throwaway mote. Unlike a Hazard it
// has HP (a Stats component) that attacks whittle down over several hits, and VIT
// (an Attributes component) that softens the blows it receives; it chases the player
// (chase_prey) and hurts them on contact (resolve_creature_contacts) WITHOUT being
// consumed. Dies for good through the same handle_deaths permadeath path as an NPC.
// `attack_damage` is its contact hit, before the victim's VIT defence mitigates it.
// It swings on a cooldown (`attack_timer` counts down), so touching one is a series
// of discrete blows, not a per-tick grind. `chase_speed` is per-creature so archetypes
// can differ — a slow tanky brute vs a fast fragile swarmer (see make_creature).
// What a creature leaves behind when it dies — keyed on its archetype, so the drop is
// DETERMINISTIC (no roll on the shared stream). The kinds keep the loot economy legible: a swarmer
// yields SUSTAIN (a health orb), a brute raw OFFENCE (a steel weapon), a sentinel DEFENCE (armour),
// and a spitter a VENOM fang (the poison-build blade) — so every archetype's kill pays out
// something you'd arm up with, and the venom weapon gets a renewable battlefield source instead of
// a single scene spawn. Exhaustive by design — handle_deaths switches on it with no default, so a
// new kind is a compile error until it's handled (the same -Wswitch discipline as
// SkillId/AttrId/CommandKind).
enum class DropKind : std::uint8_t { HealthOrb, Weapon, Armour, VenomWeapon };

struct Enemy {
  float attack_damage = 15.0f;
  float attack_timer = 0.0f;            // seconds until it can swing again; 0 = ready
  float chase_speed = 70.0f;            // how fast it closes on its prey (chase_prey)
  DropKind drop = DropKind::HealthOrb;  // on death: what this archetype leaves behind
  // If > 0, a landed attack also ENVENOMS the victim: it applies a Poisoned that chips health for a
  // while after. 0 = not venomous. "Procs as data" (the design's P4) — an archetype knob, not a
  // special case in the combat code. Both the swarmer's BITE (resolve_creature_contacts) and the
  // spitter's SPIT (which carries this on its Projectile) read it; brutes/sentinels leave it 0.
  float poison_per_second = 0.0f;
  // If spit_range > 0 this creature is a RANGED attacker: creature_spit periodically launches a
  // Projectile (the same primitive the player's throw uses) at the nearest person within
  // spit_range, dealing spit_damage on impact. 0 = melee-only (the default). More "procs as data" —
  // a spitter is just these ranged knobs (plus poison_per_second above, so its spit envenoms), no
  // new creature class. spit_timer counts down to its next spit.
  float spit_range = 0.0f;
  float spit_damage = 0.0f;
  float spit_timer = 0.0f;
  // If > 0 this creature DRINKS: every landed attack HEALS it this much (capped at its max), so it
  // recovers as it feeds — the LEECH archetype, the only creature that heals ON A HIT (the
  // knitflesh heals PASSIVELY instead, a non-zero health regen). It REVERSES the wear-down: you
  // must burst it or deny its bites, not chip it. More "procs as data" — a knob, not a creature
  // class. 0 = doesn't drink (every other archetype — bit-identical). Placed LAST so existing
  // positional `Enemy{...}` initialisers keep their meaning.
  float lifesteal_per_hit = 0.0f;
  // If > 0 this creature DETONATES on death: when it's reaped (handle_deaths), every PERSON within
  // a blast radius takes this much damage — the BOMBER archetype, so you kill it at RANGE
  // (throw/bolt) or eat the blast finishing it in melee. The death-effect twin of the knitflesh's
  // passive regen and the leech's on-hit drink: "procs as data", an archetype knob, not a creature
  // class. 0 = no blast (every other archetype — bit-identical). Placed LAST so existing positional
  // `Enemy{...}` initialisers keep their meaning.
  float death_blast_damage = 0.0f;
};

// A lingering damage-over-time left by a venomous blow (resolve_creature_contacts). Unlike a hit,
// it keeps chipping `health` for `remaining` seconds AFTER the attacker has gone — the "you got
// away but the venom lingers" threat that makes the fast swarm scarier to disengage from.
// tick_poison ages it and reaps it at 0; the chip routes through the normal handle_deaths death
// path. Venom also SUPPRESSES healing while it lasts (regenerate_vitals skips a poisoned entity),
// so the chip lands in full instead of being cancelled by regen — that's what keeps the threat
// real. Resistance is the bigger HP POOL a tougher character carries (VIT grows max), not
// out-healing it.
struct Poisoned {
  float remaining = 0.0f;          // seconds of venom left
  float damage_per_second = 0.0f;  // health chipped each second while it lasts
};
inline constexpr float kPoisonDuration = 3.0f;  // how long a fresh envenoming lingers (a knob),
                                                // shared by the swarmer's bite and a venom weapon

// A cast BARRIER — the game's first TIMED BUFF, Poisoned's beneficial mirror (same {timer,
// magnitude} shape). While present, `absorb` is soaked off each incoming hit before it bites — a
// melee blow (resolve_creature_contacts) OR a ranged shot (advance_projectiles: a spitter's venom
// bolt or a thrown one) — a GENERAL damage buffer, the last line of defence on top of static
// VIT/armour. It stops DAMAGE, not CONTACT (a spit's venom still lands). Raised by
// shield_spell (the design's defensive "ward"-role spell, named Shield here so it doesn't collide
// with warded ARMOUR's thorns), aged and reaped by tick_shield. Absent = unshielded = every
// existing contact is bit-identical.
struct Shielded {
  float remaining = 0.0f;  // seconds of barrier left
  float absorb = 0.0f;     // damage soaked off each blow while it lasts
};

// A cast HASTE — the game's first MOVEMENT buff, the mobility twin of Shielded (same {timer,
// magnitude} shape). While present, `factor` multiplies the caster's MOVEMENT each tick:
// integrate_motion scales the position DELTA by it, in the very slot the mire drag already lives,
// so haste is literally "mire in reverse". It scales the DELTA, not the stored Velocity, so every
// velocity-reading system downstream (update_stamina, drain_hunger — both binary moving/still) sees
// the true heading; a hasted mover just covers more ground. Raised by haste_spell (the design's
// UTILITY-role spell — the first that neither harms a foe nor mends a friend nor roots you in
// place, it just speeds you, to close a gap or break a chase), aged and reaped by tick_haste.
// Absent = unhasted = every existing move is bit-identical; a defaulted factor of 1.0 is also a
// no-op.
struct Hasted {
  float remaining = 0.0f;  // seconds of quickening left
  float factor = 1.0f;     // movement multiplier while it lasts (1.0 = no change)
};

// Marks a character holding a GUARD (a raised block). While present, an incoming creature blow
// (resolve_creature_contacts) — AND a physical ranged shot, a thrown weapon or a spit
// (advance_projectiles) — is softened to kBlockDamageFactor of its damage via the shared
// guard_block_factor; a MAGIC bolt pierces the guard (Projectile::from_magic). But the guarded
// stance ROOTS you to kGuardMoveScale of your speed, so it's a real trade (plant-and-tank vs
// move-and-dodge), not free upside. Set/cleared each tick by the MovePlayer command's `guard` flag
// (the player) and by npc_guard (a hardened bulwark colonist rooting under threat), so BOTH the
// player and NPCs guard — the softening system reads Blocking on any entity.
struct Blocking {};
inline constexpr float kBlockDamageFactor =
    0.4f;  // fraction of a blow that gets through a raised guard
inline constexpr float kGuardMoveScale = 0.35f;  // how much a guard slows you — the block's cost

// The offensive twin of the guard stance: a SPRINT. Held from the MovePlayer command's `sprint`
// flag (a key), it BOOSTS move speed by kSprintMoveScale — a burst to close a gap or break a chase
// — but burns stamina FASTER (update_stamina adds kSprintDrainBonus while Sprinting + moving), so
// it's a short dash that ends in the exhaustion crawl, not a free faster pace. Guard takes
// precedence (you can't sprint with your guard up), and an exhausted player (0 stamina) can't
// sprint at all. Like Blocking it's an active, input-driven stance, so today only the player
// sprints. No Sprinting component / no sprint flag -> bit-identical.
struct Sprinting {};
inline constexpr float kSprintMoveScale = 1.6f;  // how much faster a sprint moves you...
inline constexpr float kSprintDrainBonus =
    40.0f;  // ...and the EXTRA stamina/sec it burns (2x total)

// The OFFENSIVE third of the held-stance trio (Sprinting = mobility, Blocking = defence,
// PowerAttack = offence — each trading the stamina bar for its edge). Set from the MovePlayer
// command's `power` flag (a held key), like Blocking from guard: while it's up, perform_attack
// lands a HARDER but dearer swing (kPowerDamage for kPowerStaminaCost). An input-driven stance, so
// today only the player powers up; no PowerAttack (every NPC, or a player not holding it) -> the
// base swing -> bit-identical. The cost/damage knobs live in perform_attack (systems.cpp) beside
// the melee ones.
struct PowerAttack {};

// How much an EXHAUSTED character (stamina drained to 0 by moving) is slowed — a crawl, not a stop,
// so the spent can always limp to safety while stamina recovers. Shared so the player (MovePlayer)
// and NPCs (steer_npcs) tire identically — the "the bane bites both" parity the codebase keeps.
inline constexpr float kExhaustedMoveScale = 0.4f;

// A projectile in flight — a thrown attack (perform_throw) made VISIBLE and given a travel time. It
// HOMES on its `target`: advance_projectiles steers it there each tick and, on arrival, applies the
// (already VIT-mitigated) `damage`, crediting `owner` with Valor on a killing hit. Homing (not
// straight-line) so it reliably catches an APPROACHING creature — a straight shot aimed at where
// the foe was would overshoot as it closes. It carries its own render dot, so the renderer draws it
// for free. If the target dies mid-flight the shot is wasted (despawned) — the one cost of the
// travel delay. This is the reusable seed of every ranged effect: the player's throw and the
// spitter's (venomous) spit ride it already; arrows and bolts would too.
struct Projectile {
  entt::entity target;   // the Enemy it homes on (despawns if this becomes invalid)
  entt::entity owner;    // who threw it — credited Valor on a killing hit
  float damage = 0.0f;   // pre-mitigated at launch (homing keeps the same target, so VIT is fixed)
  float speed = 600.0f;  // world units/second — faster than any creature, so it always converges
  // If > 0, a landed shot also ENVENOMS its target (applies Poisoned), the RANGED echo of a
  // swarmer's venomous bite — a venom spitter's spit carries this; the player's plain throw leaves
  // it 0. Placed LAST so the existing positional Projectile{target, owner, damage, speed} inits
  // keep meaning.
  float poison_per_second = 0.0f;
  // TRUE for a MAGIC projectile (magic_bolt), FALSE for a PHYSICAL one (a thrown weapon or a
  // creature's spit). A raised GUARD turns a physical shot (advance_projectiles reads this) but a
  // magic bolt PIERCES it — a bolt is warded by Wisdom (magic_defence_of), not stopped by a shield
  // or a guard: the design's "a bolt pierces plate". Placed LAST and defaulting FALSE so the
  // existing positional Projectile{...} inits (throw + spit) stay physical and unchanged; only
  // magic_bolt sets it true.
  bool from_magic = false;
};
inline constexpr float kProjectileHitRadius = 10.0f;  // how close counts as an impact
inline constexpr float kProjectileSpeed = 600.0f;     // a thrown shot's travel speed

// A collectible a slain creature leaves behind: walk over it (collect_pickups) to
// restore `heal` health AND permanently raise your max HP by `bonus_max_hp`, then it's
// consumed. The first loot — winning the fight patches you up now and hardens you a
// little for good, so kills both sustain and grow you. The seed of a fuller item
// system later. `lifetime` counts down so an ungrabbed orb fades rather than piling up
// forever (creatures die all over the field, many far from the player).
struct Pickup {
  float heal = 25.0f;
  float bonus_max_hp = 2.0f;  // permanent max-HP gain on collect (a small loot reward)
  float lifetime = 20.0f;     // seconds before an uncollected orb fades away
  float food = 50.0f;  // hunger it refills when eaten — a monster-drop orb's default. A MEAL
                       // (spawn_meal, harvested from a ripe crop) sets this higher: prepared
                       // food fills more than the loot you scavenge. Appended LAST so the
                       // orb default is unchanged (bit-identical) — collect_pickups reads it.
};

// A SPELLBOOK lying on the ground, waiting to be READ. The design's "magic is LEARNED" made real:
// walking over one teaches the reader the Spellcasting skill (study_spellbooks), so a caster EARNS
// the right to cast by finding a tome rather than being born with it. A marker only — the one spell
// it teaches (Spellcasting) is implicit while that's the sole spell; a `SkillId taught` field is
// the natural growth when a second spell exists. Consumed on a read that teaches something.
struct Spellbook {};

// A weapon lying on the ground, waiting to be WIELDED (the Equip command). The equip-
// counterpart of Pickup: not consumed for a one-off effect but WORN — its bonuses fold
// into an Equipped cache on the wearer until swapped. The first item that changes HOW you
// fight, and the first with a BANE: the design's non-negotiable "every item has a positive
// AND a negative trait, nothing rolls pure-upside" — here a heavier swing hits harder but
// slows you. The seed of the full P5 Equipment system (Item{def,quality,durability,traits},
// multi-slot Equipment); one hardcoded def for now (design: "defs hardcoded first").
// Full durability of a fresh weapon/armour — the value `equip` copies onto Equipped, and the cap
// the hearth mends worn gear back toward (`mend_gear`). Named once so the struct default and the
// repair cap are one source of truth and can never drift apart.
inline constexpr float kWeaponMaxDurability = 40.0f;  // connecting hits a blade survives
inline constexpr float kArmourMaxDurability = 30.0f;  // blows a plate absorbs (plate is frailer)

struct Weapon {
  int strength_bonus = 4;      // + effective Strength while wielded (longer reach + harder hits)
  float move_penalty = 0.25f;  // the BANE: fraction of move speed lost while wielding (heft)
  // If > 0 the blade is VENOMOUS: a landed hit also envenoms the foe (Poisoned), the player-side
  // mirror of a swarmer's bite. "Gear grants a +aspect" (P5). A venom blade trades raw Strength for
  // this lingering chip — a poison build. 0 = a plain blade (bit-identical for anyone not wielding
  // one).
  float venom_per_second = 0.0f;
  // How many connecting hits on a hostile the blade survives before it SHATTERS (the design's
  // "durability now, repair later"): each such swing wears it by 1, and at 0 the weapon slot clears
  // and the wielder is unarmed until it grabs another. It makes the item's tradeoff TEMPORAL — the
  // bane you pay for the buff is now "and it won't last forever". Placed last so existing
  // positional `Weapon{...}` initialisers keep their meaning.
  float durability = kWeaponMaxDurability;
  // QUALITY — the item-tier axis (the design's "found/crafted gear rolls a quality"). A multiplier
  // on the BOON only (strength_bonus at equip), so a finer blade hits harder while a crude one hits
  // softer; 1.0 = the baseline blade, and everything spawned today is 1.0 (so the whole pre-quality
  // world is bit-identical). Deliberately scales only the upside: the BANE (heft) stays full, and
  // shrinking the bane is the ORTHOGONAL mastery track (STR via carried_move_penalty). So quality
  // is "how good the item is", mastery is "how well you wield it" — two independent axes. Last for
  // positional-init safety. (Rolled/per-source quality is a follow-up; this PR is the seam.)
  float quality = 1.0f;
  // KEEN — the crit trait (the second named weapon trait; the design's "gear grants a +aspect"). If
  // > 0 the blade adds this much CRIT CHANCE on top of the wielder's Luck-driven crit
  // (perform_attack folds it in), so a keen blade lands the doubled blow more often — a distinct
  // proc from the venom blade, feeding a Luck/crit build. Bought with a notch of raw Strength
  // (spawn_keen_steel), so it is never pure-upside. 0 = a plain blade, bit-identical for anyone not
  // wielding a keen one. Last for positional-init safety.
  float crit_bonus = 0.0f;
  // LEECH — the vampiric trait (the third named weapon trait). If > 0 a landed hit on a hostile
  // DRINKS: the wielder heals this fraction of the damage it dealt (perform_attack), the
  // player-side mirror of the leech creature's on-bite heal and the on-HIT twin of kill-vigor's
  // on-KILL heal. It feeds a sustain build — you out-heal a war of attrition instead of finishing
  // fast. Bought with a notch of raw Strength (spawn_vampiric_weapon), so it is never pure-upside.
  // 0 = a plain blade, bit-identical for anyone not wielding a vampiric one. Last for
  // positional-init safety.
  float leech = 0.0f;
};

// A dropped piece of ARMOUR — the first defensive item, the offense/defence counterpart of
// Weapon. Worn in a SECOND slot (see Equipped), so a character can carry a weapon, armour, or
// both. Like every item it has a BANE, but a DISTINCT one from the weapon's move-heft: plate
// tires you, so it slows STAMINA recovery (a weaker second wind between fights).
struct Armour {
  float defence_bonus = 6.0f;  // + physical defence (softens blows via defence_of/mitigate)
  float stamina_regen_penalty = 0.30f;  // the BANE: fraction of stamina recovery lost while worn
  // How many creature blows the plate ABSORBS before it SHATTERS — the defensive twin of
  // Weapon::durability. Each blow it softens wears it by 1, and at 0 the armour slot clears (the
  // wearer is bare again). Fewer than a blade's life (30 vs 40): plate takes the brunt in a swarm.
  // Placed last so existing positional `Armour{...}` initialisers keep their meaning.
  float durability = kArmourMaxDurability;
  // QUALITY — the item-tier axis, the twin of Weapon::quality. A multiplier on the BOON only
  // (defence_bonus at equip): finer plate softens more, crude plate less. 1.0 = baseline (every
  // armour spawned today), so bit-identical. The BANE (stamina-regen) stays full — shrinking it is
  // the orthogonal VIT mastery (borne_regen_penalty). Last for positional-init safety.
  float quality = 1.0f;
  // WARDED — the thorns trait, armour's FIRST flavourful trait (the defensive twin of the weapon's
  // venom/keen). If > 0 the plate is SPIKED: every creature blow it absorbs reflects this flat chip
  // back onto the attacker (resolve_creature_contacts), routed through the creature's own Stats
  // like the guard's riposte, so a warded tank punishes a swarm for hitting it. Bought with a notch
  // of raw defence (spawn_warded_armour), so it is never pure-upside. 0 = a plain plate,
  // bit-identical for anyone not wearing a warded one. Last for positional-init safety.
  float thorns_per_hit = 0.0f;
  // EVASIVE — the DODGE trait, armour's SECOND flavourful trait (the twin of the weapon's two:
  // venom+keen). If > 0 the plate is LIGHT and nimble: it adds this flat dodge chance to the
  // wearer's hit-vs-Evasion contest in resolve_creature_contacts (where a worn plate meets a
  // creature's blow), so an evasive wearer slips more blows OUTRIGHT — the active-avoidance
  // counterpart to warded's passive chip-back. Bought with a bigger notch of raw defence than
  // warded (spawn_evasive_armour), so it is never pure-upside. 0 = a plain plate, bit-identical for
  // anyone not wearing an evasive one. Last for positional-init safety.
  float evasion_bonus = 0.0f;
};

// The cached bonuses of everything a character is wearing — the design's EquipMods, folded ONCE
// on equip (not recomputed per tick). Absent = bare. TWO conceptual SLOTS, kept as flat
// field-pairs (weapon: strength_bonus/move_penalty; armour: defence_bonus/stamina_regen_penalty)
// rather than a slots array — there are exactly two, so naming the fields is simpler than any
// Slot abstraction (add one only when a third slot with its own rules actually arrives). Each
// equip overwrites ONLY its own pair, so grabbing armour never disturbs a wielded weapon and
// vice-versa; a zero pair means that slot is empty.
struct Equipped {
  int strength_bonus = 0;
  float move_penalty = 0.0f;
  float defence_bonus = 0.0f;
  float stamina_regen_penalty = 0.0f;
  // A second WEAPON-slot field (logically pairs with strength_bonus/move_penalty), but placed LAST
  // so the existing positional `Equipped{...}` initialisers keep their meaning — it just
  // zero-fills.
  float weapon_venom = 0.0f;  // the wielded blade's venom_per_second (0 = plain or unarmed)
  // The wielded blade's REMAINING durability, copied from Weapon::durability on equip and worn down
  // by each connecting hit on a hostile (perform_attack). 0 = no weapon (unarmed, or the blade
  // shattered and cleared its slot). Placed last so existing positional `Equipped{...}`
  // initialisers keep their meaning — a hand-built Equipped with no durability set is treated as a
  // wear-free fixture (nothing to dull), which keeps the weapon-combat tests bit-identical.
  float weapon_durability = 0.0f;
  // The worn armour's REMAINING durability, copied from Armour::durability on equip and worn down
  // by each creature blow it absorbs (resolve_creature_contacts). 0 = no armour (bare, or the plate
  // shattered and cleared its slot). Last field — same positional-init and wear-free-fixture
  // guarantees as weapon_durability, so the armour-combat tests stay bit-identical.
  float armour_durability = 0.0f;
  // The wielded blade's KEEN crit bonus, copied from Weapon::crit_bonus on equip and added to the
  // Luck-driven crit chance in perform_attack. 0 = a plain blade (bit-identical). Last field, same
  // positional-init and zero-fill guarantee as the durability fields above.
  float crit_bonus = 0.0f;
  // The worn plate's WARDED thorns, copied from Armour::thorns_per_hit on equip; a creature blow
  // this plate absorbs reflects it back onto the attacker (resolve_creature_contacts). 0 = plain
  // plate or bare (bit-identical). Cleared when the armour slot clears (plate shatters). Last
  // field, same positional-init and zero-fill guarantee as the fields above.
  float armour_thorns = 0.0f;
  // The worn plate's EVASIVE dodge bonus, copied from Armour::evasion_bonus on equip and added to
  // the wearer's dodge in resolve_creature_contacts (where a plate meets a creature's blow). 0 =
  // plain plate or bare (bit-identical). Cleared when the armour slot clears (plate shatters). Last
  // field, same positional-init and zero-fill guarantee as the fields above.
  float armour_evasion = 0.0f;
  // The wielded blade's VAMPIRIC leech, copied from Weapon::leech on equip; a landed hit heals the
  // wielder this fraction of the damage dealt (perform_attack). 0 = a plain blade (bit-identical).
  // Cleared when the weapon slot clears (blade shatters). Last field, same positional-init and
  // zero-fill guarantee as the fields above.
  float weapon_leech = 0.0f;
};

// --- Stats system ---
//
// The foundation for player and NPC stats. Deliberately small: a reusable Vital
// building block and a Stats "character sheet" that holds them. This is where
// the game's stats and skills grow (see docs/engine/skeleton/extending.md), but
// only as they're actually needed — no speculative RPG scaffolding.

// A depleting, regenerating resource with a cap. Health today; stamina, hunger,
// and mana will be the same shape, so they share this one type. Floats (not
// ints) so recovery can accrue smoothly across the fixed 60 Hz ticks.
struct Vital {
  float current = 100.0f;
  float max = 100.0f;
  float regen_per_second = 0.0f;  // 0 = doesn't recover on its own
  // The un-bonused pool size. `max` is recomputed each tick as base + attribute
  // bonuses (see advance_progression), so an entity that should be tankier just
  // spawns with a bigger base — no hardcoded constant to trip over. Defaults to
  // 100 so every Vital{current, max, regen} literal keeps today's numbers.
  float base = 100.0f;
};

// An entity's stat sheet — one component holding everything about its condition,
// so a player or an NPC-management screen reads a single place. Give an entity a
// Stats and it participates in the stats systems; leave it off and it doesn't.
//
// To grow the system you add fields HERE — another Vital, later a set of
// attributes or skills — and teach a system to read them. `stamina` was added
// exactly that way: one field here plus a small update_stamina system (it's spent
// by moving, not passively regained, so it earns its own system rather than a
// line in regenerate_vitals). Nothing else in the engine had to change.
struct Stats {
  Vital health{100.0f, 100.0f, 5.0f};    // full, and slowly self-heals
  Vital stamina{100.0f, 100.0f, 20.0f};  // spent by moving; recovers when resting
  // A survival Need: unlike the others it only ever falls (regen 0) — you refill it by
  // EATING, not by resting (drain_hunger + eating from loot orbs). Empty it and you start
  // to starve (it chips health, killing you through the normal death path). The first of
  // Food/Water/Fatigue; the same Vital shape, the "you must feed the colony" pressure.
  Vital hunger{100.0f, 100.0f, 0.0f};  // falls over time; 0 = starving
  // A SECOND survival Need, Water — the same falling Vital as hunger, but refilled at a fixed
  // WaterSource (the drink system) rather than by eating orbs, so it drives the design's "walk to
  // the well" spatial loop. Empty it and you DEHYDRATE: it chips health through the same death path
  // as starving. Falls independently of hunger, so a colonist can be watered yet starving, or vice
  // versa.
  Vital water{100.0f, 100.0f, 0.0f};  // falls over time; 0 = dehydrating
  // The THIRD survival Need, Fatigue — and the odd one out: unlike hunger/water (which only ever
  // fall, refilled by eating/drinking), fatigue RECOVERS with REST. It falls as you EXERT (walk,
  // sprint — the same tiers the other needs use) and rises when you stand still, so it's the "you
  // can't run forever" pressure that the sprint you already pay stamina for now also costs in the
  // long run. `tick_fatigue` owns it (regen 0 here so the generic regenerate_vitals leaves it
  // alone). 0 = exhausted; the COLLAPSE-at-empty consequence and the sit/sleep faster-rest tiers
  // are the next slices — this field + drain/recover is the seam. Appended LAST so every positional
  // Stats{Vital
  // ...} init (health only) default-fills it, bit-identical.
  Vital fatigue{100.0f, 100.0f, 0.0f};  // falls while exerting, rises at rest; 0 = exhausted
  // MANA — the magic resource, the design's third bar beside health and stamina, but NOT a survival
  // Need: it is SPENT (casting a spell) and steadily REGENERATES (regenerate_vitals), the shape
  // stamina uses. Everyone carries a trickle of it, but it's INERT until you've LEARNED a spell
  // (magic_bolt gates on the Spellcasting skill, which the untrained don't have) — the design's
  // "magic is learned, not innate". Appended LAST so every positional Stats{Vital ...} init
  // default-fills it, and since nothing reads mp unless you can cast, a world with no caster is
  // bit-identical.
  Vital mp{100.0f, 100.0f, 10.0f};  // spent by casting, regenerates at rest; the magic bar
  // WARMTH — a LOCALIZED survival Need (the design's temperature, split out spatially). Unlike
  // hunger/water it doesn't fall on a background timer: it holds steady in the open, DRAINS only
  // inside a ColdZone, and REFILLS by a Hearth's fire (drain_warmth) — so a cold snap is a "huddle
  // by the fire" pressure, not a clock. At 0 it FREEZES (chips health, the starving/dehydrating
  // death path). Appended LAST so every positional Stats{...} init default-fills it, and since it
  // only moves inside a ColdZone (none in a fresh world), a world without cold is bit-identical.
  Vital warmth{100.0f, 100.0f, 0.0f};  // holds in the open, drains in cold, refills at a fire
};

// How hard a character can fight given how FED, WATERED, WARM, and RESTED it is — the design's "an
// empty Need is an escalating inefficiency debuff", made concrete on the one number a swing, throw,
// bolt, and stride already scale: raw damage and move speed. Efficiency stays 1.0 while ALL FOUR
// needs sit above kNeedPenaltyBelow of their max (the common case — and every full-fed combat test
// — so combat is bit-identical there), then ramps down LINEARLY to kNeedFloor as the WORST
// (most-depleted) of hunger/water/warmth/FATIGUE falls to empty. The floor mirrors mitigate's 10%
// chip: a starving, freezing, OR exhausted fighter is weakened, never toothless. Reads the BINDING
// need (whichever is worse), so topping off only one doesn't lift the debuff — you must keep the
// colony fed, watered, warm AND rested to keep it at full strength. Fatigue is the LATEST need to
// join: exhaustion used to jump straight from full strength to the lethal collapse (handle_deaths
// at 0 fatigue) with no graded bite, so this is the missing "escalating debuff" before the fall — a
// bone-tired colonist swings and moves weaker, its FIRST graded consequence short of dropping.
// Fatigue holds full at rest and only falls while exerting, so a rested colony is bit-identical
// (like warmth, which only drains in a ColdZone). Pure (no RNG, no sim state beyond the sheet), so
// combat stays deterministic and this is unit-testable like standing.
inline float need_efficiency(const Stats& s) {
  constexpr float kNeedPenaltyBelow = 0.25f;  // penalty bites only in the last quarter of a need
  constexpr float kNeedFloor = 0.5f;          // bone-empty still swings at half strength, never 0
  const float hunger_frac = s.hunger.max > 0.0f ? s.hunger.current / s.hunger.max : 1.0f;
  const float water_frac = s.water.max > 0.0f ? s.water.current / s.water.max : 1.0f;
  const float warmth_frac = s.warmth.max > 0.0f ? s.warmth.current / s.warmth.max : 1.0f;
  const float fatigue_frac = s.fatigue.max > 0.0f ? s.fatigue.current / s.fatigue.max : 1.0f;
  float worst = hunger_frac < water_frac ? hunger_frac : water_frac;
  if (warmth_frac < worst)
    worst = warmth_frac;  // cold is a Need too: a chilled colonist is weakened
  if (fatigue_frac < worst)
    worst = fatigue_frac;  // and EXHAUSTION: a bone-tired colonist is weakened before it collapses
  if (worst >= kNeedPenaltyBelow)
    return 1.0f;  // comfortable — bit-identical to the pre-debuff world
  return kNeedFloor + (1.0f - kNeedFloor) * (worst / kNeedPenaltyBelow);  // linear kNeedFloor..1.0
}

// How WORN-DOWN a character LOOKS (starved, parched, freezing, OR bone-tired), in [0, 1] — a
// renderer-only cue that EXACTLY tracks the combat debuff, so a colonist that fights weaker also
// visibly wastes. It follows whichever need is binding (need_efficiency reads the worst), so a
// chilled or exhausted colonist wastes too, from the same source of truth. Derived straight from
// need_efficiency (the one source of truth, so the look and the penalty can never drift apart):
// need_efficiency runs [kNeedFloor .. 1.0], so 2*(1 - eff) maps that to [0 .. 1] — 0 while a
// colonist is fed (no pallor, an unchanged draw) up to 1 at empty. The renderer mixes the dot
// toward a sallow grey by this much; the sim never reads it (presentation only, like
// wounded_brightness).
inline float need_pallor(const Stats& s) {
  return 2.0f * (1.0f - need_efficiency(s));  // eff in [0.5, 1.0] -> pallor in [0, 1]
}

// The four survival needs, so a query can NAME which one is dragging a character down.
enum class NeedKind { Hunger, Water, Warmth, Fatigue };

// Which need is BINDING — the most-depleted of the four, EXACTLY the one need_efficiency reads as
// "worst" and need_pallor greys the dot for. So the HUD can NAME the failing need (STARVING /
// PARCHED / FREEZING / EXHAUSTED) rather than only greying it — the WORD twin of the pallor's
// COLOUR, both off the same binding need, so they can never disagree. A pure query over the sheet
// (no RNG, the sim never reads it), unit-testable like need_efficiency. Ties break in the fixed
// scan order hunger < water < warmth < fatigue via strict `<` — the same order (and the same
// 0-max-reads-full guard) as need_efficiency's worst scan, so the named need is always the one
// actually binding. NOTE: this RE-IMPLEMENTS that four-frac scan rather than sharing it
// (need_efficiency throws away the identity, keeping only the min VALUE) — so if its need set or
// frac formula ever changes, update this in LOCKSTEP or the label and the pallor will drift apart.
inline NeedKind binding_need(const Stats& s) {
  const float hunger_frac = s.hunger.max > 0.0f ? s.hunger.current / s.hunger.max : 1.0f;
  const float water_frac = s.water.max > 0.0f ? s.water.current / s.water.max : 1.0f;
  const float warmth_frac = s.warmth.max > 0.0f ? s.warmth.current / s.warmth.max : 1.0f;
  const float fatigue_frac = s.fatigue.max > 0.0f ? s.fatigue.current / s.fatigue.max : 1.0f;
  NeedKind worst = NeedKind::Hunger;
  float worst_frac = hunger_frac;
  if (water_frac < worst_frac) {
    worst_frac = water_frac;
    worst = NeedKind::Water;
  }
  if (warmth_frac < worst_frac) {
    worst_frac = warmth_frac;
    worst = NeedKind::Warmth;
  }
  if (fatigue_frac < worst_frac) {
    worst_frac = fatigue_frac;
    worst = NeedKind::Fatigue;
  }
  return worst;
}

// The screaming-caps label for a binding need — the word the HUD shows when a colonist is failing.
inline const char* need_label(NeedKind kind) {
  switch (kind) {
    case NeedKind::Hunger:
      return "STARVING";
    case NeedKind::Water:
      return "PARCHED";
    case NeedKind::Warmth:
      return "FREEZING";
    case NeedKind::Fatigue:
      return "EXHAUSTED";
  }
  return "";  // unreachable — the switch is exhaustive (a -Wswitch guard for a new NeedKind)
}

// A fixed drinking spot — a pond or well the `drink` system tops nearby thirsty characters up from,
// WITHOUT being consumed (unlike a one-shot food orb). `radius` is how close you must be to drink.
// The seed of the design's water economy (wells now, irrigated crops later); a thirsty NPC walks to
// the nearest one (steer_npcs). Drawn like any entity if given a RenderDot — the sim never reads
// it.
struct WaterSource {
  float radius = 60.0f;
};

// A fixed HEARTH — a warm, safe spot that speeds the HEALTH regen of anyone resting within `radius`
// (regenerate_vitals multiplies their regen while in range). The design's base-building recovery
// seed: a reason to hold/return to a place, and a real positioning trade (mend at base vs act in
// the field). Like the pond it's scenery the sim reads only for its effect; drawn if given a
// RenderDot.
struct Hearth {
  float radius = 0.0f;
};

// A COLD ZONE — a patch of the world where the cold bites: a person standing within `radius` of it
// loses WARMTH each tick (drain_warmth), the spatial half of the design's temperature Need. The
// inverse of a Hearth (which re-warms), so the two make the "flee the cold, huddle by the fire"
// loop. Scenery (Transform + a pale render disc + this); no Stats/Velocity. None in a fresh scene,
// so warmth never moves and the world is bit-identical until one is placed.
struct ColdZone {
  float radius = 0.0f;
};

// A MIRE — a boggy patch (mud, quicksand) that DRAGS on anyone crossing it. Where the ColdZone
// drains a Need, this touches MOVEMENT: while an entity stands inside the radius,
// `integrate_motion` scales its MOVEMENT that tick by `slow_factor` (< 1) — the position delta, not
// the stored velocity — so people, creatures, AND ambient motes bog down alike. Neutral terrain
// that reshapes a chase or a flight (lead a charging brute through the mud to gain ground) without
// blocking passage: because the drag is on the delta, an un-re-driven mover crawls THROUGH rather
// than compounding to a frozen stop. Scenery (Transform + a murky render disc + this); no
// Stats/Velocity. Default-absent: with none placed the factor is 1.0, so a world without a mire
// moves exactly as before — bit-identical.
struct MireZone {
  float radius = 0.0f;
  float slow_factor = 0.4f;  // movement multiplier inside — 0.4 = crawl to ~40% speed. A knob.
};

// A HAZARD FIELD — persistent damaging terrain (brambles, a scald patch, a thorn bed): anyone
// standing within `radius` loses HEALTH each tick (tick_hazard_fields), the direct-HP twin of a
// ColdZone's warmth drain. Where the ColdZone touches a Need and the MireZone touches movement,
// this bites HP outright — and it bites EVERYONE inside, people AND creatures (the tick excludes
// only the Downed, not Enemy), so you can KITE a brute across a thorn bed to wear it down: terrain
// as a weapon, not just an obstacle. 0 HP routes through the normal death path (handle_deaths).
// Scenery (Transform + a render disc + this); no Stats/Velocity. Distinct from the transient
// `Hazard` mote above (a consumed-on-contact pickup marker) — this PERSISTS. None in a fresh scene,
// so the view is empty and the world is bit-identical until one is placed.
struct HazardField {
  float radius = 0.0f;
  float damage_per_second = 8.0f;  // HP chipped per second while inside — a felt bite. A knob.
};

// A fixed food plot — a berry patch / garden a hungry character GRAZES to refill hunger (the
// `graze` system). Unlike the water pond it is FINITE: `stock` falls as colonists eat and REGROWS
// over time toward `max_stock`, so a picked-over patch must recover — the seed of the design's food
// PRODUCTION chain (crops, farming), distinct from both the one-shot loot orb and the ambient,
// infinite pond. It fixes the "starving in a quiet corner with no orbs" gap: a colonist can always
// walk to a plot.
struct FoodSource {
  float stock = 100.0f;            // food available to eat right now
  float max_stock = 100.0f;        // regrows up to this
  float regrow_per_second = 2.0f;  // a slow renewal — a plot feeds a few, then needs time
  float radius = 60.0f;            // how close you must be to graze
};

// The plot stock a single HARVEST spends to prepare one meal — and, equivalently, the RIPENESS bar
// a plot must clear to be worth harvesting (no half-meals from a barely-grown patch). Shared so the
// two sites that care agree on "ripe": harvest_nearest_crop reaps only at/above it, and the
// Provider steer rung heads a farming NPC ONLY toward a plot this ripe (a nearer under-ripe patch
// it couldn't actually work must not lure it away from one it can). One threshold, so the seek and
// the reap can never drift apart. Grazing (the forage rung) is unaffected — you can nibble any
// stock bite-by-bite.
inline constexpr float kHarvestCost = 60.0f;

// What a colonist DREAMS of — the design's "Aspirations (hopes/dreams) steer behaviour". Every
// steer rung wired so far is REACTIVE (flee a threat, forage when hungry, gather when idle); this
// is the first PROACTIVE one: an idle, safe colonist that carries an Aspiration goes and PURSUES
// it instead of merely ambling to the fire. It is a soft drive, not a compulsion — it sits LOW in
// the steer ladder, so any real want (fear, a wounded friend, hunger, thirst, cold) is tended
// first; only a content colonist chases its dream. New kinds append LAST (the steer rung switches
// on the kind, so -Wswitch catches any new value that forgets a rung). The component is
// DEFAULT-ABSENT: no entity carries it unless explicitly given one, so the pursuit rung is a no-op
// for everyone else and the pre-Aspiration world is bit-identical.
enum class AspirationKind : std::uint8_t {
  Warrior,   // dreams of battle — an idle, hale one seeks the nearest creature to fight
  Provider,  // dreams of plenty — an idle one works the land, harvesting a ripe plot into a meal
  Scholar,   // dreams of magic — an unlearned one seeks a Spellbook to LEARN Spellcasting, then
             // casts
  Healer,    // dreams of mercy — a compassionate one seeks the nearest HURT ally (wounded or
             // poisoned) to tend, so npc_heal / npc_cleanse can reach it. Appended LAST so existing
             // Aspiration{...} values hold.
};
struct Aspiration {
  AspirationKind kind = AspirationKind::Warrior;
};

// --- Progression: skills feed attributes ---
//
// The game grows characters the "learn by doing" way, and in three layers:
//
//   activity  ->  a SKILL levels up  ->  an ATTRIBUTE rises  ->  a derived stat grows
//
// A Skill improves with the activity that uses it; skills roll up into broad
// Attributes; attributes shape the Vitals you feel in play. The player and NPCs
// share this exact machinery — anyone carrying these components levels the same
// way, which is the whole point of doing it in the ECS.

// One trainable skill. XP accrues from doing the related activity and crossing a
// threshold raises the level. `xp` is a Fixed so it can gain a *fraction* each
// 60 Hz tick (20 XP/sec is 0.333/tick) and still be deterministic — an int would
// round every tick's gain to zero.
struct Skill {
  int level = 1;
  Fixed xp{};
};

// Identifies a skill. An enum (not a string) so lookups are cheap and the wire/
// save formats stay small. New skills append here.
enum class SkillId : std::uint16_t {
  Conditioning,  // trained by moving; main attribute Endurance
  Toughness,     // trained by taking damage; main attribute Endurance (a VIT skill)
  Striking,      // trained by attacking; main attribute Strength
  Recovery,      // trained by resting to recover spent stamina; main attribute Endurance
  Evasion,       // trained by facing a creature's swing; main attribute Dexterity (dodging)
  Scavenging,    // trained by collecting loot; main attribute Luck (fortune -> crit)
  Throwing,      // trained by landing a ranged throw; main attribute Dexterity (aim), a little STR
  Foraging,      // trained by grazing a food plot; main attribute Wisdom (the first WIS skill)
  Leadership,    // trained by public HEROISM with allies watching (a kill OR a rescue); main attr
                 // Charisma
  Guarding,      // trained by turning a blow with a raised guard; main attribute Endurance (a VIT
                 // skill)
  Resistance,    // trained by ENDURING venom (tick_poison); main attribute Endurance (a VIT skill)
  Athletics,     // trained by SPRINTING (a burst of speed); main attribute Dexterity (agility)
  Survivalist,   // trained by pushing into EXHAUSTION (low fatigue); main attribute Endurance. Its
                 // level LENGTHENS every TIMER need (fatigue, hunger, AND water) via the shared
                 // survivalist_relief that slows each drain: the design's "growth lengthens but
                 // never removes the timer", the ONE need-buffer (VIT/Endurance stays pure combat
                 // defence; warmth is spatial, eased by the fire not the skill). Added LAST so
                 // existing SkillId values keep their numbers.
  Spellcasting,  // the MAGIC gate: an entity that HAS this skill has LEARNED to cast (magic_bolt
                 // checks for it), so magic is learned-not-innate. Main attribute Intellect (the
                 // design's magic domain); trained by casting, and its main-attr feeds the INT that
                 // scales a bolt. Added LAST so existing SkillId values keep their numbers.
  Teaching,  // trained by MENTORING — passing a skill you've mastered to a nearby novice (teach).
             // Main attribute Charisma (the design's CHA "Teaching" skill, its 2nd feeder beside
             // Leadership): leading by instruction. Added LAST so existing SkillId values hold.
  Healing,   // trained by MENDING a wounded ally (heal_spell). Main attribute WISDOM: the design's
             // WIS Healing/Medicine domain, the support twin of Spellcasting's offence (WIS scales
             // the mend as INT scales a bolt). Added LAST so existing SkillId values hold.
  Cooking,   // trained by PREPARING a meal (harvest_nearest_crop). Main attribute INTELLECT (the
             // design's INT Cooking skill, Spellcasting's non-magic sibling): a better cook's meal
             // fills more hunger, so INT scales the meal the way it scales a bolt. Added LAST so
             // existing SkillId values hold.
  Attunement,  // trained by CASTING (spending mana, every spell). Main attribute ENDURANCE — the
               // design's VIT MP resource-skill, the mana twin of Recovery for stamina. Its
               // main-attr feeds the Endurance that GROWS the mana pool (mp.max) AND its base
               // regen, so a pure caster's mana finally DEEPENS by casting (not only by
               // moving/fighting); and its OWN level speeds mana regen directly
               // (kAttunementPerLevel in regenerate_vitals, mirroring Recovery's second wind).
               // Added LAST so existing SkillId values hold.
};

// The skills an entity is training — a KEYED collection, so a character can hold a
// whole branching tree of skills rather than one struct field each. A small
// append-ordered vector: deterministic iteration (a hash map's order is not) and
// no per-entity heap churn until a character actually learns many. Everyone starts
// knowing Conditioning (trained by staying active).
struct Skills {
  std::vector<std::pair<SkillId, Skill>> owned{{SkillId::Conditioning, Skill{}}};

  // Find a learned skill, or nullptr.
  const Skill* find(SkillId id) const {
    for (const auto& [key, skill] : owned) {
      if (key == id) return &skill;
    }
    return nullptr;
  }
  // Get a skill to train, learning it at level 1 if it's new.
  Skill& train(SkillId id) {
    for (auto& [key, skill] : owned) {
      if (key == id) return skill;
    }
    owned.emplace_back(id, Skill{});
    return owned.back().second;
  }
};

// A broad character attribute — now with its OWN level + XP (revised). The skills
// that use it feed it: a skill grants its MAIN attribute a lot of XP and each
// contributor a little, so an attribute levels in parallel with the skills that
// train it, rather than being recomputed from one skill. Endurance is the first;
// its level shapes the health & stamina pools (see advance_progression).
struct Attribute {
  int level = 1;
  Fixed xp{};
};

struct Attributes {
  Attribute endurance;  // fed by every Endurance-main VIT skill (Conditioning, Toughness, Recovery,
                        // Guarding, Resistance, Survivalist, Attunement); each level past 1 grows
                        // the pools
  Attribute strength;   // fed by Striking; each level past 1 lengthens attack reach + damage AND
                        // eases a wielded weapon's heft (carry, carried_move_penalty below)
  Attribute dexterity;  // fed by Evasion + Striking; each level past 1 raises the dodge chance
  Attribute
      luck;  // fed by Scavenging; each level past 1 raises crit chance AND how much a
             // collected orb heals (collect_pickups) — fortune sharpens the blade and the find
  Attribute wisdom;    // fed by Foraging; each level past 1 raises how much a food plot yields. The
                       // first of the design's NON-combat attributes (nature/foraging), so it does
                       // not feed the pools or a fighter build — it grows the survival economy.
  Attribute charisma;  // fed by Leadership; the design's SOCIAL attribute. Each level past 1
                       // deepens the CAMARADERIE a witness feels toward this character per shared
                       // victory (bond_witnesses) — a charismatic champion inspires more devotion.
                       // The second non-combat attribute (social), so like Wisdom it grows neither
                       // the pools nor a fighter build — it grows the colony's bonds instead.
  Attribute
      intellect;  // fed by Spellcasting; the design's MAGIC attribute (the 7th, completing the
                  // set). Each level past 1 sharpens a magic bolt's damage (magic_bolt) — the
                  // arcane mirror of STR on a swing / DEX on a throw. Appended LAST so the
                  // default-constructed Attributes every entity gets is unchanged; nothing
                  // reads it unless you can cast, so a world of non-mages is bit-identical.
};

// The design's gear-mastery pillar as ONE curve: "mastery shrinks a bane by about half but NEVER
// removes it." Given a gear BANE (a fraction of some stat lost while the item is worn) and the
// level of the attribute that MASTERS that gear, return the eased penalty — each level past the
// first relieves kBaneReliefPerLevel of it, capped at kBaneReliefCap (half) so a worn item ALWAYS
// costs something (the tradeoff survives to endgame). Mastery level 1 (the spawn default, every
// un-mastered fixture) yields relief 0 — the FULL bane — so the pre-mastery world is bit-identical.
// Shared by the weapon-heft (STR) and armour-stamina (VIT) gear easers below AND the mire-wade
// (DEX) terrain easer, so every "mastery shrinks a penalty, never removes it" case bends by the
// SAME rule. Manual clamp, no <algorithm> pulled in for one call (matching build_title above).
inline float eased_bane(float base_penalty, int mastery_level) {
  constexpr float kBaneReliefPerLevel = 0.05f;  // each mastery level past 1 eases the bane 5%...
  constexpr float kBaneReliefCap = 0.5f;        // ...up to half; a worn item always costs something
  float relief = static_cast<float>(mastery_level - 1) * kBaneReliefPerLevel;
  if (relief > kBaneReliefCap) relief = kBaneReliefCap;
  return base_penalty * (1.0f - relief);
}

// STR = "carry": a strong wielder shrugs off part of a weapon's move-heft (eased_bane by Strength)
// — the third Strength effect beside reach and damage. Returns the EFFECTIVE move penalty that BOTH
// the player (world.cpp MovePlayer) and NPCs (steer_npcs) fold into move speed, so the relief is
// parity-shared through one function. A null Attributes or Strength level 1 -> the full heft.
inline float carried_move_penalty(float base_penalty, const Attributes* attrs) {
  return attrs != nullptr ? eased_bane(base_penalty, attrs->strength.level) : base_penalty;
}

// VIT = "hardiness": a hardy body BEARS armour better, so its stamina-regen bane bites less
// (eased_bane by Endurance) — the armour twin of STR's weapon carry, closing the gap where the
// weapon heft could be mastered but the armour bane could not. Endurance ALSO speeds base stamina
// recovery (update_stamina), so a hardy character is resilient two coherent ways: quicker second
// wind AND less slowed by the plate it wears. A null Attributes or Endurance level 1 -> the full
// bane, so the pre-mastery world is bit-identical. Read where update_stamina folds the armour
// penalty into the recovery boost.
inline float borne_regen_penalty(float base_penalty, const Attributes* attrs) {
  return attrs != nullptr ? eased_bane(base_penalty, attrs->endurance.level) : base_penalty;
}

// VIT = "Cost": a hardier body spends LESS stamina per ACTION — the design's action-aspect family
// (Yield / Speed / Crit / Quality / COST ...), the one aspect with no code yet. A swing's or
// throw's stamina cost is eased by Endurance (VIT), the SAME eased_bane half-floor the STR/VIT gear
// masteries use, so a cost still bites (never free) but a seasoned body sustains a longer fight —
// the raw VIT attribute's own combat-sustain aspect. Endurance 1 (or no Attributes) ->
// eased_bane(base, 1) = base
// -> the FULL cost -> bit-identical. Distinct from the Survivalist SKILL's need-buffer (that eases
// survival Need DRAINS); this eases an action's stamina SPEND.
inline float eased_cost(float base_cost, const Attributes* attrs) {
  return attrs != nullptr ? eased_bane(base_cost, attrs->endurance.level) : base_cost;
}

// INT = "the arcane Cost": a cleverer caster spends LESS mana per spell — the magic mirror of VIT's
// eased_cost for stamina, easing the flat mana cost by the SAME eased_bane half-floor the STR/VIT
// gear masteries and the stamina spend use. A spell still costs mana (never free — the half-floor
// guarantees it), but a sharp mind casts more before the bar runs dry: INTELLECT's second effect
// beside sharpening a bolt, the caster-economy fantasy. Intellect 1 (or no Attributes) ->
// eased_bane(base, 1) = base -> the FULL cost -> every non-INT-trained caster (every existing mana
// test) is bit-identical. Distinct from Attunement, which grows/regens the MP POOL; this eases the
// SPEND — the arcane twin of eased_cost's stamina spend.
inline float eased_mana_cost(float base_cost, const Attributes* attrs) {
  return attrs != nullptr ? eased_bane(base_cost, attrs->intellect.level) : base_cost;
}

// DEX = "agility": a nimble mover WADES a mire's mud better — the movement twin of STR's weapon
// carry and VIT's armour-bear, closing the gap where the mire was the ONE movement modifier that
// read no attribute (weapon-heft and need-efficiency already shape move speed). Given a mire's
// `slow_factor` (< 1 means drag; 1.0 is firm ground), returns the EFFECTIVE factor after Dexterity
// eases it: the DRAG itself (1 - slow_factor) shrinks by `eased_bane`, so a nimble mover keeps more
// of its speed — but eased_bane caps the relief at half, so mud ALWAYS slows (agility is not
// immunity, the tradeoff the parity comment cares about survives). Firm ground (>= 1), a null
// Attributes (a mote, a projectile), OR Dexterity level 1 (every un-trained mover, every existing
// mire test) -> the slow_factor is returned VERBATIM via an early return, so the pre-agility world
// is EXACTLY bit-identical -- not merely ~1-ulp-close. That matters: the round-trip `1 - (1 - x)`
// is NOT the float identity for x < 0.5 (it double-rounds), so a DEX-1 mover routed through the
// eased path would drift a ulp from the old `mire_factor` value and desync the lockstep/replay
// gate; the dex <= 1 short-circuit keeps it literally equal. Read in integrate_motion, where the
// EASED wade is applied to self-driven movement — a passive shove (perform_attack's knockback)
// takes the raw mire_factor instead, no wade.
inline float waded_mire_factor(float slow_factor, const Attributes* attrs) {
  if (slow_factor >= 1.0f || attrs == nullptr) return slow_factor;  // firm ground, or no agility
  const int dex = attrs->dexterity.level;
  if (dex <= 1) return slow_factor;  // untrained -> the raw drag VERBATIM (no 1-(1-x) round-trip)
  const float drag = 1.0f - slow_factor;  // how much speed the mud steals
  return 1.0f - eased_bane(drag, dex);    // agility keeps some of it back
}

// A BUILD-derived title — the "from build" half of the derived recognition the `standing_title`
// comment promised. Which of the four COMBAT Attributes dominates names what KIND of fighter a
// character has become: STR a Warrior, DEX a Skirmisher, VIT a Bulwark, LCK a Chancer. Wisdom (the
// non-combat gathering attribute) is deliberately NOT considered — a forager isn't a fighter build,
// so a pure grazer stays a Greenhorn until it trains a combat stat; a "Naturalist"-style WIS title
// is a later add. A pure query over Attributes (never a stored slot), so it always matches the
// levels behind it; the HUD shows
// it beside standing_title and the sim never reads it. All four still at the starting level 1 = no
// build has emerged ("Greenhorn"). Ties break in a fixed order (strength, dexterity, endurance,
// luck) so the result is deterministic. Ready to grow — mastery bands, gear, a per-skill "Master
// Smith" — the same way standing_title will.
inline const char* build_title(const Attributes& attrs) {
  const int str = attrs.strength.level;
  const int dex = attrs.dexterity.level;
  const int vit = attrs.endurance.level;
  const int lck = attrs.luck.level;
  int top = str;  // manual max (no <algorithm> pulled into this header for one call)
  if (dex > top) top = dex;
  if (vit > top) top = vit;
  if (lck > top) top = lck;
  if (top <= 1) return "Greenhorn";     // untrained — no build has emerged yet
  if (str == top) return "Warrior";     // STR: power and reach
  if (dex == top) return "Skirmisher";  // DEX: speed, dodge, aim
  if (vit == top) return "Bulwark";     // VIT: hardiness and defence
  return "Chancer";                     // LCK: fortune and crits
}

// Names an attribute so a data-driven `SkillDef` can say which attribute(s) a skill feeds
// (a skill's XP flows to its MAIN attribute a lot, and to each CONTRIBUTOR a little). An
// enum, not a member pointer, keeps the defs plain data — the shape mods will add rows to.
// New attributes append here (and get a case in `attr_ref`, guarded by -Wswitch).
enum class AttrId : std::uint8_t {
  Endurance,
  Strength,
  Dexterity,
  Luck,
  Wisdom,
  Charisma,
  Intellect  // the magic attribute (the 7th); appended LAST so existing AttrId values keep numbers
};

// A single global "how experienced overall" level, fed by a fraction of ALL
// activity (not one skill). Its level is a gentle multiplier — via the same POWER
// curve — on the EARNED portion of stats: the earned HP/stamina pools
// (advance_progression) and the earned Strength delta on combat damage
// (perform_attack). So a long-lived character is genuinely a bit better across the
// board — the "veteran" layer on top of specific skills and attributes. Starts at
// level 1 = POWER(0) = no head start, exactly like the other {level, xp} pairs.
struct CharacterLevel {
  int level = 1;
  Fixed xp{};
};

// An EXPERIENCE-derived title — the fourth axis of the derived recognition, orthogonal to its three
// siblings: standing_title says how GOOD or BAD you are (the deed ledger), build_title what KIND of
// fighter (your dominant trained attribute), deed_epithet what you're KNOWN FOR (a single ledger
// dim), and this says how much you've LIVED — your Character Level, the slow "veteran" layer that
// grows from a fraction of ALL activity. A pure query over CharacterLevel (never a stored slot), so
// it always matches the level behind it; the HUD shows it beside the character-level number and the
// sim never reads it. The bands are chosen so a fresh colonist (level 1) is a Novice and a
// long-lived one earns its stripes; deterministic (a plain level comparison). Ready to grow — finer
// bands, a per-domain "Elder Smith" — the same way the sibling titles will.
inline const char* veteran_title(const CharacterLevel& character) {
  const int lvl = character.level;
  if (lvl < 3) return "Novice";    // levels 1-2 — barely blooded
  if (lvl < 6) return "Seasoned";  // 3-5 — has some rounds behind it
  if (lvl < 10) return "Veteran";  // 6-9 — a proven survivor
  return "Grizzled";               // 10+ — lived through it all
}

// An ARCANE-RANK title — how accomplished a CASTER is, banded from the Spellcasting skill's level
// (which grows by casting). The magic twin of veteran_title (character level) and build_title
// (which fighter): a warrior's progression reads on the HUD, so a mage's should too. Returns
// nullptr for anyone who never LEARNED to cast (no Spellcasting skill) — magic is
// learned-not-innate, so a non-caster has no arcane rank and the HUD simply omits the line (like
// deed_epithet's nullptr). A pure query over the Skills sheet (never a stored slot); the HUD shows
// it, the sim never reads it, so it can't touch replay. A freshly-learned caster (Spellcasting
// level 1) is an Apprentice, and casting climbs it; deterministic (a plain level comparison, the
// same bands as veteran_title). Ready to grow — finer bands, a school prefix — the same way the
// sibling titles will.
inline const char* mage_title(const Skills& skills) {
  const Skill* cast = skills.find(SkillId::Spellcasting);
  if (cast == nullptr) return nullptr;  // never learned to cast -> no arcane rank
  const int lvl = cast->level;
  if (lvl < 3) return "Apprentice";  // levels 1-2 — just learned the first spell
  if (lvl < 6) return "Adept";       // 3-5 — a working caster
  if (lvl < 10) return "Magus";      // 6-9 — a practised mage
  return "Archmage";                 // 10+ — a master of the arcane
}

// A PERSONALITY-derived TEMPERAMENT title — how BRAVE or COWARDLY a character is, the panel-text
// twin of the field cue personality_tint (which tints the dot red->green by this SAME bravery
// axis). The FIFTH derived recognition beside standing_title (how good/bad), build_title (what
// fighter), deed_epithet (known for), and veteran_title (how experienced) — this one reads the
// PERSONALITY, surfacing the design's named "the Coward"/"Fearless" (character-systems.md). bravery
// is the most behaviourally-loaded axis (drifted by every Valor/Violence deed AND by a friend's
// death — see grief), so this badge shifts as a character is shaped by what it lives through — a
// title that reads "the war changed him". A pure const char* query: the HUD shows it, the sim never
// reads it, so it can't touch replay. An ALWAYS-present band like veteran_title (every character
// has a temperament); neutral bravery — the PLAYER's spawn default (an NPC gets a non-neutral
// archetype) — reads "Steady", and since nothing in the sim reads this the whole badge is
// bit-identical to before it existed. Symmetric bands at ±20 / ±60.
inline const char* temperament_title(std::int8_t bravery) {
  if (bravery >= 60) return "Fearless";     // +60..+100 — charges what others flee
  if (bravery >= 20) return "Bold";         // +20..+59 — leans brave
  if (bravery <= -60) return "the Coward";  // -60..-100 — the design's named deep-coward badge
  if (bravery <= -20) return "Timid";       // -20..-59 — leans cowardly
  return "Steady";                          // -19..+19 — neutral, the common case
}

// A BREADTH-derived title — the generalist counterpart to build_title's PEAK. Where build_title
// names your ONE dominant COMBAT attribute (a specialist's KIND of fighter), this rewards the
// opposite shape: a character trained BROADLY. It reads ALL SEVEN attributes — including the
// non-combat Wisdom/Charisma/Intellect that build_title deliberately ignores (the
// "Naturalist-style" title its comment promised for later) — and counts how many are meaningfully
// trained (level >= kVersatileAt). Enough breadth earns a badge; a specialist earns NONE
// (build_title already names their single peak), so — like deed_epithet, and unlike the always-on
// veteran/temperament bands — this returns nullptr below the bar rather than a filler band. The
// SIXTH derived recognition, beside standing_title (good/bad), build_title (which peak),
// deed_epithet (known for), veteran_title (how experienced) and temperament_title (how brave). A
// pure const char* query over Attributes (never a stored slot), so it always matches the levels
// behind it; the HUD shows it and the sim never reads it, so it can't touch replay. A fresh
// character — every attribute at the starting level 1 — clears nothing, counts 0 and gets no badge,
// so the whole title is bit-identical to before it existed. Deterministic (plain level compares, no
// RNG). Ready to grow — finer bands, a weight by how FAR past the bar each attribute sits — the
// same way its siblings will.
inline const char* versatile_title(const Attributes& attrs) {
  constexpr int kVersatileAt = 5;  // an attribute counts as "meaningfully trained" at level 5 — a
                                   // clear investment past the level-1 start (a tuning knob)
  const int trained =              // how many of the seven have cleared the bar (bool sums to int)
      (attrs.endurance.level >= kVersatileAt) + (attrs.strength.level >= kVersatileAt) +
      (attrs.dexterity.level >= kVersatileAt) + (attrs.luck.level >= kVersatileAt) +
      (attrs.wisdom.level >= kVersatileAt) + (attrs.charisma.level >= kVersatileAt) +
      (attrs.intellect.level >= kVersatileAt);
  if (trained >= 5) return "the Polymath";   // 5+ of 7 broadly mastered — a rare all-rounder
  if (trained >= 3) return "the Versatile";  // 3-4 developed sides — a generalist
  return nullptr;  // a specialist (or still green) — no breadth badge; build_title names the peak
}

}  // namespace eng::sim
