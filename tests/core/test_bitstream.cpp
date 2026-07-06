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

// STEP 2: a u32 is four bytes, so write_u32 must choose an order and read_u32
// must use the same one. This engine uses big-endian ("network byte order"):
// most-significant byte first.

TEST_CASE("a u32 round-trips through the buffer", "[bitstream]") {
  eng::WriteBuffer w;
  w.write_u32(0xDEADBEEF);

  eng::ReadBuffer r(w.bytes());
  REQUIRE(r.read_u32() == 0xDEADBEEF);
}

// Pins the byte order so it can never silently drift. A shortcut that copies
// the u32's raw memory would pass the round-trip above but fail here on a
// little-endian machine (which is every machine this engine targets).
TEST_CASE("a u32 is written big-endian, high byte first", "[bitstream]") {
  eng::WriteBuffer w;
  w.write_u32(0xDEADBEEF);

  const std::vector<std::uint8_t> expected{0xDE, 0xAD, 0xBE, 0xEF};
  REQUIRE(w.bytes() == expected);
}
