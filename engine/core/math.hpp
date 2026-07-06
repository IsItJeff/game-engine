#pragma once

// Math conventions for the whole engine (ADR: "GLM + one conventions header").
//
// We use GLM as our vector/matrix library rather than hand-rolling one — it is
// the de-facto standard, header-only, and reads like GLSL so CPU and shader
// code look alike. The ONE thing that matters is that every part of the engine
// agrees on conventions (handedness, depth range, up axis). Convention drift is
// the classic "why is everything mirrored / inside out" 3am bug, so we pin it
// here, once, and include THIS header rather than <glm/glm.hpp> directly.
//
// The skeleton is 2D (a top-down field of dots), so for now we mostly use
// glm::vec2. The 3D conventions below are declared up front so they don't have
// to be retrofitted when the renderer grows to 3D (roadmap M1).

// Depth range 0..1 (what modern GPU APIs — including SDL_GPU/Metal/D3D — expect)
// rather than OpenGL's -1..1. Must be set before including glm.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace eng {

// Short aliases so gameplay code reads cleanly.
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;

// Engine conventions, documented so they are impossible to forget:
//   - Right-handed coordinate system.
//   - +Y is up (in 2D, +Y is "up" on screen; the renderer flips as needed).
//   - Clip-space depth is 0 (near) .. 1 (far), set by the macro above.
// When the renderer reaches 3D these become load-bearing; keep them here.

}  // namespace eng
