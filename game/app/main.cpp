#include <SDL3/SDL.h>
#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "engine/core/log.hpp"
#include "engine/core/math.hpp"
#include "engine/gpu/renderer.hpp"
#include "engine/net/loopback.hpp"
#include "engine/net/server.hpp"
#include "engine/sim/components.hpp"
#include "engine/sim/progression/curve.hpp"
#include "engine/sim/simulation.hpp"
#include "engine/sim/systems.hpp"
#include "engine/sim/types.hpp"
#include "engine/sim/world.hpp"

// The game client — the top of the whole stack. It ties everything together:
//
//   input  ->  Command  ->  transport  ->  Server owns the World  ->  render
//
// Read this file top to bottom to see one frame of the engine: sample the
// keyboard into a movement Command, send it to the server over the loopback
// transport, let the server step the fixed-timestep simulation, then draw the
// world (with interpolation) plus a Dear ImGui debug panel.
//
// NOTE ON COORDINATES: the 2D demo works in screen space (pixels, +Y downward)
// for simplicity. The +Y-up convention in engine/core/math.hpp is for the future
// 3D world; the two don't clash because nothing here is 3D yet.

namespace {

// Map a world position to a screen pixel, scaling the fixed play field to fill
// the current window. ImGui's background draw list uses these screen coords.
eng::Vec2 world_to_screen(eng::Vec2 world, const ImGuiViewport& vp) {
  const eng::Vec2 scale{vp.Size.x / eng::sim::kFieldWidth, vp.Size.y / eng::sim::kFieldHeight};
  return eng::Vec2{vp.Pos.x + world.x * scale.x, vp.Pos.y + world.y * scale.y};
}

// Draw every renderable entity as a filled circle, interpolated between its last
// two tick positions by `alpha` so motion looks smooth despite the fixed 60 Hz
// step (see simulation.hpp).
void draw_entities(const eng::sim::World& world, ImDrawList* dl, float alpha) {
  const ImGuiViewport& vp = *ImGui::GetMainViewport();
  auto view = world.registry()
                  .view<const eng::sim::Transform, const eng::sim::PrevTransform,
                        const eng::sim::RenderDot>();
  for (const entt::entity e : view) {
    const eng::Vec2 curr = view.get<const eng::sim::Transform>(e).position;
    const eng::Vec2 prev = view.get<const eng::sim::PrevTransform>(e).position;

    // Skip interpolation when the entity wrapped around a field edge this tick
    // (prev and curr are far apart) — otherwise it would streak across the
    // screen. A real camera-relative renderer wouldn't have this quirk.
    const eng::Vec2 blended = (std::abs(curr.x - prev.x) > eng::sim::kFieldWidth * 0.5f ||
                               std::abs(curr.y - prev.y) > eng::sim::kFieldHeight * 0.5f)
                                  ? curr
                                  : glm::mix(prev, curr, alpha);

    const eng::sim::RenderDot& dot = view.get<const eng::sim::RenderDot>(e);
    const eng::Vec2 p = world_to_screen(blended, vp);

    // The dot's colour is layered from the base outward: personality tints it, health dims it,
    // starvation greys it, venom greens it, a fresh blow flashes it white. Each is a renderer-only
    // cue the sim never reads, and each is optional (most entities have none), so each is a try_get
    // guard on `rgb`.
    eng::Vec3 rgb = dot.color;

    // Quality sheen: a FINER grounded item glints brighter, so a fine drop (a tough kill's loot,
    // quality > 1.0) catches the eye against a baseline one and you can SEE it's worth the grab —
    // the visible half of per-source loot quality. Weapon and Armour components live ONLY on
    // grounded pickups (equip CONSUMES the entity), so a try_get hit here IS a lootable item on the
    // ground; each carries a `quality`. Baseline 1.0 -> 1.0, so ordinary gear (and the venom fang)
    // is unchanged. A steady INTRINSIC cue, so it sits at the base before the health/venom/flash
    // overlays (which grounded gear never has anyway — no Stats/Poisoned). Presentation-only: the
    // sim never reads colour.
    if (const auto* wpn = world.registry().try_get<eng::sim::Weapon>(e)) {
      rgb *= eng::sim::quality_sheen(wpn->quality);
    } else if (const auto* arm = world.registry().try_get<eng::sim::Armour>(e)) {
      rgb *= eng::sim::quality_sheen(arm->quality);
    }

    // Personality tint: colour a character by its BRAVERY (warm = brave, cool = coward) so the
    // build_scene spread reads on screen — and so does the PLAYER's own arc, now that it carries a
    // Personality its deeds drift (a battle-hardened player's blue dot warms; creatures still carry
    // none). Neutral bravery 0 is the identity, so a fresh player renders unchanged until a deed
    // moves it. The try_get guard alone scopes it — no special-casing needed.
    if (const auto* pers = world.registry().try_get<eng::sim::Personality>(e)) {
      rgb *= eng::sim::personality_tint(pers->bravery);
    }

    // Wounded dimming: darken the dot in proportion to its health, so the accumulated toll of a
    // fight reads at a glance (the steady twin of the hit-flash blink). Optional Stats (motes/
    // pickups/weapons have none) — same try_get the debug panel uses. Kept in `brightness` so the
    // poison tint below can dim its green by the same factor (see there).
    float brightness = 1.0f;
    const auto* st =
        world.registry().try_get<eng::sim::Stats>(e);  // reused by the pallor cue below
    if (st != nullptr) {
      brightness = eng::sim::wounded_brightness(st->health.current, st->health.max);
    }
    rgb *= brightness;

    // Need pallor: a starving or parched colonist wastes to a sallow grey, by EXACTLY how much the
    // Need debuff saps its blows (need_pallor is derived from need_efficiency — one source of
    // truth, so the look and the combat penalty can never diverge). Full needs -> 0 -> an unchanged
    // draw (bit-identical for the well-fed). A STEADY-state cue, so it sits UNDER the acute poison/
    // flash overlays below — a poisoned starving dot still greens (poison mixes on top,
    // self-capped) and a fresh blow still blinks white, rather than the grey erasing them. Scaled
    // by the wounded `brightness` so it never re-brightens a near-dead dot. Reuses the `st` fetched
    // for the wounded cue.
    if (st != nullptr) {
      rgb = glm::mix(rgb, eng::Vec3{0.5f, 0.48f, 0.4f} * brightness, eng::sim::need_pallor(*st));
    }

    // Poison: a venomed dot glows an ACID green while the venom lasts, deeper the stronger the dose
    // (poison_tint_strength), so the poison you've spread — or taken — reads on the field. Bilious
    // yellow-green, distinct from the friendly NPC green so it reads on colonists (a prime venom
    // victim) as well as the blue player. A STATUS overlay over the personality hue AND the steady
    // pallor (active harm matters more than the trait or a slow need) but under the hit-flash, so a
    // fresh blow still blinks white. The target green is scaled by the same wounded `brightness`,
    // so poison NEVER re-brightens a dimmed, near-dead dot — the health cue survives under the
    // venom cue. Optional (most aren't poisoned).
    if (const auto* pois = world.registry().try_get<eng::sim::Poisoned>(e)) {
      rgb = glm::mix(rgb, eng::Vec3{0.55f, 0.9f, 0.1f} * brightness,
                     eng::sim::poison_tint_strength(pois->damage_per_second));
    }

    // Hit-flash: a freshly-struck entity blinks white and fades. remaining runs from
    // kHitFlashSeconds down to 0, so t is 1 on the blow and eases to 0 — mixing the dot
    // toward white by that much. Optional component (most entities have none), so try_get.
    if (const auto* flash = world.registry().try_get<eng::sim::HitFlash>(e)) {
      const float t = flash->remaining / eng::sim::kHitFlashSeconds;
      rgb = glm::mix(rgb, eng::Vec3{1.0f, 1.0f, 1.0f}, t);
    }
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4{rgb.r, rgb.g, rgb.b, 1.0f});

    // Standing as PRESENCE: a dot grows with positive STANDING (rescues, monster kills) and shrinks
    // with negative (striking your own) — so you can WATCH a colonist swell into a figure of repute
    // or dwindle into a shunned villain. Size is the cue colour and brightness leave free. Optional
    // ledger (only those who've done a deed carry one), so try_get; standing 0 leaves the authored
    // radius. Presentation-only — the sim never reads size.
    float radius = dot.radius;
    if (const auto* led = world.registry().try_get<eng::sim::BehaviorLedger>(e)) {
      radius *= eng::sim::renown_scale(eng::sim::standing(*led));
    }
    dl->AddCircleFilled(ImVec2{p.x, p.y}, radius, color);

    // A raised GUARD reads on the FIELD, not just the panel: a Blocking entity gets a steel-blue
    // RING around its dot (the same hue the panel's "GUARDING" text uses), so you can SEE who has
    // their guard up mid-fight — the field cue every other status already has (personality tint,
    // poison green, hit-flash white, standing size), now for the stance too. Presentation-only:
    // reads Blocking, never sets it; optional (only the player guards today), so all_of guards it.
    if (world.registry().all_of<eng::sim::Blocking>(e)) {
      dl->AddCircle(ImVec2{p.x, p.y}, radius + 3.0f, IM_COL32(150, 205, 255, 255), 0, 2.0f);
    }

    // A DOWNED entity reads on the FIELD, not just the panel: a crumpled, helpless body an ally can
    // still haul up. It gets a pale beacon RING — a "someone's down here, come rescue them" halo,
    // the field twin of the panel's "Downed!" callout and the cue that lets you SEE who to run to
    // (the rescue-seek rung, the graded rescue reach, the mutual bond a save forms). The dot itself
    // is dimmed to an ember — a downed body is at 0 health, which wounded_brightness darkens to its
    // kWoundedFloor — so a pale halo around a dim husk is an unmistakable read. Distinct from the
    // steel-blue guard ring in hue, and the two never coincide (handle_deaths strips Blocking the
    // instant it downs you). Presentation-only: reads Downed, never sets it; optional, so all_of
    // guards it. Only players go Downed today (NPCs permadie), so this shows on a fallen player —
    // and, in co-op, on any teammate you might revive.
    if (world.registry().all_of<eng::sim::Downed>(e)) {
      dl->AddCircle(ImVec2{p.x, p.y}, radius + 4.0f, IM_COL32(230, 230, 235, 220), 0, 2.0f);
    }

    // A cast SHIELD reads on the FIELD: a Shielded entity (a barrier up from shield_spell) gets a
    // bright arcane-cyan RING, so you can SEE who's warded and time your aggression — the field cue
    // the other statuses all have (guard steel-blue, poison green, downed beacon). Drawn at
    // radius+5, OUTSIDE the guard (+3) and downed (+4) rings, since a shield can coincide with
    // either (you can cast a ward then raise a guard, or be warded as you're downed) — three
    // concentric rings stay legible. Presentation-only: reads Shielded, never sets it; optional
    // (only the player shields today), so all_of guards it.
    if (world.registry().all_of<eng::sim::Shielded>(e)) {
      dl->AddCircle(ImVec2{p.x, p.y}, radius + 5.0f, IM_COL32(90, 235, 220, 235), 0, 2.0f);
    }
  }
}

