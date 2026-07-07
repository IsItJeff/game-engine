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
// runs at static-init, never per tick. Snapping each value to Q16.16 makes the
// bake cross-platform-identical: `std::log` may differ across libms in its last
// ~1e-15, which is ten orders of magnitude below the Q16.16 step (~1.5e-5), so the
// rounded fixed-point value is the same on every machine.
std::array<Fixed, kTableSize> build_table() {
  std::array<Fixed, kTableSize> table{};
  for (int level = 0; level < kTableSize; ++level) {
    const double p = 1.0 + 0.35 * std::log(1.0 + static_cast<double>(level) / 10.0);
    table[static_cast<std::size_t>(level)] = Fixed::from_float(p);
  }
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

}  // namespace eng::sim
