#include "engine/sim/progression/curve.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace eng::sim {

namespace {

// The table covers levels 0..255; anything beyond is extrapolated (see power()).
// 255 is already ~3.2M XP of a single skill — hours of continuous play — so the
// table is effectively the whole practical range.
constexpr int kTableSize = 256;

// Build the table ONCE. This is the only place a float touches the curve, and it
// runs at static-init, never per tick. Snapping each value to Q16.16 hides MOST
// libm variance: `std::log` may differ across platforms in its last ~1e-15, ten
// orders of magnitude below the Q16.16 step (~1.5e-5), so the rounded value is
// almost always identical. The exception is a value landing right on a half-step
// boundary, where two libms can straddle the rounding point and snap one raw unit
// apart — fine for per-platform replay, not yet cross-OS bit-exact. See the
// determinism caveat in curve.hpp.
std::array<Fixed, kTableSize> build_table() {
  std::array<Fixed, kTableSize> table{};
  for (int level = 0; level < kTableSize; ++level) {
    const double p = 1.0 + 0.35 * std::log(1.0 + static_cast<double>(level) / 10.0);
    table[static_cast<std::size_t>(level)] = Fixed::from_float(p);
  }
  // power(0) needs no special-casing: log(1) is exactly +0 (guaranteed by the C
  // standard), so table[0] = from_float(1.0) is exactly 1.0 on every platform.
  return table;
}

const std::array<Fixed, kTableSize>& table() {
  static const std::array<Fixed, kTableSize> t = build_table();
  return t;
}

}  // namespace

Fixed power(int level) {
  if (level < 0) level = 0;
  if (level < kTableSize) return table()[static_cast<std::size_t>(level)];

  // Past the table (astronomically high levels): extend by the final step so the
  // curve keeps rising and never plateaus — the "never zero" half of the law.
  const Fixed last = table()[kTableSize - 1];
  const Fixed step = last - table()[kTableSize - 2];
  return last + step * Fixed::from_int(level - (kTableSize - 1));
}

std::int64_t xp_to_next(int level) {
  return static_cast<std::int64_t>(100) * level;
}

}  // namespace eng::sim