// The debug panel: live simulation state and controls. This is the M2 ImGui
// debug layer in embryo — the inspector you'll extend as systems are added.
void draw_debug_panel(const eng::sim::World& world, bool& paused) {
  ImGui::SetNextWindowSize(ImVec2{320, 0}, ImGuiCond_FirstUseEver);
  ImGui::Begin("Engine — debug");

  ImGui::Text("%.1f FPS", static_cast<double>(ImGui::GetIO().Framerate));
  ImGui::Text("tick: %llu", static_cast<unsigned long long>(world.tick()));
  // On a CONST registry, storage<T>() returns a pointer (the storage may not
  // exist yet), so null-check before reading its size.
  const auto* transforms = world.registry().storage<eng::sim::Transform>();
  ImGui::Text("entities: %d", transforms ? static_cast<int>(transforms->size()) : 0);
  // Watch this fall as NPCs wander into motes and die — permadeath, so it only
  // ever goes down.
  const auto* npcs = world.registry().storage<eng::sim::Npc>();
  ImGui::Text("NPCs alive: %d", npcs ? static_cast<int>(npcs->size()) : 0);
  // The red creatures hunting you — wear them down with J; a higher Strength kills faster.
  const auto* enemies = world.registry().storage<eng::sim::Enemy>();
  ImGui::Text("creatures: %d", enemies ? static_cast<int>(enemies->size()) : 0);

  const entt::entity player = world.player();
  const eng::Vec2 pos = world.registry().get<eng::sim::Transform>(player).position;
  ImGui::Text("player: (%.0f, %.0f)", static_cast<double>(pos.x), static_cast<double>(pos.y));

  // Read the player's Stats. try_get returns null if the entity has no Stats, so
  // this stays safe for entities (like the motes) that were never given any.
  // Downed: crumpled at 0 HP, counting down to an unrescued respawn (an ally who reaches
  // you revives sooner). Called out above the bars so the helpless state is unmistakable.
  if (const eng::sim::Downed* down = world.registry().try_get<eng::sim::Downed>(player)) {
    ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.4f, 1.0f}, "DOWNED — %.1fs (an ally can revive you)",
                       static_cast<double>(down->timer));
  }
  if (world.registry().all_of<eng::sim::Blocking>(player)) {
    ImGui::TextColored(ImVec4{0.6f, 0.8f, 1.0f, 1.0f},
                       "GUARDING (blows softened, movement slowed)");
  }
  if (const eng::sim::Stats* stats = world.registry().try_get<eng::sim::Stats>(player)) {
    const eng::sim::Vital& h = stats->health;
    ImGui::Text("health: %.0f / %.0f", static_cast<double>(h.current), static_cast<double>(h.max));
    ImGui::ProgressBar(h.current / h.max);
    const eng::sim::Vital& s = stats->stamina;
    ImGui::Text("stamina: %.0f / %.0f", static_cast<double>(s.current), static_cast<double>(s.max));
    ImGui::ProgressBar(s.current / s.max);
    const eng::sim::Vital& hu = stats->hunger;
    ImGui::Text("hunger: %.0f / %.0f", static_cast<double>(hu.current),
                static_cast<double>(hu.max));
    ImGui::ProgressBar(hu.current / hu.max);  // falls over time; eat orbs to refill, or starve
    const eng::sim::Vital& wa = stats->water;
    ImGui::Text("water: %.0f / %.0f", static_cast<double>(wa.current), static_cast<double>(wa.max));
    ImGui::ProgressBar(wa.current / wa.max);  // falls over time; drink at the pond, or dehydrate
    const eng::sim::Vital& fa = stats->fatigue;
    ImGui::Text("fatigue: %.0f / %.0f", static_cast<double>(fa.current),
                static_cast<double>(fa.max));
    ImGui::ProgressBar(fa.current /
                       fa.max);  // falls while exerting (worst sprinting), mends at rest
    const eng::sim::Vital& mp = stats->mp;
    ImGui::Text("mana: %.0f / %.0f", static_cast<double>(mp.current), static_cast<double>(mp.max));
    ImGui::ProgressBar(mp.current / mp.max);  // spent by casting (C: bolt, H: mend), regens at rest
    const eng::sim::Vital& wm = stats->warmth;
    ImGui::Text("warmth: %.0f / %.0f", static_cast<double>(wm.current),
                static_cast<double>(wm.max));
    ImGui::ProgressBar(wm.current /
                       wm.max);  // drains in a cold zone, refills at the hearth; 0 freezes
  }

  // Progression: the attribute the player has grown, and progress toward the next
  // conditioning level. Move around and watch endurance climb, then the health and
  // stamina bars above lengthen as the bigger pools take effect.
  if (const eng::sim::Attributes* attr = world.registry().try_get<eng::sim::Attributes>(player)) {
    ImGui::Text("endurance: %d", attr->endurance.level - 1);  // level 1 = 0 bonus
    ImGui::Text("strength: %d",
                attr->strength.level - 1);  // from attacking; longer reach + harder hits
    ImGui::Text("wisdom: %d",
                attr->wisdom.level - 1);  // from foraging; more forage yield + wider danger sense
    ImGui::Text(
        "charisma: %d",
        attr->charisma.level - 1);  // from leading kills; more devotion from those who watch
  }
  // Gear: a wielded weapon and/or worn armour fold into Equipped, each with its own bane.
  // Show only the slots that are actually filled so the tradeoffs are legible (and an
  // armour-only wearer doesn't read as "+0 STR").
  if (const eng::sim::Equipped* eq = world.registry().try_get<eng::sim::Equipped>(player)) {
    // ...each with its remaining DURABILITY (hits/blows before it shatters and the slot clears), so
    // you can see a blade or plate wearing toward the end and decide whether to keep fighting or go
    // scavenge a fresh one — the temporal half of the tradeoff, made legible.
    if (eq->strength_bonus != 0 || eq->move_penalty != 0.0f) {
      ImGui::TextColored(ImVec4{0.8f, 0.85f, 1.0f, 1.0f},
                         "wielding: +%d STR, -%.0f%% speed (%.0f hits left)", eq->strength_bonus,
                         static_cast<double>(eq->move_penalty * 100.0f),
                         static_cast<double>(eq->weapon_durability));
    }
    if (eq->defence_bonus != 0.0f || eq->stamina_regen_penalty != 0.0f) {
      ImGui::TextColored(ImVec4{0.9f, 0.7f, 0.4f, 1.0f},
                         "armoured: +%.0f DEF, -%.0f%% stamina regen (%.0f blows left)",
                         static_cast<double>(eq->defence_bonus),
                         static_cast<double>(eq->stamina_regen_penalty * 100.0f),
                         static_cast<double>(eq->armour_durability));
    }
  }
  if (const eng::sim::CharacterLevel* cl =
          world.registry().try_get<eng::sim::CharacterLevel>(player)) {
    ImGui::Text("character level: %d", cl->level);  // the slow "veteran" multiplier on earned stats
  }
  // Morality: the player's standing (climbs with Valor kills / Charity rescues; goes negative once
  // villain deeds land) and its derived title. No BehaviorLedger until the first deed, so try_get
  // -> a neutral 0 / "Unproven". Labelled "standing" (the signed scalar), not "renown" (its
  // positive half, which the dot-size shows).
  {
    const eng::sim::BehaviorLedger* led =
        world.registry().try_get<eng::sim::BehaviorLedger>(player);
    const std::int32_t standing_value = led != nullptr ? eng::sim::standing(*led) : 0;
    ImGui::Text("standing: %d (%s)", static_cast<int>(standing_value),
                eng::sim::standing_title(standing_value));
    // Epithet: what you're KNOWN FOR — your most-repeated deed once it crosses kEpithetAt (the
    // Slayer / Savior / Butcher ...). The third derived-recognition axis beside standing and build;
    // nullptr = no line until one deed kind is repeated enough to earn a name, so a fresh or
    // never-acting player shows nothing.
    if (led != nullptr) {
      if (const char* epithet = eng::sim::deed_epithet(*led)) ImGui::Text("known as: %s", epithet);
    }
  }
  // Build: which trained Attribute dominates names what KIND of fighter you are (Warrior /
  // Skirmisher / Bulwark / Chancer) — the "from build" half of the derived-recognition titles,
  // the twin of the deed-derived standing title above. "Greenhorn" until you train one.
  if (const eng::sim::Attributes* attrs = world.registry().try_get<eng::sim::Attributes>(player)) {
    ImGui::Text("build: %s", eng::sim::build_title(*attrs));
  }
  // Closest bond: the strongest tie the player holds (its highest-affinity valid edge), named by
  // the derived `bond_tier` band. The player builds ties by fighting ALONGSIDE colonists
  // (camaraderie bonds a witness to the killer), so this reads out the co-op camaraderie earned in
  // the field. No Relationships until a bond forms; a recycled edge target is skipped (ids reuse —
  // gate on valid).
  if (const auto* rel = world.registry().try_get<eng::sim::Relationships>(player)) {
    std::int8_t best = 0;
    for (const eng::sim::Relation& edge : rel->edges) {
      if (world.registry().valid(edge.other) && edge.affinity > best) best = edge.affinity;
    }
    ImGui::Text("closest bond: %s", eng::sim::bond_tier(best));
  }
  // Allies: how many colonists have bonded TO the player (the INCOMING mirror of the closest bond
  // above) — the camaraderie you've earned fighting beside the colony, and exactly the allies the
  // steer_npcs defend rung will send rushing to your side when a creature closes in. Shown even at
  // 0 (you've won nobody over yet). A cheap whole-registry scan.
  ImGui::Text("allies: %d", eng::sim::allies_of(world.registry(), player));
  if (const eng::sim::Skills* skills = world.registry().try_get<eng::sim::Skills>(player)) {
    // Show one learned skill's level + progress bar. Toughness only appears once the
    // player has taken a hit (it isn't in `owned` until then), so guard on find().
    const auto show_skill = [&](const char* label, eng::sim::SkillId id) {
      const eng::sim::Skill* s = skills->find(id);
      if (s == nullptr) return;
      ImGui::Text("%s: lvl %d", label, s->level);
      const eng::Fixed threshold =
          eng::Fixed::from_int(static_cast<std::int32_t>(eng::sim::xp_to_next(s->level)));
      ImGui::ProgressBar(static_cast<float>((s->xp / threshold).to_double()));
    };
    show_skill("conditioning", eng::sim::SkillId::Conditioning);
    show_skill("toughness", eng::sim::SkillId::Toughness);
    show_skill("striking", eng::sim::SkillId::Striking);
    show_skill("recovery", eng::sim::SkillId::Recovery);
    show_skill("evasion", eng::sim::SkillId::Evasion);  // appears once a creature has swung at you
    show_skill("scavenging", eng::sim::SkillId::Scavenging);  // appears once you've grabbed an orb
  }

  ImGui::Checkbox("pause simulation", &paused);

  ImGui::Separator();
  ImGui::TextWrapped(
      "WASD / arrows: move — and dodge, the drifting motes hurt and vanish on "
      "contact. Moving drains stamina; run it dry and you slow to a crawl until "
      "you rest. You also get HUNGRY over time (faster while moving) — let the hunger "
      "bar hit empty and you start to starve (it eats your health until you die), so eat "
      "to keep it up. The green dots are NPCs: they flee motes they sense, forage for food "
      "orbs when they get hungry (they get hungry and starve just like you — so they may "
      "grab a health orb before you do), take the same contact damage you do, and when they "
      "die they're gone for good (permadeath). YOU don't die outright: at 0 health you go "
      "DOWNED — crumpled where you fell, helpless (you can't move), for a few seconds. A "
      "nearby colonist who sees you fall will RUN OVER and haul you up on the spot; if none "
      "reaches you in time, you respawn back at the centre. Creatures hunt the nearest "
      "person — you OR an NPC — "
      "and hit back: RED brutes are slow, tanky, and hit hard, while ORANGE swarmers are "
      "fast and fragile but come in numbers. So NPCs and creatures actually war; watch "
      "them skirmish (and sometimes an NPC fall). Fresh creatures keep arriving — and "
      "fresh colonists wander in over time to replace the fallen, so the war sustains "
      "itself. "
      "Space: spawn a mote. H: take 15 damage. E: wear the nearest dropped gear — a steel "
      "weapon (harder hits, slower), a GREEN venom blade (weaker hits that POISON the foe, but "
      "nimble), or bronze armour (more defence, slower stamina regen), each its own slot; "
      "Q: drop the weapon again to shed the heft and move free. "
      "G: harvest a ripe food plot in reach into a MEAL at your feet — it fills more hunger "
      "than grazing the patch raw (prepared food goes further). "
      "T: plant a new crop where you stand — it grows over time, then G harvests it (plant a "
      "garden, don't just find one). "
      "J: strike the nearest mote or creature in reach — motes pop in one hit; a "
      "brute takes several, fewer as your Strength climbs (it hits harder), while "
      "your VIT softens its blows. Hold CTRL while you swing to POWER the blow — it hits ~1.75x "
      "harder but costs far more stamina (fell a brute in fewer swings, at the price of winding "
      "you faster). F: THROW at the nearest creature far out of melee reach — a "
      "modest but reliable chip that SPENDS STAMINA (soften an approaching swarm, but you can't "
      "kite forever); it trains Throwing -> Dexterity. C: CAST a magic bolt at the nearest "
      "creature "
      "in range — the ranged twin that spends MANA instead of stamina and scales with Intellect; "
      "you LEARN it by walking over a SPELLBOOK on the field, and casting trains Spellcasting -> "
      "Intellect. H: MEND the nearest wounded ally in range — the SUPPORT twin that spends the "
      "same "
      "mana to restore a friend's health and scales with Wisdom (trains Healing -> Wisdom). "
      "Standing "
      "to trade blows "
      "slowly trains "
      "Dexterity too, "
      "so before long some hits are dodged outright. Swing J at a peaceful COLONIST with no "
      "enemy in reach and you'll cut them down instead — a CRUEL act that sinks your standing "
      "(watch it and your title turn in the HUD); cross into villainy and colonists FLEE you on "
      "sight, while a renowned HERO draws idle colonists to rally around them. HOLD K to raise a "
      "GUARD: blows land far "
      "softer, but you move at a crawl (plant and tank, or move and dodge — not both). A "
      "worn-down creature ENRAGES and hits harder — but your blows also EXECUTE it (extra damage "
      "once it's nearly dead), so finish it fast (or guard through it). "
      "Careful of ORANGE swarmers: they're "
      "slippery and dodge some of YOUR strikes too. And VIOLET spitters hang back and pelt you "
      "with "
      "a homing spit from out of melee reach — close on them or THROW back to shut them down. "
      "Hitting back trains Striking → Strength "
      "(even a whiff a swarmer dodges). A "
      "slain "
      "creature drops loot: a SWARMER leaves a cyan health orb — walk over it to heal, feed "
      "(it refills hunger), raise your max HP a little, AND train Scavenging → Luck, which "
      "earns you critical hits (a doubled blow) on future strikes. A BRUTE instead drops a "
      "steel-grey WEAPON: press E near it to wield it for +Strength (longer reach, harder "
      "hits) — but it's heavy, so you move slower while armed. A real choice: killing power "
      "vs the speed you kite with. Your "
      "keypresses become Commands; the "
      "fleeing, chasing, motes, loot, and deaths are all systems on the server.");
  ImGui::End();
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
  eng::log::Init();

  auto renderer = eng::gpu::Renderer::create("Engine — walking skeleton", 1280, 720);
  if (renderer == nullptr) {
    // No window/GPU (e.g. a headless machine or CI). The engine's simulation
    // core still works fully — that's what the tests exercise — so this isn't a
    // failure, there's just nothing to display here.
    eng::log::info("no display; run the tests to exercise the headless engine core");
    return 0;
  }

  // Single-player wiring: this client sends input to a server that owns the
  // world, over a loopback transport. The exact shape multiplayer will use.
  eng::net::LoopbackTransport transport;
  eng::net::Server server(transport);
  eng::sim::FixedTimestep timestep(eng::sim::kSecondsPerTick);

  // Optional frame cap for automated smoke runs: ENG_MAX_FRAMES=N renders N
  // frames then exits cleanly. CI sets it so this interactive client returns
  // instead of looping forever on a machine with no one to close the window. A
  // normal run leaves it unset (-1) and loops until the window is closed.
  const char* frames_env = std::getenv("ENG_MAX_FRAMES");
  const long max_frames = frames_env != nullptr ? std::atol(frames_env) : -1;
  long frames_rendered = 0;

  Uint64 prev_counter = SDL_GetPerformanceCounter();
  const double counter_freq = static_cast<double>(SDL_GetPerformanceFrequency());
  bool paused = false;
  bool space_was_down = false;
  bool hurt_was_down = false;
  bool attack_was_down = false;
  bool throw_was_down = false;
  bool equip_was_down = false;
  bool drop_was_down = false;
  bool harvest_was_down = false;
  bool plant_was_down = false;
  bool cast_was_down = false;
  bool heal_was_down = false;
  bool shield_was_down = false;

  while (renderer->poll_events()) {
    // --- real elapsed time since the last frame ---
    const Uint64 now_counter = SDL_GetPerformanceCounter();
    const double frame_seconds = static_cast<double>(now_counter - prev_counter) / counter_freq;
    prev_counter = now_counter;

    // --- input as data: keyboard -> a movement direction ---
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool imgui_wants_keys = ImGui::GetIO().WantCaptureKeyboard;
    eng::Vec2 dir{0.0f, 0.0f};
    if (!imgui_wants_keys) {
      if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) dir.x -= 1.0f;
      if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dir.x += 1.0f;
      if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) dir.y -= 1.0f;
      if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) dir.y += 1.0f;
    }

    // Space, edge-triggered, spawns a mote at the player's position. Edge-detect
    // on the RAW physical key and gate only the ACTION on ImGui focus — if we
    // folded focus into the remembered state, ImGui gaining then losing the
    // keyboard while Space stays held would look like a fresh press and spawn twice.
    const bool space_raw = keys[SDL_SCANCODE_SPACE];
    if (space_raw && !space_was_down && !imgui_wants_keys) {
      const entt::entity player = server.world().player();
      const eng::Vec2 pos = server.world().registry().get<eng::sim::Transform>(player).position;
      transport.send(eng::net::Message{eng::sim::spawn_mote(pos)});
    }
    space_was_down = space_raw;

    // H, edge-triggered, hurts the player by 15. It travels the same funnel as
    // everything else — input becomes a DamagePlayer command the server applies —
    // so you can watch the bar drop, then the regen system heal it back up.
    const bool hurt_raw = keys[SDL_SCANCODE_H];
    if (hurt_raw && !hurt_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::damage_player(eng::sim::kLocalPlayer, 15.0f)});
    }
    hurt_was_down = hurt_raw;

    // J, edge-triggered, swings at the nearest mote in reach — hitting back. The
    // server picks the target from the player's own position (see the Attack
    // command), destroys it, and trains Striking -> Strength, which lengthens reach.
    const bool attack_raw = keys[SDL_SCANCODE_J];
    if (attack_raw && !attack_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::attack(eng::sim::kLocalPlayer)});
    }
    attack_was_down = attack_raw;

    // F, edge-triggered, HURLS at the nearest hostile at range (perform_throw) — the player's
    // ranged option. Costs stamina, so it's for softening an approaching swarm, not a melee
    // replacement.
    const bool throw_raw = keys[SDL_SCANCODE_F];
    if (throw_raw && !throw_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::hurl(eng::sim::kLocalPlayer)});
    }
    throw_was_down = throw_raw;

    // E, edge-triggered, wields the nearest dropped weapon in reach (the steel-grey dots a
    // slain brute leaves). The server folds its mods into an Equipped cache — same funnel.
    const bool equip_raw = keys[SDL_SCANCODE_E];
    if (equip_raw && !equip_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::equip(eng::sim::kLocalPlayer)});
    }
    equip_was_down = equip_raw;

    // Q, edge-triggered, drops the wielded weapon at your feet — the inverse of E. Shed the
    // heft to sprint clear of a swarm, then circle back and re-wield it (or let a colonist grab
    // it). Same funnel: input becomes a Drop command the server applies.
    const bool drop_raw = keys[SDL_SCANCODE_Q];
    if (drop_raw && !drop_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::drop(eng::sim::kLocalPlayer)});
    }
    drop_was_down = drop_raw;

    // G, edge-triggered, GATHERS a ripe food plot in reach into a meal at your feet — the food
    // economy: a prepared meal fills more hunger than grazing the same patch raw. Same funnel:
    // input becomes a Harvest command the server applies.
    const bool harvest_raw = keys[SDL_SCANCODE_G];
    if (harvest_raw && !harvest_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::harvest(eng::sim::kLocalPlayer)});
    }
    harvest_was_down = harvest_raw;

    // T, edge-triggered, PLANTS a crop seedling at your feet — the front of the food chain. It
    // grows over time (the same regrow that recovers a grazed patch) and once ripe you HARVEST it
    // (G). Same funnel: input becomes a Plant command the server applies.
    const bool plant_raw = keys[SDL_SCANCODE_T];
    if (plant_raw && !plant_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::plant(eng::sim::kLocalPlayer)});
    }
    plant_was_down = plant_raw;

    // C, edge-triggered, CASTS a magic bolt at the nearest hostile in range — the player's first
    // spell. It spends MANA (not stamina) and only works because the player has LEARNED it
    // (Spellcasting); an empty mana bar or no target fizzles. Same funnel: a Cast command.
    const bool cast_raw = keys[SDL_SCANCODE_C];
    if (cast_raw && !cast_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::cast(eng::sim::kLocalPlayer)});
    }
    cast_was_down = cast_raw;

    // H, edge-triggered, casts a MEND at the nearest wounded ally in range — the support twin of C.
    // Same mana bar and learned Spellcasting gate as the bolt; no wounded ally (or no mana)
    // fizzles.
    const bool heal_raw = keys[SDL_SCANCODE_H];
    if (heal_raw && !heal_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::cast_heal(eng::sim::kLocalPlayer)});
    }
    heal_was_down = heal_raw;

    // B, edge-triggered, casts a SHIELD on yourself — the defensive third of the trio (bolt C, mend
    // H, barrier B). Same mana bar and learned Spellcasting gate; it raises a timed Shielded that
    // soaks part of each creature blow, so you cast it BEFORE wading in. An empty bar fizzles.
    const bool shield_raw = keys[SDL_SCANCODE_B];
    if (shield_raw && !shield_was_down && !imgui_wants_keys) {
      transport.send(eng::net::Message{eng::sim::cast_shield(eng::sim::kLocalPlayer)});
    }
    shield_was_down = shield_raw;

    // K, HELD (not edge-triggered), raises a GUARD: incoming creature blows are softened but you
    // move slower. A held stance rather than a one-shot action, so it rides the per-tick MovePlayer
    // command (below) rather than an edge event — it lasts exactly as long as the key is down.
    const bool guard = !imgui_wants_keys && keys[SDL_SCANCODE_K];
    // Hold SHIFT to SPRINT — faster, but it burns stamina (see Sprinting), a short dash that ends
    // in the exhaustion crawl. Also a held stance on the per-tick MovePlayer command; guard wins if
    // both.
    const bool sprint =
        !imgui_wants_keys && (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]);
    // Hold CTRL to POWER your swings — each hits harder but costs more stamina (see PowerAttack),
    // the offensive twin of sprint. Also a held stance on the per-tick MovePlayer command;
    // orthogonal to guard/sprint (it shapes the ATTACK, not the movement), so it stacks with
    // either.
    const bool power = !imgui_wants_keys && (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]);

    // --- advance the simulation in fixed steps ---
    const int steps = paused ? 0 : timestep.advance(frame_seconds);
    for (int i = 0; i < steps; ++i) {
      // One input Command per tick — the client's only way to affect the world.
      transport.send(eng::net::Message{
          eng::sim::move_player(eng::sim::kLocalPlayer, dir, guard, sprint, power)});
      server.tick();
    }

    // --- render: debug panel + interpolated entities ---
    renderer->begin_frame();
    draw_debug_panel(server.world(), paused);
    draw_entities(server.world(), renderer->background_draw_list(),
                  static_cast<float>(timestep.alpha()));
    renderer->end_frame();

    if (max_frames >= 0 && ++frames_rendered >= max_frames) break;
  }

  eng::log::info("shutting down cleanly");
  return 0;
}
