#include <SDL3/SDL.h>
#include <imgui.h>

#include <cmath>

#include "engine/core/log.hpp"
#include "engine/core/math.hpp"
#include "engine/gpu/renderer.hpp"
#include "engine/net/loopback.hpp"
#include "engine/net/server.hpp"
#include "engine/sim/components.hpp"
#include "engine/sim/simulation.hpp"
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

  const entt::entity player = world.player();
  const eng::Vec2 pos = world.registry().get<eng::sim::Transform>(player).position;
  ImGui::Text("player: (%.0f, %.0f)", static_cast<double>(pos.x), static_cast<double>(pos.y));

  ImGui::Checkbox("pause simulation", &paused);

  ImGui::Separator();
  ImGui::TextWrapped(
      "WASD / arrows: move the blue dot. Space: spawn a mote. "
      "Everything you press becomes a Command sent to the server.");
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

  Uint64 prev_counter = SDL_GetPerformanceCounter();
  const double counter_freq = static_cast<double>(SDL_GetPerformanceFrequency());
  bool paused = false;
  bool space_was_down = false;

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

    // Space, edge-triggered, spawns a mote at the player's position.
    const bool space_down = !imgui_wants_keys && keys[SDL_SCANCODE_SPACE];
    if (space_down && !space_was_down) {
      const entt::entity player = server.world().player();
      const eng::Vec2 pos = server.world().registry().get<eng::sim::Transform>(player).position;
      transport.send(eng::net::Message{eng::sim::spawn_mote(pos)});
    }
    space_was_down = space_down;

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
  }

  eng::log::info("shutting down cleanly");
  return 0;
}
