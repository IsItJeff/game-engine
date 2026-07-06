#include "engine/gpu/renderer.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include "engine/core/log.hpp"

// This file is the whole graphics backend. It's the most "C-API heavy" code in
// the engine because SDL_GPU is a C library; the comments walk through each step
// so it reads as a tutorial. The exact call sequence follows the SDL3 wiki.

namespace eng::gpu {

std::unique_ptr<Renderer> Renderer::create(const char* title, int width, int height) {
  // SDL's video subsystem: opens the connection to the OS windowing system. On a
  // headless machine this can still succeed but window/GPU creation below fails.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    log::error("SDL_Init(VIDEO) failed: {}", SDL_GetError());
    return nullptr;
  }

  // A plain resizable window. Do NOT pass SDL_WINDOW_METAL/OPENGL/VULKAN — the
  // GPU device owns the swapchain, so a regular window is what it wants.
  SDL_Window* window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
  if (window == nullptr) {
    log::error("SDL_CreateWindow failed (no display?): {}", SDL_GetError());
    SDL_Quit();
    return nullptr;
  }

  // The GPU device. We advertise which shader formats WE can supply; on macOS
  // that's MSL (Metal Shading Language). ImGui's backend ships its own shaders,
  // so we don't have to write any. debug_mode=true turns on validation.
  SDL_GPUDevice* device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL,
      /*debug_mode=*/true, /*name=*/nullptr);
  if (device == nullptr) {
    log::error("SDL_CreateGPUDevice failed (no GPU?): {}", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return nullptr;
  }

  // Wire the device's swapchain to our window: now the device can present to it.
  if (!SDL_ClaimWindowForGPUDevice(device, window)) {
    log::error("SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return nullptr;
  }

  // Dear ImGui: context, dark theme, then the two backends — one that feeds it
  // SDL events (platform) and one that draws it via SDL_GPU (renderer).
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplSDL3_InitForSDLGPU(window);

  ImGui_ImplSDLGPU3_InitInfo init{};
  init.Device = device;
  init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
  init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  ImGui_ImplSDLGPU3_Init(&init);

  log::info("renderer ready: {}x{}, GPU driver '{}'", width, height,
            SDL_GetGPUDeviceDriver(device));

  // Private constructor + friend-free assembly: build the object and hand back
  // ownership. unique_ptr means the caller can't forget to destroy it.
  auto r = std::unique_ptr<Renderer>(new Renderer());
  r->window_ = window;
  r->device_ = device;
  return r;
}

Renderer::~Renderer() {
  // Order matters: wait for the GPU to finish any in-flight work, THEN tear down
  // ImGui, THEN the device and window. Releasing a resource the GPU is still
  // using is undefined behaviour, which is why the wait comes first.
  SDL_WaitForGPUIdle(device_);
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_ReleaseWindowFromGPUDevice(device_, window_);
  SDL_DestroyGPUDevice(device_);
  SDL_DestroyWindow(window_);
  SDL_Quit();
}

bool Renderer::poll_events() {
  bool keep_running = true;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL3_ProcessEvent(&event);  // let ImGui see mouse/keyboard first
    if (event.type == SDL_EVENT_QUIT) {
      keep_running = false;
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        event.window.windowID == SDL_GetWindowID(window_)) {
      keep_running = false;
    }
  }
  return keep_running;
}

void Renderer::begin_frame() {
  // Tell both backends and ImGui a new frame is starting. After this, ImGui::
  // calls build the UI for this frame.
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
}

ImDrawList* Renderer::background_draw_list() const {
  return ImGui::GetBackgroundDrawList();
}

void Renderer::end_frame() {
  // 1. Finalize the UI into a list of draw commands.
  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();

  // 2. A command buffer records this frame's GPU work.
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);

  // 3. The swapchain texture is the image we draw into and then show. It can be
  //    null when the window is minimized — that's not an error, we just skip.
  SDL_GPUTexture* swapchain = nullptr;
  SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, nullptr, nullptr);

  if (swapchain != nullptr) {
    // Upload ImGui's vertex/index data. MUST happen before the render pass —
    // buffer copies can't occur inside one.
    ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

    // Describe the frame's single render pass: clear to a dark blue-grey, keep
    // the result so it can be presented.
    SDL_GPUColorTargetInfo color{};
    color.texture = swapchain;
    color.clear_color = SDL_FColor{0.09f, 0.09f, 0.12f, 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    // (Real scene geometry would draw here first, on the cleared background.)
    // ImGui shares this pass and draws on top — including our entity dots, which
    // we added to the background draw list.
    ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
    SDL_EndGPURenderPass(pass);
  }

  // 4. Submitting the command buffer is what presents the frame.
  SDL_SubmitGPUCommandBuffer(cmd);
}

}  // namespace eng::gpu
