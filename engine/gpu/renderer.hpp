#pragma once

#include <memory>

struct SDL_Window;
struct SDL_GPUDevice;
struct ImDrawList;

// The renderer: the ONE place raw graphics-API calls are allowed to live
// (code-design-rules rule 6: "quarantine replaceable libs — GPU types never
// leave their module"). Everything above it deals in entities and positions,
// never in SDL_GPU handles.
//
// For the walking skeleton the renderer is deliberately thin: it owns the
// window, the SDL_GPU device, and the Dear ImGui debug layer, and each frame it
// clears the screen and draws whatever ImGui built (our debug panels plus the
// entities we paint onto a background draw list). Real mesh/shader rendering is
// roadmap M1 — it slots in as "draws inside the render pass, before ImGui".
//
// It is an RAII object: create() acquires everything, the destructor releases it
// in the correct order. No manual cleanup, no leaks on an early return.

namespace eng::gpu {

class Renderer {
 public:
  // Build a window + GPU device + ImGui. Returns nullptr (having logged why) if
  // no display/GPU is available — e.g. a headless CI machine. Callers treat
  // nullptr as "there's no screen here, run without one or exit".
  static std::unique_ptr<Renderer> create(const char* title, int width, int height);

  ~Renderer();

  // Owns unique GPU resources, so it can't be copied or moved — it lives inside
  // a unique_ptr instead.
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  Renderer(Renderer&&) = delete;
  Renderer& operator=(Renderer&&) = delete;

  // Pump OS + input events into ImGui. Returns false when the user closed the
  // window (quit).
  bool poll_events();

  // Start a frame's UI. Call ImGui:: functions, and draw onto
  // background_draw_list(), between begin_frame() and end_frame().
  void begin_frame();

  // Finish the frame: render ImGui, clear the screen, and present it.
  void end_frame();

  // The draw list for painting world entities, behind the ImGui windows. Uses
  // screen (pixel) coordinates.
  ImDrawList* background_draw_list() const;

  SDL_Window* window() const { return window_; }

 private:
  Renderer() = default;

  SDL_Window* window_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
};

}  // namespace eng::gpu
