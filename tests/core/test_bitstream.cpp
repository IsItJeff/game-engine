#include <catch2/catch_test_macros.hpp>

#include "engine/core/bitstream.hpp"

// STEP 1: the smallest possible round-trip. Write one byte, read it back,
// get the same value. This test fails until you implement write_u8/read_u8
// in engine/core/bitstream.hpp. That failure is the "red" — make it green.
TEST_CASE("a byte round-trips through the buffer", "[bitstream]") {
  eng::WriteBuffer w;
  w.write_u8(0x42);

  eng::ReadBuffer r(w.bytes());
  REQUIRE(r.read_u8() == 0x42);
}
