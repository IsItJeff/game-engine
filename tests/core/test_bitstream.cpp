#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>

#include <cstdint>
#include <limits>

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

// STEP 3: property tests. Instead of one hand-picked value, assert the
// round-trip rule read(write(x)) == x holds for a flood of values. Catch2's
// GENERATE re-runs the test body once per generated value.

TEST_CASE("every byte value round-trips", "[bitstream]") {
  // range(0, 256) is exhaustive: every possible byte, 0..255.
  const int value = GENERATE(range(0, 256));

  eng::WriteBuffer w;
  w.write_u8(static_cast<std::uint8_t>(value));
  eng::ReadBuffer r(w.bytes());
  REQUIRE(r.read_u8() == value);
}

TEST_CASE("random u32 values round-trip", "[bitstream]") {
  // 4 billion values is too many to check exhaustively, so sample 1000 at
  // random across the whole range, plus the boundary values by hand.
  const std::uint32_t value =
      GENERATE(std::uint32_t{0}, std::numeric_limits<std::uint32_t>::max(),
               take(1000, random(std::uint32_t{0}, std::numeric_limits<std::uint32_t>::max())));

  eng::WriteBuffer w;
  w.write_u32(value);
  eng::ReadBuffer r(w.bytes());
  REQUIRE(r.read_u32() == value);
}

// STEP 4: untrusted input. A read past the end of the buffer must be safe —
// no out-of-bounds access — because the server parses packets from anyone
// (master-plan: "the server must survive arbitrary bytes"). Reading past the
// end returns 0 and flips ok() to false, rather than reading memory it doesn't
// own.
TEST_CASE("reading past the end is safe, not undefined", "[bitstream]") {
  eng::ReadBuffer r(std::vector<std::uint8_t>{0x01});  // exactly one byte

  REQUIRE(r.read_u8() == 0x01);  // consumes the byte; still ok
  REQUIRE(r.ok());

  const std::uint8_t past = r.read_u8();  // nothing left to read
  REQUIRE(past == 0);                     // safe default, no out-of-bounds read
  REQUIRE_FALSE(r.ok());                  // the stream records that it overflowed
}

// STEP 5: unify write and read. One serialize() function, written once, saves
// a struct and loads it back depending only on the stream's direction.
namespace {
struct Player {
  std::uint32_t health = 0;
  std::uint8_t level = 0;
};

// The single source of truth for Player's wire format. Note it names each field
// exactly once — add a field here and BOTH save and load pick it up.
void serialize(eng::ByteStream& s, Player& p) {
  s.serialize_u32(p.health);
  s.serialize_u8(p.level);
}
}  // namespace

TEST_CASE("a struct round-trips through one serialize function", "[bitstream]") {
  Player saved{1000, 7};

  eng::ByteStream writer = eng::ByteStream::writer();
  serialize(writer, saved);  // same function...

  eng::ByteStream reader = eng::ByteStream::reader(writer.bytes());
  Player loaded{};
  serialize(reader, loaded);  // ...opposite direction

  REQUIRE(loaded.health == 1000);
  REQUIRE(loaded.level == 7);
  REQUIRE(reader.ok());
}
