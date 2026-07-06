#pragma once

#include <cstdint>
#include <vector>

// The bitstream: the byte format the engine uses for network packets and save
// files (adr-0013). One serializer, exercised by round-trip tests, is the M0
// C++ graduation kata — you implement it step by step, test-first.
//
// STEP 1 (now): write_u8 / read_u8. A byte in, the same byte out.
//   Later steps add u32 (endianness), a round-trip property test, bounds
//   checking against malformed input, then unify write/read into one function.

namespace eng {

// Appends integers to a growable byte buffer, in order.
class WriteBuffer {
 public:
  // Append one byte to the buffer.
  void write_u8(std::uint8_t value) {
    // TODO(you): store `value` at the end of bytes_.
    bytes_.push_back(value);
  }

  // Append a 32-bit value as four bytes, most-significant byte first
  // (big-endian, a.k.a. "network byte order").
  void write_u32(std::uint32_t value) {
    // TODO(you): append the four bytes of `value`, high byte first. You can
    // build this entirely from write_u8, shifts (>>), and masks (& 0xFF).
    write_u8((value >> 24) & 0xFF);
    write_u8((value >> 16) & 0xFF);
    write_u8((value >> 8) & 0xFF);
    write_u8(value & 0xFF);
  }

  // The bytes written so far, in write order.
  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
};

// Reads integers back out of a byte buffer, in the order they were written.
class ReadBuffer {
 public:
  explicit ReadBuffer(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

  // Read one byte and advance past it. (Bounds checking comes in a later step —
  // for now assume the caller reads no more than was written.)
  std::uint8_t read_u8() {
    // TODO(you): return the byte at read_pos_, then advance read_pos_ by one.
    return bytes_[read_pos_++];
  }

  // Read four bytes as a big-endian 32-bit value (the mirror of write_u32).
  std::uint32_t read_u32() {
    // TODO(you): read four bytes, high byte first, and reassemble them with
    // shifts (<<) and bitwise-or (|). Building on read_u8 works well.
    std::uint32_t result = 0;
    result |= static_cast<std::uint32_t>(read_u8()) << 24;
    result |= static_cast<std::uint32_t>(read_u8()) << 16;
    result |= static_cast<std::uint32_t>(read_u8()) << 8;
    result |= static_cast<std::uint32_t>(read_u8());
    return result;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t read_pos_ = 0;
};

}  // namespace eng
