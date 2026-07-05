#include <SDL3/SDL.h>

#include "engine/core/log.hpp"
#include "engine/core/version.hpp"

int main(int /*argc*/, char* /*argv*/[]) {
  eng::log::Init();
  eng::log::info("engine {} | SDL {}.{}.{}", eng::kVersionString, SDL_MAJOR_VERSION,
                 SDL_MINOR_VERSION, SDL_MICRO_VERSION);

  // Events-only init keeps this runnable on headless CI (no display needed).
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    eng::log::error("SDL_Init failed: {}", SDL_GetError());
    return 1;
  }
  eng::log::info("hello toolchain: SDL initialized and shut down cleanly");
  SDL_Quit();
  return 0;
}
