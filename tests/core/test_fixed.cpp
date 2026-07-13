#include <catch2/catch_test_macros.hpp>

#include "engine/core/fixed.hpp"

// Unit tests for the deterministic Q16.16 fixed-point type. It is the numeric
// bedrock of the sim, so these pin its exact behaviour — including the fraction
// it keeps and the saturation that keeps it UB-free.

using eng::Fixed;

TEST_CASE("Fixed represents whole numbers exactly", "[fixed]") {
  REQUIRE(Fixed::from_int(3).floor() == 3);
  REQUIRE(Fixed::from_int(3).raw() == 3 * Fixed::kOne);
  REQUIRE(Fixed::from_int(-5).floor() == -5);
  REQUIRE(Fixed().raw() == 0);  // default-constructed is zero
}

TEST_CASE("Fixed add and subtract are exact", "[fixed]") {
  REQUIRE((Fixed::from_int(2) + Fixed::from_int(3)) == Fixed::from_int(5));
  REQUIRE((Fixed::from_int(2) - Fixed::from_int(5)) == Fixed::from_int(-3));

  Fixed a = Fixed::from_int(10);
  a += Fixed::from_int(4);
  a -= Fixed::from_int(1);
  REQUIRE(a == Fixed::from_int(13));
}

TEST_CASE("Fixed multiply and divide keep the fraction", "[fixed]") {
  const Fixed half = Fixed::from_ratio(1, 2);
  REQUIRE((half * half) == Fixed::from_ratio(1, 4));  // 0.5 * 0.5 = 0.25
  REQUIRE((Fixed::from_ratio(5, 2) * Fixed::from_int(4)) == Fixed::from_int(10));  // 2.5 * 4

  const Fixed three_halves = Fixed::from_int(3) / Fixed::from_int(2);  // 1.5
  REQUIRE(three_halves == Fixed::from_ratio(3, 2));
  REQUIRE(three_halves.floor() == 1);
  REQUIRE(three_halves.round() == 2);
}

TEST_CASE("Fixed exposes the Q16.16 precision limit honestly", "[fixed]") {
  // 1/3 is not exactly representable; 3 * (1/3) lands one step below 1.0.
  const Fixed third = Fixed::from_ratio(1, 3);
  REQUIRE(third.floor() == 0);
  REQUIRE((third * Fixed::from_int(3)).floor() == 0);  // 0.99998… floors to 0
}

TEST_CASE("Fixed frac returns the part in [0, 1)", "[fixed]") {
  const Fixed v = Fixed::from_ratio(29, 4);  // 7.25
  REQUIRE(v.floor() == 7);
  REQUIRE(v.frac() == Fixed::from_ratio(1, 4));   // 0.25 — the fractional-carry piece
  REQUIRE(Fixed::from_int(4).frac() == Fixed());  // a whole number has no fraction
}

TEST_CASE("Fixed comparisons order values", "[fixed]") {
  REQUIRE(Fixed::from_int(1) < Fixed::from_int(2));
  REQUIRE(Fixed::from_ratio(1, 2) <= Fixed::from_ratio(1, 2));
  REQUIRE(Fixed::from_int(5) > Fixed::from_ratio(9, 2));  // 5 > 4.5
  REQUIRE(Fixed::from_int(-1) < Fixed());
  REQUIRE(Fixed::from_int(2) != Fixed::from_int(3));
}

TEST_CASE("Fixed from_float round-trips within precision", "[fixed]") {
  REQUIRE(Fixed::from_float(0.25) == Fixed::from_ratio(1, 4));  // exact power-of-two fraction
  REQUIRE(Fixed::from_float(2.4).round() == 2);
  REQUIRE(Fixed::from_float(2.6).round() == 3);
  REQUIRE(Fixed::from_float(-1.5).floor() == -2);  // floor rounds toward -infinity
}

TEST_CASE("Fixed saturates instead of overflowing (UB-free, replay-stable)", "[fixed]") {
  // 30000 * 30000 = 9e8, far over the ~32767 max — clamps to the max raw value
  // instead of the undefined behaviour a plain int32 overflow would be.
  const Fixed big = Fixed::from_int(30000);
  REQUIRE((big * big) == Fixed::from_raw(0x7fffffff));
  REQUIRE((Fixed::from_raw(0x7fffffff) + Fixed::from_int(1)) == Fixed::from_raw(0x7fffffff));
}

TEST_CASE("Fixed round and negate are UB-free at the raw extremes", "[fixed]") {
  // The two ops that historically skipped the 64-bit path: round() added kOne/2 in int32
  // (overflowing near INT32_MAX), and unary operator-() negated in int32 (overflowing at
  // INT32_MIN). At the saturated extremes both must stay UB-free — the ASan/UBSan CI leg guards
  // this, and the values must be right (the old int32 overflow both trips UBSan AND yields a wrong
  // wrapped result).
  const Fixed max_val = Fixed::from_raw(0x7fffffff);  // ~32767.99998 — the saturated max
  REQUIRE(max_val.round() == 32768);  // rounds up; the +kOne/2 must not overflow int32
  const Fixed min_val = Fixed::from_raw(-0x7fffffff - 1);  // INT32_MIN raw — the saturated min
  REQUIRE((-min_val) == Fixed::from_raw(0x7fffffff));      // negate saturates (no -INT32_MIN UB)
  REQUIRE((-Fixed::from_int(3)) == Fixed::from_int(-3));  // and ordinary negation still round-trips
}
