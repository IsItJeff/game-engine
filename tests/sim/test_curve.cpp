#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "engine/sim/progression/curve.hpp"

// Property tests for the progression curve — they prove "the one law" (uncapped,
// ever-slower, never zero) holds, rather than trusting the formula.

using eng::Fixed;
using eng::sim::power;

// Test names stay ASCII-only: CTest passes the name to the Catch2 binary as a
// filter, and on Windows a non-ASCII char (this was an em-dash) gets mangled by the
// console codepage so the filter matches zero cases and the test "fails". ASCII is
// the portable contract — keep new TEST_CASE names to it.
TEST_CASE("power(0) is exactly 1.0 (no head start)", "[curve]") {
  REQUIRE(power(0) == Fixed::from_int(1));
}

TEST_CASE("power rises forever, ever more slowly, and never plateaus", "[curve]") {
  // Strictly rising at every single level — uncapped, never a flat step.
  Fixed prev = power(0);
  for (int level = 1; level <= 250; ++level) {
    const Fixed cur = power(level);
    REQUIRE(cur > prev);
    prev = cur;
  }
  // Ever-slower: the gain across a fixed 10-level window shrinks as levels climb.
  // Measured over decades, coarse enough to be immune to Q16.16 rounding jitter.
  const Fixed gain_low = power(20) - power(10);
  const Fixed gain_mid = power(110) - power(100);
  const Fixed gain_high = power(210) - power(200);
  REQUIRE(gain_low > gain_mid);
  REQUIRE(gain_mid > gain_high);
  REQUIRE(gain_high > Fixed());  // ...but still positive — never zero
}

TEST_CASE("power keeps rising past the baked table (never zero)", "[curve]") {
  REQUIRE(power(300) > power(255));
  REQUIRE(power(1000) > power(300));
}

TEST_CASE("power matches the design's reference values", "[curve]") {
  // 1 + 0.35·ln(1 + L/10): L10≈1.243, L50≈1.627, L100≈1.839.
  REQUIRE(std::abs(power(10).to_double() - 1.243) < 0.01);
  REQUIRE(std::abs(power(50).to_double() - 1.627) < 0.01);
  REQUIRE(std::abs(power(100).to_double() - 1.839) < 0.01);
}
