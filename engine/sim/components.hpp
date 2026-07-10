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
};

// The one derived scalar the whole morality system collapses to: positive = heroic repute,
// negative = villainous. A PURE function of the ledger (reads no sim state), so it is unit-testable
// and recomputed on demand rather than stored — the design's "multi-dimension ledger -> one derived
// standing". Weights are the design's exact .8/1.0/.6/.6/-1.2/-.8 scaled ×5 — the smallest scale
// that makes every one an integer (each is a multiple of 0.2) — so NO float enters the sim and
// replay stays bit-identical; the unit is thus "fifths of a design-point". Charity/Valor lift you,
// Cruelty/Violence sink you. Three deeds feed it so far — Charity and Valor lift, Cruelty (the
// first villain deed) sinks it below zero — proving the signed formula in both directions; Honesty,
// Loyalty and unjust Violence stay 0 until their deeds land. The formula was locked whole from the
// start, so each new deed is a one-line add here, never a reshape.
inline std::int32_t standing(const BehaviorLedger& led) {
  const auto d = [&](Deed k) { return led.dims[static_cast<std::size_t>(k)]; };
  return d(Deed::Charity) * 4 + d(Deed::Valor) * 5 + d(Deed::Honesty) * 3 + d(Deed::Loyalty) * 3 -
         d(Deed::Cruelty) * 6 - d(Deed::Violence) * 4;
}

// One directed tie: THIS entity -> `other`. The seed of the design's P8 RELATIONSHIPS (directed,
// sparse, small event-deltas). int8 like a Personality axis — affinity SATURATES into bond bands,
// it doesn't accumulate over a life toward a gate the way ledger dims do — so no wide int and no
// float ever enters the sim. Only `affinity` is fed for now (the design's R1 slice); its two
// siblings wire later with NO reshape: `trust` is a one-field append the day its own event lands,
// and the bond ladder (Acquaintance -> Friend -> Partner / Rival -> Nemesis) is a DERIVED band
// `bond_tier(affinity)` — a pure query, never a stored slot — exactly the standing ->
// standing_title split.
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
};

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

// How much bigger a fully-renowned dot draws, and the standing it takes to get there — presentation
// KNOBS. Renown (positive `standing`) is shown as PRESENCE (size), the channel the bravery tint
// (colour) and wounded dimming (brightness) leave free, so the three cues never fight. ~10 kills or
// ~13 rescues reaches the cap.
inline constexpr float kRenownMaxScale = 0.3f;     // +30% radius at full renown
inline constexpr std::int32_t kRenownFullAt = 50;  // standing at which the size bump caps

// Presentation-only, the morality twin of personality_tint: how much to scale a dot's radius for a
// character's RENOWN (its derived `standing`). Returns a >= 1.0 multiplier — 1.0 at neutral OR
// villainous standing (<= 0), rising linearly to 1 + kRenownMaxScale at kRenownFullAt and CAPPING
// there, so a celebrated colonist visibly looms while nobody balloons without bound. A pure
// function of the scalar (the renderer computes standing() and passes it in), so it's unit-testable
// and the sim never reads it. Villainy (negative standing) is left at authored size for now — its
// own cue lands when villain deeds do.
inline float renown_scale(std::int32_t standing_value) {
  if (standing_value <= 0) return 1.0f;
  const float t = standing_value >= kRenownFullAt
                      ? 1.0f
                      : static_cast<float>(standing_value) / static_cast<float>(kRenownFullAt);
  return 1.0f + t * kRenownMaxScale;
}

// The lower title threshold — cross it (in either direction) and you stop being anonymous. The
// upper one reuses kRenownFullAt, so the title flips to "Renowned" exactly when the renown dot-size
// caps.
inline constexpr std::int32_t kKnownAt = 15;

