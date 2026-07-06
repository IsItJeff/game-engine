#pragma once

// The fixed-timestep accumulator (ADR: "fixed 60 Hz tick, render interpolates").
//
// THE PROBLEM: the simulation must advance in fixed, equal steps (so physics and
// networking are deterministic), but monitors refresh at all sorts of rates
// (60, 120, 144 Hz...) and frames take uneven amounts of real time. If we
// stepped the world once per rendered frame, the game would run faster on a
// faster monitor. Unacceptable.
//
// THE SOLUTION (Glenn Fiedler's "Fix Your Timestep"): keep a running
// accumulator of real elapsed time. Each frame, add the frame's real duration,
// then run as many fixed steps as fit. Whatever time is left over (less than one
// step) becomes an interpolation factor `alpha` in 0..1, telling the renderer
// how far between the last two ticks to draw.
//
//   accumulator += frame_time (clamped)
//   while accumulator >= dt:  step();  accumulator -= dt
//   alpha = accumulator / dt          // used to blend PrevTransform -> Transform
//
// THE CLAMP: if the app was paused (a breakpoint, the laptop slept), frame_time
// could be huge and we'd try to run thousands of catch-up steps — the "spiral of
// death". Clamping the frame time to a maximum caps how far behind we let the
// sim fall. It's better to briefly run in slow-motion than to freeze.
//
// This class is pure logic (no clock, no rendering) so it is fully unit-tested.
// The client feeds it real frame durations and calls World::step() the returned
// number of times.

namespace eng::sim {

class FixedTimestep {
 public:
  // seconds_per_tick: the fixed step length (e.g. 1/60). max_frame: the largest
  // real frame time we'll honour before clamping, to prevent the spiral of death.
  explicit FixedTimestep(double seconds_per_tick, double max_frame = 0.25)
      : dt_(seconds_per_tick), max_frame_(max_frame) {}

  // Feed the real time elapsed since the last call; returns how many fixed steps
  // to run right now (often 0 or 1, occasionally more).
  int advance(double frame_seconds) {
    if (frame_seconds > max_frame_) frame_seconds = max_frame_;  // spiral guard
    accumulator_ += frame_seconds;

    int steps = 0;
    while (accumulator_ >= dt_) {
      accumulator_ -= dt_;
      ++steps;
    }
    return steps;
  }

  // How far (0..1) we are between the last completed tick and the next one.
  // The renderer blends PrevTransform -> Transform by this amount.
  double alpha() const { return accumulator_ / dt_; }

 private:
  double dt_;
  double max_frame_;
  double accumulator_ = 0.0;
};

}  // namespace eng::sim
