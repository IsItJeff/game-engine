#pragma once

#include <cstdint>
#include <vector>

// The bitstream: the byte format the engine uses for network packets and save
// files (adr-0013). ByteStream is the real interface — one serialize() function
// per type saves it and loads it, so the two directions can't drift apart.
// WriteBuffer and ReadBuffer are the earlier stepping-stones the kata built on
// the way there (big-endian byte order, bounds-safe reads).

namespace eng {

// Appends integers to a growable byte buffer, in order.
class WriteBuffer {
 public:
  // Append one byte to the buffer.
  void write_u8(std::uint8_t value) { bytes_.push_back(value); }

  // Append a 32-bit value as four bytes, most-significant byte first
  // (big-endian, a.k.a. "network byte order").
  void write_u32(std::uint32_t value) {
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

  // Read one byte and advance past it. Reading past the end is safe: it returns
  // 0 and trips ok() to false rather than reading out of bounds.
  std::uint8_t read_u8() {
    if (read_pos_ >= bytes_.size()) {
      ok_ = false;
      return 0;
    }
    return bytes_[read_pos_++];
  }

  // True until a read runs past the end of the buffer. Once false, the data
  // read afterwards can't be trusted — check this after parsing untrusted input.
  bool ok() const { return ok_; }

  // Read four bytes as a big-endian 32-bit value (the mirror of write_u32).
  std::uint32_t read_u32() {
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
  bool ok_ = true;
};

// The unified stream: one object that is either writing or reading. Gameplay
// writes a serialize() function ONCE against it, and passing a writer saves the
// data while passing a reader loads it back — so the two directions can never
// drift apart. This is the pattern the engine uses for packets and saves
// (adr-0013); it folds WriteBuffer and ReadBuffer above into a single type.
class ByteStream {
 public:
  // A stream in write mode, starting empty.
  static ByteStream writer() { return ByteStream(Mode::Write, {}); }
  // A stream in read mode over existing bytes.
  static ByteStream reader(std::vector<std::uint8_t> bytes) {
    return ByteStream(Mode::Read, std::move(bytes));
  }

  bool is_writing() const { return mode_ == Mode::Write; }
  bool ok() const { return ok_; }
  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

  // Serialize one byte. Writing appends `value`; reading overwrites `value`
  // with the next byte (0 and ok()=false past the end). The direction branch
  // lives HERE so callers never repeat it.
  void serialize_u8(std::uint8_t& value) {
    if (is_writing()) {
      bytes_.push_back(value);
    } else {
      if (read_pos_ >= bytes_.size()) {
        ok_ = false;
        value = 0;
      } else {
        value = bytes_[read_pos_++];
      }
    }
  }

  // Serialize a big-endian u32, built from serialize_u8 so both directions
  // route through the one guarded primitive above.
  void serialize_u32(std::uint32_t& value) {
    if (is_writing()) {
      std::uint8_t b0 = static_cast<std::uint8_t>((value >> 24) & 0xFF);
      std::uint8_t b1 = static_cast<std::uint8_t>((value >> 16) & 0xFF);
      std::uint8_t b2 = static_cast<std::uint8_t>((value >> 8) & 0xFF);
      std::uint8_t b3 = static_cast<std::uint8_t>(value & 0xFF);
      serialize_u8(b0);
      serialize_u8(b1);
      serialize_u8(b2);
      serialize_u8(b3);
    } else {
      std::uint32_t result = 0;
      std::uint8_t byte = 0;
      serialize_u8(byte);
      result |= static_cast<std::uint32_t>(byte) << 24;
      serialize_u8(byte);
      result |= static_cast<std::uint32_t>(byte) << 16;
      serialize_u8(byte);
      result |= static_cast<std::uint32_t>(byte) << 8;
      serialize_u8(byte);
      result |= static_cast<std::uint32_t>(byte);
      value = result;
    }
  }

 private:
  enum class Mode : std::uint8_t { Write, Read };
  ByteStream(Mode mode, std::vector<std::uint8_t> bytes) : mode_(mode), bytes_(std::move(bytes)) {}

  Mode mode_;
  std::vector<std::uint8_t> bytes_;
  std::size_t read_pos_ = 0;
  bool ok_ = true;
};

}  // namespace eng
