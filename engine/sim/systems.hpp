#pragma once

#include <cstdint>
#include <random>

#include <entt/entity/registry.hpp>

#include "engine/core/math.hpp"
#include "engine/sim/components.hpp"

// Systems: the BEHAVIOUR of the simulation.
//
// A system is just a free function that runs over every entity holding a given
// set of components (code-design-rules rule 11: "systems are plain functions in
// one explicit ordered schedule — no ISystem base class"). There is no clever
// framework here on purpose: World::step() calls these in a fixed, readable
// order, and that call order IS the definition of how a tick behaves. To add
// behaviour, you write a new free function and add one line to step().
//
// Every system takes the fixed per-tick timestep as `dt` — never a variable
// frame time. That is what "fixed timestep" buys: the same inputs always
// produce the same motion, on every machine and every frame rate.

namespace eng::sim {

// Copy each renderable entity's current Transform into its PrevTransform, so the
// renderer can smoothly interpolate between the last two ticks (see the fixed-
// timestep explanation in simulation.hpp). Runs first, before anything moves.
void snapshot_previous(entt::registry& reg);

// Steer NPCs: each NPC looks for the nearest Hazard within its senses and, if it
// finds one, sets its Velocity to flee directly away from it (otherwise it keeps
// drifting). The first taste of NPC behaviour — perception (find the threat) then
// action (set the velocity). MUST run before integrate_motion, which is what
// turns the velocity it sets into actual movement this tick.
void steer_npcs(entt::registry& reg);

// NPCs raise a guard: a hardened colonist (veteran Endurance) with a creature upon it PLANTS and
// emplaces Blocking — so resolve_creature_contacts softens the blow, it ripostes, and it trains the
// Guarding skill (the first NPC path to Guarding; only the player set Blocking before — a parity
// hole). A fresh (level-1) NPC never qualifies, so it is bit-identical. MUST run after steer_npcs
// (it overrides the flee velocity, rooting the guard) and before integrate_motion. No RNG.
void npc_guard(entt::registry& reg);

// Steer creatures: each Enemy homes straight in on the NEAREST person — the player or an
// NPC (anything with Stats that isn't itself a creature) — the hostile mirror of
// steer_npcs (which flees). Like steer_npcs, MUST run before integrate_motion so the
// chosen velocity turns into movement this tick.
void chase_prey(entt::registry& reg);

// Move every entity with a Transform and Velocity: position += velocity * dt. A mover standing in a
// MireZone advances at that bog's slow_factor this tick — the drag scales the MOVEMENT (position
// delta), not the stored velocity, so a mover nothing re-drives (a mote, an idle loner) crawls
// through and exits rather than compounding to a frozen stop. No mire -> position += velocity * dt.
void integrate_motion(entt::registry& reg, float dt);

// Wrap positions around the field edges (toroidal space) so nothing drifts out
// of view. Keeps the demo self-contained; a real game uses a camera instead.
void wrap_bounds(entt::registry& reg, Vec2 field_size);

// Recover every entity's vitals toward their max at each vital's regen rate.
// Runs over exactly the entities that have a Stats component (players, NPCs).
void regenerate_vitals(entt::registry& reg, float dt);

// Slowly REPAIR worn equipment (weapon/armour durability) for any bearer resting in a Hearth's
// warmth — the "repair later" the durability system promised, so gear is a managed resource (mend
// it at the base) rather than a one-way trip to shattering. No hearth in reach -> a no-op.
void mend_gear(entt::registry& reg, float dt);

// Update stamina from movement: any entity with Stats and a non-zero Velocity
// spends stamina; one standing still recovers it. (Motes have Velocity but no
// Stats, so they're untouched.) This makes movement cost something — the
// MovePlayer funnel reads the result and slows an exhausted player to a crawl.
void update_stamina(entt::registry& reg, float dt);

// Drain the Hunger need: every PERSON (Stats without the Enemy marker — player and NPCs,
// not creatures) loses hunger each tick, faster while moving. Hunger never self-recovers;
// you refill it by eating (collect_pickups). At 0 it starves — chipping health so an unfed
// character dies through the normal handle_deaths path. The first survival Need (P6).
void drain_hunger(entt::registry& reg, float dt);

// Drain the Water need: the twin of drain_hunger for the SECOND survival Need. Every person loses
// water each tick, faster while moving; at 0 they dehydrate, chipping health through the same death
// path. Refilled not by orbs but by the `drink` system at a fixed WaterSource — the design's "walk
// to the well" loop. (P6.)
void drain_water(entt::registry& reg, float dt);

// Refill Water: every person standing within a WaterSource's radius drinks — its water rises toward
// full while it lingers. The source is NOT consumed (unlike a food orb), so it's a place you return
// to. Downed bodies and creatures don't drink. (P6.)
void drink(entt::registry& reg, float dt);

// Drive the WARMTH need — the design's temperature, made LOCAL. A person loses warmth inside a
// ColdZone, regains it by a Hearth's fire, and holds steady in the open; at 0 it FREEZES, chipping
// health like starving/dehydrating. So it's a "flee the cold, huddle by the fire" pressure, not a
// background timer. No ColdZone in the world -> nobody chills -> bit-identical.
void drain_warmth(entt::registry& reg, float dt);

// Chip HEALTH from anyone standing in a HazardField (persistent damaging terrain: brambles, a scald
// patch). The direct-HP twin of drain_warmth's cold; bites creatures too (kite a brute across it).
void tick_hazard_fields(entt::registry& reg, float dt);

// Tick the Fatigue need — the THIRD survival Need, and the one that RECOVERS with rest. Every
// person loses fatigue while exerting (moving, faster sprinting — the same rest<walk<sprint tiers
// the other needs use) and regains it while standing still. Unlike hunger/water it self-recovers,
// so it's the "you can't run forever" pressure rather than a "feed me" one. Clamped to [0, max].
// The empty- collapse consequence and the sit/sleep faster-rest tiers are follow-up slices; this is
// the seam.
void tick_fatigue(entt::registry& reg, float dt);

// Tend the food plots and feed grazers: every FoodSource regrows its stock a little each tick, then
// hands food to nearby hungry people (refilling hunger, DEPLETING the plot). A picked-bare plot
// can't feed until it regrows — the design's renewable-but-finite food production. (P6.)
void graze(entt::registry& reg, float dt);

// Resolve contact damage: any entity with Stats (the player or an NPC) that
// overlaps a Hazard takes its `damage`, and the hazard is then consumed
// (destroyed). A SYSTEM, not a command — collision is the sim's own rule, so it
// changes state directly (the funnel is only for input from outside the sim).
// Note it destroys entities only AFTER iterating; destroying during iteration
// invalidates the view (a classic bug).
void resolve_contacts(entt::registry& reg);

// Resolve creature contact: an Enemy overlapping a player, once its attack cooldown
// is up, deals a `attack_damage` blow softened by the player's VIT (ratio mitigation)
// and trains the player's Toughness (via train_on_damage). Unlike a mote it is NOT
// consumed — it keeps chasing and swinging on its cooldown. `dt` advances the
// cooldown. The player may DODGE a swing (a DEX-driven roll off `rng`), taking no
// damage; facing a swing at all trains Evasion -> Dexterity, hit or miss. A SYSTEM,
// not a command (collision is the sim's own rule).
void resolve_creature_contacts(entt::registry& reg, float dt, std::mt19937& rng);

// Age every Poisoned entity: chip its `health` by the venom's damage-per-second (routing a lethal
// dose through the normal handle_deaths death path), count the timer down, and reap the status when
// it wears off. The lingering-damage half of a venomous blow (resolve_creature_contacts applies
// it).
void tick_poison(entt::registry& reg, float dt);

// Age each cast barrier (Shielded) and reap it when spent — the BUFF twin of tick_poison. MUST run
// AFTER resolve_creature_contacts so a freshly-cast shield still soaks that tick's blows before
// this ages it (apply-at-cast, use-at-contact, decay-here — poison's rhythm). Collect-then-remove
// (a mid-view removal invalidates the iterator). No RNG.
void tick_shield(entt::registry& reg, float dt);

// Age each cast quickening (Hasted) and reap it when spent — the BUFF twin of tick_shield. Order is
// not load-bearing (integrate_motion reads Hasted at the START of the tick; this just decays it),
// so it sits beside tick_shield. Collect-then-remove (a mid-view removal invalidates the iterator).
// No RNG.
void tick_haste(entt::registry& reg, float dt);

// Count down the PANIC of every routed colonist (Panicked, emplaced by handle_deaths when a bonded
// friend falls) and remove it when it lapses — the acute grief reaction wears off and the colonist
// recovers its nerve. The timer twin of tick_poison; a no-op when nothing is panicked. Draws no
// RNG.
void tick_panic(entt::registry& reg, float dt);

// Fly every in-flight Projectile toward its homing target and, on arrival, apply its carried damage
// (crediting the owner Valor on a killing hit, but only for felling a hostile) and despawn. A shot
// whose target has died mid-flight is despawned unhit (a wasted throw). The delayed-impact half of
// a throw (perform_throw launches it). MUST run after integrate_motion (positions current) and
// before handle_deaths (a killed target reaps the same tick). Draws no RNG.
void advance_projectiles(entt::registry& reg, float dt);

// The RANGED creature attack — the hostile mirror of the player's throw. Each spitter (an Enemy
// with spit_range > 0), off its own spit cooldown, launches a homing Projectile (the same
// primitive) at the nearest person in range. So a ranged enemy reuses advance_projectiles
// wholesale. MUST run after integrate_motion (positions current); place it before
// advance_projectiles so a fresh spit starts flying the same tick. Draws no RNG.
void creature_spit(entt::registry& reg, float dt);

// Age every entity's HitFlash and remove the ones that have burned out. Pure
// presentation upkeep — HitFlash is stamped at the damage sites so the renderer can
// blink a struck dot white, and this fades it over kHitFlashSeconds. No rule reads it.
void decay_flashes(entt::registry& reg, float dt);

// Train Toughness on a hit: surviving `damage` grows the victim's Toughness skill
// and its main attribute Endurance (a VIT skill — you toughen by enduring hardship),
// which advance_progression turns into a bigger HP pool. The single place damage
// feeds progression, so every damage source — contact now, weapons later — trains
// it the same way just by calling this. A no-op for entities without Skills.
void train_on_damage(entt::registry& reg, entt::entity victim, float damage);

// Stamp a fresh white hit-flash on an entity that just took a blow — presentation only (the
// renderer blinks it; no rule reads HitFlash). Public so every damage source shares one "blink on
// a hit": the systems contact/attack/projectile sites and the DamagePlayer command alike. Draws no
// RNG; emplace_or_replace refreshes rather than stacks.
void stamp_flash(entt::registry& reg, entt::entity e);

// Resolve one melee swing for `attacker`: find the nearest attackable target (a
// Hazard mote OR a hostile Enemy) within reach (reach grows with Strength), train
// Striking -> Strength for a connecting strike, and act by target kind:
//   - a MOTE is fragile: it's returned for the caller to destroy (instant kill).
//   - an ENEMY takes STR-vs-VIT damage to its HP (base + Strength, softened by the
//     enemy's VIT via ratio mitigation); it is NOT returned — it dies later through
//     handle_deaths when HP hits 0, so it survives weak hits and takes several.
// Returns the mote to destroy, or entt::null (missed, hit an enemy that lived, or the
// enemy DODGED). An enemy may slip the strike on a DEX roll off `rng` (the offensive
// mirror of a player dodging a creature's blow) — a swarmer is slippery, a brute is not.
// Callers collect-then-destroy so no view is invalidated mid-iteration. Shared by the
// player's Attack command and npc_attack. A no-op without Transform+Attributes+Skills.
entt::entity perform_attack(entt::registry& reg, entt::entity attacker, std::mt19937& rng);

// The player's RANGED option: hurl at the nearest hostile far out of melee reach for a modest,
// RNG-free (no dodge/crit/execute) chip that COSTS STAMINA, so a wave can be softened but not kited
// forever. Trains Throwing -> Dexterity; credits Valor on a killing throw, exactly like a melee
// kill. A no-op if nothing hostile is in range or the thrower is out of stamina (an exhausted
// fizzle). Player-driven only (no npc_throw); needs Transform + Attributes + Skills + Stats. Draws
// no rng, so unlike perform_attack it takes none.
void perform_throw(entt::registry& reg, entt::entity attacker);

// The player's first SPELL — the MAGIC mirror of perform_throw: a homing bolt at the nearest
// hostile in range for an RNG-free chip, but gated on the LEARNED Spellcasting skill (magic is
// taught, not innate) and spending MANA (mp), not stamina. INTELLECT scales its damage; casting
// trains Spellcasting -> INT. A no-op if the caster hasn't learned it, nothing's in range, or the
// mana bar is empty. Actor-agnostic: the player casts it via the Cast command, an NPC via npc_cast
// (the player==NPC parity). Reuses the throw's Projectile primitive (advance_projectiles delivers
// it). Draws no rng.
void magic_bolt(entt::registry& reg, entt::entity caster);

// Cast a MEND — the support twin of magic_bolt. A learned caster (Spellcasting) restores HP to the
// nearest WOUNDED ally in range, spending mana, and trains Healing -> WISDOM (the mend scales with
// WIS, the way a bolt scales with INT). Instant (no Projectile — a mending word), clamped at max
// (no over-heal), never self (that's regenerate_vitals). A no-op if the caster hasn't learned to
// cast, no ally is hurt in range, or the mana bar is empty — so a non-caster world is
// bit-identical. Actor- agnostic: the player casts it via the CastHeal command, an NPC via npc_heal
// (the parity). No RNG.
void heal_spell(entt::registry& reg, entt::entity caster);

// Cast a SHIELD — the DEFENSIVE spell of the caster's kit (bolt = offence, mend = support, cleanse
// = cure). A learned
// caster (Spellcasting) raises a timed BARRIER on ITSELF: for a few seconds, `absorb` is soaked off
// each creature blow (resolve_creature_contacts reads it), thickness scaling with INTELLECT and
// trained by casting -> Spellcasting -> INT. (Re)emplaces the caster's own Shielded
// (get_or_emplace, so a recast re-ups rather than stacks); tick_shield ages it. A no-op if the
// caster hasn't learned, is at 0 HP, or the mana bar is empty -> a non-caster world is
// bit-identical. Actor-agnostic like the bolt/mend: the player casts it via CastShield, a colonist
// mage via npc_shield (a threat-triggered self-ward) — the player==NPC parity. No RNG.
void shield_spell(entt::registry& reg, entt::entity caster);

// Cast a HASTE — the UTILITY spell of the caster's kit (bolt = offence, mend = support, shield =
// defence, cleanse = cure, haste = mobility). A learned caster (Spellcasting) raises a timed
// quickening on ITSELF: for a few seconds, integrate_motion scales its movement by `factor`, which
// scales with INTELLECT and trains by casting -> Spellcasting -> INT. (Re)emplaces the caster's own
// Hasted (get_or_emplace, so a recast re-ups rather than stacks); tick_haste ages it. A no-op if
// the caster hasn't learned, is at 0 HP, or the mana bar is empty -> a non-caster world is
// bit-identical. UNLIKE the shield, the factor is NOT need-scaled (the base velocity already is —
// see haste_spell). Player-only for now (no npc_haste yet). No RNG.
void haste_spell(entt::registry& reg, entt::entity caster);

// NPCs fight back: every NPC with a hazard in reach strikes it (via perform_attack),
// training Striking -> Strength just as the player does — so NPCs build Strength too,
// not only Endurance. Complements steer_npcs (flee): a threat that closes to reach
// gets struck rather than merely dodged. `rng` drives the target's dodge roll (threaded
// to perform_attack). MUST run after integrate_motion (positions current) and before
// resolve_contacts (so a struck mote can't also land its hit).
void npc_attack(entt::registry& reg, std::mt19937& rng);

// NPCs cast: every Npc that has LEARNED Spellcasting and has a FULL mana bar flings a bolt at the
// nearest hostile in range (via the shared magic_bolt), so a colonist mage casts exactly as the
// player does — the caster twin of npc_attack, closing the player==NPC parity for magic. The
// full-bar gate throttles it to a considered bolt then a recharge (no per-tick spam), and
// magic_bolt no-ops with no target or no mana. MUST run after integrate_motion (positions current)
// and before advance_projectiles (so a fresh bolt flies the same tick). Draws no RNG.
void npc_cast(entt::registry& reg);

// NPCs mend: every Npc that has LEARNED Spellcasting and has a FULL mana bar mends the nearest
// wounded ally (via the shared heal_spell) — the support twin of npc_cast, closing the player==NPC
// parity for healing. Runs AFTER npc_cast and shares the full-bar throttle, so a battle-mage bolts
// a threat first (spending mana) and only mends in a lull — offence-then-support falls out of the
// order. heal_spell no-ops with no wounded ally or no mana. MUST run after integrate_motion
// (current positions). Draws no RNG.
void npc_heal(entt::registry& reg);

// NPCs ward: every Npc that has LEARNED Spellcasting, has a FULL mana bar, is NOT already Shielded,
// and has a creature within a threat range raises a barrier on itself (via the shared shield_spell)
// — the defensive twin of npc_cast/npc_heal, closing the player==NPC parity for the shield. Runs
// BEFORE npc_cast so a threatened mage wards first then bolts under the barrier. Wards once per
// lapse (the not-already-Shielded gate), not per recharge. MUST run after integrate_motion (current
// positions). Draws no RNG.
void npc_shield(entt::registry& reg);

// Cast a CURE: a learned caster strips Poisoned off the nearest poisoned ally in range, spending
// the same mana as a bolt/mend and training the same Healing -> Wisdom skill — the fourth of the
// caster's kit (bolt / mend / barrier / CLEANSE), the restorative counter to a swarmer's or
// spitter's venom (heal restores lost HP; cleanse stops the DoT draining it). Ally-only (not self,
// like the mend), a 0-HP body is inert, and a non-caster / empty bar / no-poisoned-ally is a silent
// no-op -> bit-identical. Unlike heal it MUTATES THE ENTITY SET (reg.remove<Poisoned>), done after
// the search loop. Actor-agnostic: the player casts it via CastCleanse, an NPC via npc_cleanse. No
// RNG.
void cleanse_spell(entt::registry& reg, entt::entity caster);

// NPCs cure: every Npc that has LEARNED Spellcasting and has a FULL mana bar strips the venom off
// the nearest poisoned ally (via the shared cleanse_spell) — the fourth caster verb's NPC twin,
// closing the player==NPC parity for the cure. Runs AFTER npc_heal and shares the full-bar
// throttle, so a battle-mage bolts/mends first and cures in a lull. Collect-then-act (cleanse_spell
// removes Poisoned, mutating the entity set). MUST run after integrate_motion (current positions).
// No RNG.
void npc_cleanse(entt::registry& reg);

// NPCs farm: every Npc carrying a Provider Aspiration reaps the nearest ripe food plot in reach
// into a meal (via the shared, actor-agnostic harvest_nearest_crop the player's Harvest command
// also calls) — the peaceful mirror of npc_attack/npc_cast, the design's "NPC farm behaviour".
// Self-throttled by the plot's regrow (harvest_nearest_crop no-ops unless a plot is ripe and in
// reach). No Provider aspiration -> no-op -> bit-identical. Collect-then-harvest (spawn_meal
// creates an entity). MUST run after integrate_motion (a provider is at the plot by then). Draws no
// RNG.
void npc_harvest(entt::registry& reg);

// Wear the nearest dropped GEAR within reach of `wearer` — a Weapon or a piece of Armour,
// whichever is closer — folding its mods into the matching SLOT of an Equipped cache and
// RETURNing the item to destroy, or entt::null if none in reach. Non-clobbering: each slot's
// pair is written independently, so grabbing armour keeps a wielded weapon and vice-versa. THE
// one place gear mods are folded — shared by the player's Equip command and npc_equip, so a
// player and an NPC gear up identically (the player==NPC parity guardrail).
entt::entity equip_nearest_gear(entt::registry& reg, entt::entity wearer);

// NPCs gear up: every UNGEARED NPC within reach of dropped gear grabs it (via
// equip_nearest_gear) — the parity twin of the player's Equip command, so the mods
// perform_attack/defence_of read for any entity are actually earned by NPCs too. MUST run
// after integrate_motion (positions current). steer_npcs walks the unarmed ones to a blade first.
void npc_equip(entt::registry& reg);

// Gather the nearest ripe food plot within reach into a MEAL at the harvester's feet — the food
// economy's seam: a prepared meal (spawn_meal) fills more hunger than grazing the same stock raw.
// Spends a chunk of the plot's stock; a plot too bare to bother yields nothing. Returns whether a
// crop was harvested. THE one definition shared by the player's Harvest command and (later) an NPC
// farm behaviour, so the two harvest identically (the player==NPC parity guardrail).
bool harvest_nearest_crop(entt::registry& reg, entt::entity harvester);

// Sow a new crop SEEDLING where the planter stands — the FRONT of the food chain (plant -> grow ->
// harvest -> meal). It is a FoodSource like the wild garden but starts with zero stock, so it feeds
// no one until the existing regrow (graze) grows it ripe. Returns the new crop entity. Actor-
// agnostic and public: the one call the player's Plant command and the Provider NPC farmer
// (npc_harvest sows through it where the land is barren) both make.
entt::entity plant_crop(entt::registry& reg, entt::entity planter);

// Advance progression, the whole "learn by doing" chain in one pass over every
// entity with Skills + Attributes + Stats + Velocity + CharacterLevel: activity
// earns XP for the skill it trains, that skill's main attribute, AND a fraction to
// the global Character Level; full XP bars level each up; and derived stats (max
// health & stamina) grow from the attribute's level, scaled a little by the
// Character Level's veteran multiplier. Runs on the player and NPCs alike. No `dt`
// — the timestep is fixed, so XP is a constant per-tick amount. (The XP curve
// `xp_to_next` and the effect curve `power` live in progression/curve.hpp.)
// MENTORSHIP: a colonist far ahead in a skill teaches it to a nearby much-lower one — the student
// gains XP in that skill, the mentor grows a Teaching -> Charisma skill for passing it on. So
// skills SPREAD through the colony, not only from each person's own doing. Gated on a skill-level
// gap that can't exist at spawn (all skills start at level 1), so a fresh/short-run world is
// bit-identical. Runs on players and NPCs alike (creatures have no Skills). Place it just before
// advance_progression so the lesson's XP banks the same tick. No dt (fixed timestep).
void teach(entt::registry& reg);

void advance_progression(entt::registry& reg);

// The rolled-quality band for a FINE drop (a slain brute's steel, a sentinel's plate): its
// `quality` is drawn uniformly from [kFineQualityMin, kFineQualityMax), so two tough kills yield
// subtly different gear and looting stays interesting past the first drop. The floor is above 1.0
// so a fine drop is always a finer ITEM than baseline (the equip fold scales the boon by quality);
// the int truncation on a weapon's +Strength can still round a modest roll back to the baseline
// integer — a known ceiling, smooth on armour's float defence. Shared with the drop test so the
// band is one source of truth.
inline constexpr float kFineQualityMin = 1.1f;
inline constexpr float kFineQualityMax = 1.4f;

// A fine steel weapon drop can rarely roll a VENOMOUS variant — the first named equipment TRAIT: a
// heavy steel blade that keeps its full heft bane but trades one notch of raw Strength for hits
// that ENVENOM (reusing the whole already-wired venom -> Poisoned path). A NAMED intra-item trade
// (poison build vs raw power) — the qualitative variety the flat boon/bane pairs lacked — with NO
// new struct field and NO traits[] list (both premature: the Equipped comment already documents the
// anti-list stance, and a list earns its place only when two named traits must stack on one item).
// The variant decision is ONE raw mt19937 draw off the dedicated drop stream — PORTABLE across
// stdlibs (unlike the uniform_real quality draw), so ~15% of fine steel drops come out venomous,
// identically everywhere. ponytail: flat 15% for a brute's steel; a per-archetype chance is the
// refinement if a spitter-kill steel should differ. The strength/venom/chance numbers are
// calibration knobs.
inline constexpr std::uint32_t kVenomousDropThreshold =  // ~15% of mt19937's full 2^32 output range
    static_cast<std::uint32_t>(0.15 * 4294967296.0);
inline constexpr int kVenomousStrength = 3;  // steel's +4 knocked down one notch (the paired -STR)
inline constexpr float kVenomousVenomPerSecond =
    4.0f;  // a modest chip on a heavy steel base (a knob)

// The SECOND named weapon trait: a fine steel drop can instead roll KEEN — a razor blade that lands
// the doubled crit more often (perform_attack adds this to the Luck crit chance), bought with the
// same one-notch Strength trade as venomous — a distinct PROC (crit vs poison), a different build.
// This is the point where a trait needs a FIELD (crit is not expressible through an existing Weapon
// field, unlike venom), so Weapon/Equipped each gain a `crit_bonus` (appended LAST, default 0 ->
// bit-identical). Still NO traits[] list: the variants are MUTUALLY EXCLUSIVE (a drop is plain OR
// venomous OR keen), so no two traits stack on one item — a list only earns its place when they
// must. Rolled off the same dedicated drop stream as venomous, the decision a PORTABLE raw draw.
// Knobs.
inline constexpr std::uint32_t
    kKeenDropThreshold =  // ~15% too; sits just past the venomous band in
    static_cast<std::uint32_t>(0.15 * 4294967296.0);  // the one raw draw (see handle_deaths)
inline constexpr int kKeenStrength = 3;  // steel's +4 down one notch — the paired -STR trade
inline constexpr float kKeenCritBonus = 0.15f;  // +15% crit chance while wielded (a knob)

// The ARMOUR analog of the two weapon traits: a fine sentinel-armour drop can roll WARDED — thorns
// that reflect a chip onto an attacker, bought with a reduced defence (`spawn_warded_armour`,
// already wired through the equip fold -> `Equipped.armour_thorns` -> `resolve_creature_contacts`,
// and unit-tested). Its only spawn was a single hardcoded seed, so warded plate could never appear
// as loot — this lets it drop like the weapon traits do. Rolled off the SAME dedicated drop stream
// by ONE portable raw draw; ~15% warded, ~15% EVASIVE (the second armour trait, below), the rest
// plain — two MUTUALLY-EXCLUSIVE bands now, exactly like the weapon's venomous/keen (an armour drop
// rolls warded OR evasive OR plain, never two). Knobs.
inline constexpr std::uint32_t kWardedDropThreshold =  // ~15% of mt19937's full 2^32 output range
    static_cast<std::uint32_t>(0.15 * 4294967296.0);
// The EVASIVE band, sitting JUST PAST the warded band (like keen sits past venomous): a raw draw in
// [kWardedDropThreshold, kWardedDropThreshold + kEvasiveDropThreshold) rolls an evasive plate
// (+dodge, -defence). Placed ABOVE warded so the warded band [0, kWardedDropThreshold) is unchanged
// — a warded-seed drop stays warded, bit-identical. ~15% too. Knob.
inline constexpr std::uint32_t kEvasiveDropThreshold =  // ~15% of mt19937's full 2^32 output range
    static_cast<std::uint32_t>(0.15 * 4294967296.0);

// React to death. A player at 0 health goes DOWNED — helpless where they fell for a
// timer (`dt` counts it down); a living ally within reach revives them in place, else on
// expiry they respawn at `respawn_point`. An NPC or creature at 0 health is destroyed —
// permadeath, the game's core rule; a slain CREATURE drops a health Pickup where it fell.
// MUST run before regenerate_vitals, or a just-killed entity gets healed back above 0 the
// same tick and never dies (a downed player is also excluded from regen for the same reason).
// `rng` is a DEDICATED drop stream: it rolls each fine drop's quality (above) and NOTHING else, so
// loot varies without perturbing the creature/combat stream — every wave, dodge and spawn is
// unchanged. Draws from it only when a fine drop actually lands (a brute/sentinel death).
void handle_deaths(entt::registry& reg, Vec2 respawn_point, float dt, std::mt19937& rng);

// Record one moral DEED on an actor's BehaviorLedger — the SINGLE write-point the whole morality
// system funnels through (the design's one `record_deed`). Lazily emplaces the ledger (an actor
// earns one on its first deed, so a never-acting entity stays ledger-free and bit-identical), then
// adds `mag` to the `kind` dimension. Pure and deterministic (no RNG); it takes a bare entity with
// no player/NPC branch, so morality has player==NPC parity for free. Every future deed kind is a
// new CALLER of this function, never new state.
void record_deed(entt::registry& reg, entt::entity actor, Deed kind, std::int32_t mag);

// Leaky moral DECAY: every kDecayPeriod ticks, each of an actor's deed dimensions creeps one step
// toward 0, so a reputation FADES if it isn't renewed — the design's "redemption and corruption for
// free" (a villain who stops being cruel climbs back toward neutral; a hero who rests on old glory
// dims). Symmetric about neutral. Only actors WITH a ledger (those who've done a deed) decay. An
// exact per-ledger integer tick-count (BehaviorLedger::decay_ticks), so it stays bit-exact; takes
// no dt (per-tick, fixed-timestep, like advance_progression). Runs once per step().
void decay_standing(entt::registry& reg);

// Leaky RELATIONSHIP decay — the affinity twin of decay_standing: every kBondDecayPeriod ticks each
// UNLATCHED affinity edge creeps one step toward 0, so a tie COOLS if it isn't renewed. A deep bond
// (Partner) or grudge (Nemesis) LATCHES (`bond_latched`) and resists, so the strongest ties persist
// — the design's "bonds latch, resist decay". An exact per-Relationships integer counter, so
// bit-exact; takes no dt (per-tick, fixed-timestep). Runs once per step().
void decay_bonds(entt::registry& reg);

// Nudge a directed RELATIONSHIP — the record_deed twin, the SINGLE write-point for the P8
// relationships seed. Lazily emplaces `from`'s Relationships (edge-free until its first bond, so
// bit-identical when absent), find-or-updates the edge toward `toward` by `delta`, clamped to ±100.
// Pure/deterministic, touches no view (safe mid-iteration). Every future bond-forming event is a
// new CALLER, never new state. Stores entity handles by value, so every READER must gate on
// reg.valid.
void nudge_affinity(entt::registry& reg, entt::entity from, entt::entity toward, std::int8_t delta);

// The peaceful BEFRIEND path: colonists sharing a hearth warm to each other over time (up to
// Friend, never the earned Partner tier). Called staggered from World::step. See the .cpp.
void socialize(entt::registry& reg);

// The reader counterpart of nudge_affinity: how `from` feels about `toward` (its directed
// affinity), or 0 if there is no tie. A const, no-op-when-absent lookup used by the bond/grudge
// readers.
std::int8_t affinity_toward(const entt::registry& reg, entt::entity from, entt::entity toward);

// How many entities regard `e` as a friend (an INCOMING bond at/above kBondPull) — the mirror of
// the HUD's outgoing "closest bond", and the camaraderie payoff made legible: every colonist bonded
// TO you is one ally the steer_npcs DEFEND rung will send rushing to your side. A read-only whole-
// registry scan the HUD calls; the sim never reads it.
int allies_of(const entt::registry& reg, entt::entity e);

// How many entities hold a GRUDGE against `e` (an INCOMING affinity at/below kGrudgeThreshold) —
// the negative mirror of allies_of: the enmity a cruel character has sown, the "who won't cross the
// field to save you" count. A read-only whole-registry scan the HUD calls; the sim never reads it.
int foes_of(const entt::registry& reg, entt::entity e);

// Spawn a Weapon on the ground at `pos` — the one canonical grounded-weapon entity, shared by
// a slain brute's drop (handle_deaths) and the player's Drop command so both look and behave
// identically. `quality` scales the item's boon (1.0 = baseline; a tough kill drops finer). No RNG.
void spawn_weapon(entt::registry& reg, Vec2 pos, float quality = 1.0f);
// Spawn a weapon on the ground carrying a SPECIFIC identity (the dropped-blade case) — its stat
// block copied verbatim, so a keen/venom/worn blade is picked back up IDENTICAL rather than reset
// to plain steel. Used by the Drop command (world.cpp) to preserve item identity across a
// drop/re-equip.
void spawn_weapon(entt::registry& reg, Vec2 pos, const Weapon& def);

// Spawn a piece of Armour on the ground at `pos` — the defensive counterpart of spawn_weapon
// (distinct render colour). Used to seed wearable armour into the opening scene, and dropped
// (finer) by a slain sentinel. `quality` scales its boon (1.0 = baseline). Draws no RNG.
void spawn_armour(entt::registry& reg, Vec2 pos, float quality = 1.0f);

// Spawn a VENOM blade on the ground at `pos` — the second weapon TYPE (a poison build: weaker,
// nimbler hits that envenom the foe). The ONE canonical grounded venom-weapon entity, shared by a
// slain spitter's drop (handle_deaths) and the opening scene (build_scene), so both look and behave
// identically — the same no-drift discipline as spawn_weapon. Draws no RNG.
void spawn_venom_weapon(entt::registry& reg, Vec2 pos);

// Spawn a VAMPIRIC blade on the ground at `pos` — the third weapon TRAIT (a sustain build: hits
// DRINK, healing the wielder a fraction of the damage dealt), bought with a -Strength notch so it's
// never pure-upside. The player-side mirror of the leech creature's on-bite heal. Draws no RNG.
void spawn_vampiric_weapon(entt::registry& reg, Vec2 pos);

// Spawn a VENOMOUS STEEL blade — a fine steel weapon that rolled the venomous trait
// (handle_deaths). Distinct from spawn_venom_weapon's light fang: this keeps steel's FULL heft
// (move_penalty 0.25) and trades only ONE notch of Strength (+3 not +4) for a modest venom, so it's
// a HEAVY poison build, not a nimble one. `quality` scales its (reduced) boon like any fine steel.
// The canonical grounded venomous-steel entity so a future player-Drop stays parity-clean. Draws no
// RNG (the caller rolled).
void spawn_venomous_steel(entt::registry& reg, Vec2 pos, float quality = 1.0f);

// Spawn a KEEN STEEL blade — a fine steel weapon that rolled the keen trait (handle_deaths). Like
// venomous steel it keeps steel's full heft and trades one notch of Strength (+3), but the boon is
// CRIT (kKeenCritBonus) rather than venom — a razor edge that lands the doubled blow more often,
// feeding a Luck/crit build. The canonical grounded keen-steel entity so a future player-Drop stays
// parity-clean. Draws no RNG (the caller rolled).
void spawn_keen_steel(entt::registry& reg, Vec2 pos, float quality = 1.0f);

// The WARDED (spiked) plate's paired numbers — armour's FIRST flavourful trait, the defensive twin
// of venomous/keen steel. It trades a notch of raw defence (kWardedDefence, below plain plate's 6)
// for THORNS (kWardedThorns): every creature blow it absorbs chips the attacker back. So it is
// never pure-upside — you soak a little less to punish the swarm. Knobs.
inline constexpr float kWardedDefence =
    4.0f;  // plain plate's 6 down two — the paired -defence trade
inline constexpr float kWardedThorns =
    2.5f;  // ...bought with spikes: flat chip back per blow soaked

// The EVASIVE (light) plate's paired numbers — armour's SECOND flavourful trait, the twin of the
// weapon's venom+keen pair. It trades MORE raw defence than warded (kEvasiveDefence, plain plate's
// 6 down THREE) for a flat DODGE bonus (kEvasiveEvasion): the wearer slips more blows OUTRIGHT, the
// active-avoidance counterpart to warded's passive chip-back. Well under the 0.50 dodge cap, so it
// stacks with earned DEX without ever guaranteeing a dodge — a stream of hits still lands. Knobs.
inline constexpr float kEvasiveDefence =
    3.0f;  // plain plate's 6 down three — the paired -defence trade (lighter than warded's 4)
inline constexpr float kEvasiveEvasion =
    0.12f;  // ...bought with nimbleness: +12% flat dodge while worn

// Spawn a WARDED plate — a piece of Armour that rolled the thorns trait, the defensive counterpart
// of spawn_keen_steel/spawn_venomous_steel. Reduced defence (kWardedDefence) in exchange for
// reflecting kWardedThorns onto any creature that strikes the wearer (resolve_creature_contacts). A
// distinct spiked-iron tint so it reads apart from plain bronze plate. Draws no RNG.
void spawn_warded_armour(entt::registry& reg, Vec2 pos, float quality = 1.0f);

// Spawn an EVASIVE plate — a piece of Armour that rolled the dodge trait, the LIGHT counterpart of
// spawn_warded_armour. Reduced defence (kEvasiveDefence, a bigger notch than warded's) in exchange
// for a flat kEvasiveEvasion dodge bonus added to the wearer's dodge in resolve_creature_contacts
// (where a plate meets a creature's blow). A distinct pale tint so it reads apart from plain bronze
// plate. Draws no RNG.
void spawn_evasive_armour(entt::registry& reg, Vec2 pos, float quality = 1.0f);

// Spawn a WATERSKIN at `pos` — a portable Pickup that refills the WATER need on collect (pk.water),
// the water twin of a meal / the portable answer to a fixed well. Pure water (no heal/food/max-HP),
// a long lifetime so a placed cache lingers, no RNG. Closes the food-has-a-pickup / water-doesn't
// asymmetry; build_scene places one as a showcase supply.
void spawn_waterskin(entt::registry& reg, Vec2 pos);

// Collect loot and age it: each Pickup's `lifetime` counts down by `dt`, and one a
// player overlaps restores its `heal` health (capped) AND permanently raises max HP by
// its `bonus_max_hp`, then is consumed; an orb whose lifetime runs out fades away
// uncollected (so drops from far-off kills don't pile up forever). Collect-then-destroy.
void collect_pickups(entt::registry& reg, float dt);

// Learn magic by READING: ANY person (player OR NPC) standing on a Spellbook gains the Spellcasting
// skill — the design's "magic is learned, not innate" made a found-and-read loop, and the
// player==NPC parity (a colonist that finds a tome becomes a mage too, which npc_cast then lets
// cast). The tome is a PERMANENT LIBRARY, NOT consumed: the whole colony can learn from one book
// over time (the Scholar aspiration's supply), and the player no longer "steals" the only tome by
// reaching it first. Creatures carry no Skills so they never learn; Downed bodies are excluded.
// Skips a reader who already knows the spell (a re-read is a no-op). Mutates only values, destroys
// nothing — view-safe.
void study_spellbooks(entt::registry& reg);

}  // namespace eng::sim
