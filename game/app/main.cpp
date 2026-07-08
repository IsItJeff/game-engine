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
    const ImU32 color =
        ImGui::ColorConvertFloat4ToU32(ImVec4{dot.color.r, dot.color.g, dot.color.b, 1.0f});
    dl->AddCircleFilled(ImVec2{p.x, p.y}, dot.radius, color);
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
  if (const eng::sim::Stats* stats = world.registry().try_get<eng::sim::Stats>(player)) {
    const eng::sim::Vital& h = stats->health;
    ImGui::Text("health: %.0f / %.0f", static_cast<double>(h.current), static_cast<double>(h.max));
    ImGui::ProgressBar(h.current / h.max);
    const eng::sim::Vital& s = stats->stamina;
    ImGui::Text("stamina: %.0f / %.0f", static_cast<double>(s.current), static_cast<double>(s.max));
    ImGui::ProgressBar(s.current / s.max);
  }

  // Progression: the attribute the player has grown, and progress toward the next
  // conditioning level. Move around and watch endurance climb, then the health and
  // stamina bars above lengthen as the bigger pools take effect.
  if (const eng::sim::Attributes* attr = world.registry().try_get<eng::sim::Attributes>(player)) {
    ImGui::Text("endurance: %d", attr->endurance.level - 1);  // level 1 = 0 bonus
    ImGui::Text("strength: %d",
                attr->strength.level - 1);  // from attacking; longer reach + harder hits
  }
  if (const eng::sim::CharacterLevel* cl =
          world.registry().try_get<eng::sim::CharacterLevel>(player)) {
    ImGui::Text("character level: %d", cl->level);  // the slow "veteran" multiplier on earned stats
  }
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
  }

  ImGui::Checkbox("pause simulation", &paused);

  ImGui::Separator();
  ImGui::TextWrapped(
      "WASD / arrows: move — and dodge, the drifting motes hurt and vanish on "
      "contact. Moving drains stamina; run it dry and you slow to a crawl until "
      "you rest. The green dots are NPCs: they now flee motes they sense, take the "
      "same contact damage you do, and when they die they're gone for good "
      "(permadeath) — you respawn. The RED dots are creatures: they hunt you and hit "
      "back, but have HP you can wear down, and fresh ones keep arriving (the NPCs "
      "pitch in and fight them too). Space: spawn a mote. H: take 15 damage. "
      "J: strike the nearest mote or creature in reach — motes pop in one hit; a "
      "creature takes several, fewer as your Strength climbs (it hits harder), while "
      "your VIT softens its blows. Hitting back trains Striking → Strength. Your "
      "keypresses become Commands; the fleeing, chasing, motes, and deaths are all "
      "systems on the server.");
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

    // --- advance the simulation in fixed steps ---
    const int steps = paused ? 0 : timestep.advance(frame_seconds);
    for (int i = 0; i < steps; ++i) {
      // One input Command per tick — the client's only way to affect the world.
      transport.send(eng::net::Message{eng::sim::move_player(eng::sim::kLocalPlayer, dir)});
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