// A character's TITLE — the design's "derived recognition": a pure query over `standing`, never a
// stored slot, so it's always in sync with the deeds behind it. Five bands, symmetric about neutral
// (villain titles are ready but unreachable until a villain deed exists). This is the seed of the
// richer title system (Master Smith, Dragonslayer, the Butcher — from build + gear + deeds too);
// today it reads standing alone. Pure and unit-testable; the HUD shows it, the sim never reads it.
inline const char* standing_title(std::int32_t standing_value) {
  if (standing_value >= kRenownFullAt) return "Renowned";
  if (standing_value >= kKnownAt) return "Known";
  if (standing_value <= -kRenownFullAt) return "Notorious";
  if (standing_value <= -kKnownAt) return "Suspect";
  return "Unproven";
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
  // If > 0, a landed blow also ENVENOMS the victim: it applies a Poisoned that chips health for a
  // while after. 0 = not venomous. "Procs as data" (the design's P4) — an archetype knob, not a
  // special case in the combat code. Swarmers are venomous; brutes/sentinels aren't.
  float poison_per_second = 0.0f;
  // If spit_range > 0 this creature is a RANGED attacker: creature_spit periodically launches a
  // Projectile (the same primitive the player's throw uses) at the nearest person within
  // spit_range, dealing spit_damage on impact. 0 = melee-only (the default). More "procs as data" —
  // a spitter is just these three knobs, no new creature class. spit_timer counts down to its next
  // spit.
  float spit_range = 0.0f;
  float spit_damage = 0.0f;
  float spit_timer = 0.0f;
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

// Marks a character holding a GUARD (a raised block). While present, incoming creature blows are
// softened to kBlockDamageFactor of their damage — but the guarded stance ROOTS you to
// kGuardMoveScale of your speed, so it's a real trade (plant-and-tank vs move-and-dodge), not free
// upside. Set/cleared each tick by the MovePlayer command's `guard` flag (a held key), so it lasts
// exactly as long as you hold it. An ACTIVE, input-driven choice, so today only the player guards;
// the softening system already applies to anyone Blocking, so an NPC-guard behaviour is a
// follow-up.
struct Blocking {};
inline constexpr float kBlockDamageFactor =
    0.4f;  // fraction of a blow that gets through a raised guard
inline constexpr float kGuardMoveScale = 0.35f;  // how much a guard slows you — the block's cost

// A projectile in flight — a thrown attack (perform_throw) made VISIBLE and given a travel time. It
// HOMES on its `target`: advance_projectiles steers it there each tick and, on arrival, applies the
// (already VIT-mitigated) `damage`, crediting `owner` with Valor on a killing hit. Homing (not
// straight-line) so it reliably catches an APPROACHING creature — a straight shot aimed at where
// the foe was would overshoot as it closes. It carries its own render dot, so the renderer draws it
// for free. If the target dies mid-flight the shot is wasted (despawned) — the one cost of the
// travel delay. This is the reusable seed of every future ranged effect (arrows, spit, bolts).
struct Projectile {
  entt::entity target;   // the Enemy it homes on (despawns if this becomes invalid)
  entt::entity owner;    // who threw it — credited Valor on a killing hit
  float damage = 0.0f;   // pre-mitigated at launch (homing keeps the same target, so VIT is fixed)
  float speed = 600.0f;  // world units/second — faster than any creature, so it always converges
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
};

// A weapon lying on the ground, waiting to be WIELDED (the Equip command). The equip-
// counterpart of Pickup: not consumed for a one-off effect but WORN — its bonuses fold
// into an Equipped cache on the wearer until swapped. The first item that changes HOW you
// fight, and the first with a BANE: the design's non-negotiable "every item has a positive
// AND a negative trait, nothing rolls pure-upside" — here a heavier swing hits harder but
// slows you. The seed of the full P5 Equipment system (Item{def,quality,durability,traits},
// multi-slot Equipment); one hardcoded def for now (design: "defs hardcoded first").
struct Weapon {
  int strength_bonus = 4;      // + effective Strength while wielded (longer reach + harder hits)
  float move_penalty = 0.25f;  // the BANE: fraction of move speed lost while wielding (heft)
  // If > 0 the blade is VENOMOUS: a landed hit also envenoms the foe (Poisoned), the player-side
  // mirror of a swarmer's bite. "Gear grants a +aspect" (P5). A venom blade trades raw Strength for
  // this lingering chip — a poison build. 0 = a plain blade (bit-identical for anyone not wielding
  // one).
  float venom_per_second = 0.0f;
};

// A dropped piece of ARMOUR — the first defensive item, the offense/defence counterpart of
// Weapon. Worn in a SECOND slot (see Equipped), so a character can carry a weapon, armour, or
// both. Like every item it has a BANE, but a DISTINCT one from the weapon's move-heft: plate
// tires you, so it slows STAMINA recovery (a weaker second wind between fights).
struct Armour {
  float defence_bonus = 6.0f;  // + physical defence (softens blows via defence_of/mitigate)
  float stamina_regen_penalty = 0.30f;  // the BANE: fraction of stamina recovery lost while worn
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
};

// A fixed drinking spot — a pond or well the `drink` system tops nearby thirsty characters up from,
// WITHOUT being consumed (unlike a one-shot food orb). `radius` is how close you must be to drink.
// The seed of the design's water economy (wells now, irrigated crops later); a thirsty NPC walks to
// the nearest one (steer_npcs). Drawn like any entity if given a RenderDot — the sim never reads
// it.
struct WaterSource {
  float radius = 60.0f;
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
  Attribute endurance;  // fed by Conditioning + Toughness; each level past 1 grows the pools
  Attribute strength;   // fed by Striking; each level past 1 lengthens attack reach + damage
  Attribute dexterity;  // fed by Evasion + Striking; each level past 1 raises the dodge chance
  Attribute luck;       // fed by Scavenging; each level past 1 raises the chance to crit a strike
};

// Names an attribute so a data-driven `SkillDef` can say which attribute(s) a skill feeds
// (a skill's XP flows to its MAIN attribute a lot, and to each CONTRIBUTOR a little). An
// enum, not a member pointer, keeps the defs plain data — the shape mods will add rows to.
// New attributes append here (and get a case in `attr_ref`, guarded by -Wswitch).
enum class AttrId : std::uint8_t { Endurance, Strength, Dexterity, Luck };

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

}  // namespace eng::sim
