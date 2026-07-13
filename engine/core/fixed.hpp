#pragma once

#include <cstdint>

// A Q16.16 signed fixed-point number — a 32-bit integer holding (value << 16).
//
// WHY THIS EXISTS: the simulation must be DETERMINISTIC to the bit on every OS,
// because that is what makes record/replay, the state-hash, and (later) lockstep
// netcode work — the same inputs must produce byte-identical output on a Mac, a
// Windows box, and a Linux CI runner. Floating-point results differ subtly across
// compilers and CPUs (fused-multiply-add, 80-bit intermediates, rounding modes);
// integer math does not. So every gameplay VALUE that feeds a replayable
// computation is a Fixed, never a float.
//
// LAYOUT: 16 integer bits give a range of about ±32768, and 16 fractional bits
// give a step of 1/65536 ≈ 0.0000153 — ample for multipliers, pools (health,
// stamina), damage, and the "fractional carry" that keeps action outputs whole.
// Whole numbers that never need a fraction (levels, XP, counts) stay plain ints;
// Fixed is only for values with a fractional part.
//
// SAFETY: every operation goes through a 64-bit intermediate and SATURATES to the
// int32 range instead of overflowing. Signed overflow is undefined behaviour in
// C++ (and would be caught by UBSan); saturating is both UB-free and
// deterministic, so a runaway multiply clamps rather than corrupts a replay.

namespace eng {

class Fixed {
 public:
  static constexpr int kFractionBits = 16;
  static constexpr std::int32_t kOne = 1 << kFractionBits;  // 65536 == 1.0

  constexpr Fixed() = default;

  // --- construction ---------------------------------------------------------

  // A whole number: from_int(3) is exactly 3.0.
  static constexpr Fixed from_int(std::int32_t whole) {
    return from_raw(saturate(static_cast<std::int64_t>(whole) * kOne));
  }
  // An exact ratio without touching floating point, e.g. from_ratio(1, 3) = 0.333…
  static constexpr Fixed from_ratio(std::int32_t num, std::int32_t den) {
    return from_raw(saturate((static_cast<std::int64_t>(num) * kOne) / den));
  }
  // From a double — for TESTS and content/JSON loading only, never in the hot sim
  // loop, since a float input is exactly where non-determinism would sneak back in.
  static constexpr Fixed from_float(double v) {
    return from_raw(saturate(static_cast<std::int64_t>(v * kOne + (v >= 0 ? 0.5 : -0.5))));
  }
  // Wrap a raw Q16.16 integer (round-trips with raw()).
  static constexpr Fixed from_raw(std::int32_t raw) {
    Fixed f;
    f.raw_ = raw;
    return f;
  }

  // --- reading back ---------------------------------------------------------

  constexpr std::int32_t raw() const { return raw_; }
  constexpr std::int32_t floor() const { return raw_ >> kFractionBits; }  // toward -infinity
  // Add kOne/2 in int64 before the shift: the sum can exceed INT32_MAX for a Fixed near its
  // saturated max (~32767.5+), so doing it in int32 would overflow (UB). The shifted result fits
  // int32.
  constexpr std::int32_t round() const {
    return static_cast<std::int32_t>((static_cast<std::int64_t>(raw_) + kOne / 2) >> kFractionBits);
  }
  constexpr double to_double() const { return static_cast<double>(raw_) / kOne; }
  // The fractional part in [0, 1) — the piece the fractional-carry accumulator keeps.
  constexpr Fixed frac() const { return *this - from_int(floor()); }

  // --- arithmetic (saturating, 64-bit intermediates) ------------------------

  constexpr Fixed operator+(Fixed o) const {
    return from_raw(saturate(static_cast<std::int64_t>(raw_) + o.raw_));
  }
  constexpr Fixed operator-(Fixed o) const {
    return from_raw(saturate(static_cast<std::int64_t>(raw_) - o.raw_));
  }
  // Negate through the 64-bit saturate path like every other op: -raw_ in int32 overflows at
  // INT32_MIN (the saturated min), so widen first — negating the min then saturates to the max.
  constexpr Fixed operator-() const { return from_raw(saturate(-static_cast<std::int64_t>(raw_))); }
  constexpr Fixed operator*(Fixed o) const {
    return from_raw(saturate((static_cast<std::int64_t>(raw_) * o.raw_) >> kFractionBits));
  }
  constexpr Fixed operator/(Fixed o) const {
    // Caller must ensure a non-zero divisor (division by zero is undefined, as for ints).
    return from_raw(saturate((static_cast<std::int64_t>(raw_) << kFractionBits) / o.raw_));
  }

  constexpr Fixed& operator+=(Fixed o) { return *this = *this + o; }
  constexpr Fixed& operator-=(Fixed o) { return *this = *this - o; }
  constexpr Fixed& operator*=(Fixed o) { return *this = *this * o; }

  // Value comparison == raw comparison (a single member). Spelled out rather than
  // defaulted so nothing depends on the C++20 spaceship operator.
  constexpr bool operator==(Fixed o) const { return raw_ == o.raw_; }
  constexpr bool operator!=(Fixed o) const { return raw_ != o.raw_; }
  constexpr bool operator<(Fixed o) const { return raw_ < o.raw_; }
  constexpr bool operator<=(Fixed o) const { return raw_ <= o.raw_; }
  constexpr bool operator>(Fixed o) const { return raw_ > o.raw_; }
  constexpr bool operator>=(Fixed o) const { return raw_ >= o.raw_; }

 private:
  // Clamp a 64-bit result into the int32 raw range — the saturation that keeps
  // every operation UB-free and replay-stable.
  static constexpr std::int32_t saturate(std::int64_t v) {
    constexpr std::int64_t kMax = 0x7fffffff;
    constexpr std::int64_t kMin = -0x80000000LL;
    if (v > kMax) return static_cast<std::int32_t>(kMax);
    if (v < kMin) return static_cast<std::int32_t>(kMin);
    return static_cast<std::int32_t>(v);
  }

  std::int32_t raw_ = 0;
};

}  // namespace eng
