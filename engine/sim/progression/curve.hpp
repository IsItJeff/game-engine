#pragma once

#include "engine/core/fixed.hpp"

// The progression curve — "the one law: uncapped, ever-slower, never zero."
//
// Two curves, kept apart on purpose:
//   - COST rises forever, so specializing never caps (only ever-slows): xp_to_next.
//   - EFFECT grows by an ever-smaller-but-never-zero step: power().
//
// `power(L) = 1 + 0.35·ln(1 + L/10)` is the *effect* multiplier every skill,
// attribute, and character level reads. It is baked to a fixed-point lookup table
// at first use, so the hot per-tick loop never evaluates a transcendental — and a
// given binary always returns the same value, so single-platform record/replay is
// bit-stable.
//
// DETERMINISM CAVEAT: the table is built from `std::log`, which is NOT correctly
// rounded, so at a half-step boundary two platforms' libms can snap to Q16.16 raw
// values one unit apart. That is fine for per-platform replay (the master plan's
// current bar) but would break *cross-OS* lockstep. When cross-OS determinism is
// gated, replace the `std::log` bake with a generated constant table or a
// fixed-point `ln` (tracked as a follow-up). `power(0)` is already anchored exact.

namespace eng::sim {

// The diminishing effect multiplier for a level. power(0) == 1.0; it rises forever
// but ever more slowly (log growth), never plateauing.
//
// (The cost half of the law — `xp_to_next(level) = 100·level` — currently lives in
// systems.cpp as the shipped float version; it moves here, as an integer, in the
// attribute-XP refactor.)
Fixed power(int level);

}  // namespace eng::sim
