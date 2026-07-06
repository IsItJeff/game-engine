#pragma once

#include <cstdint>

// Fundamental simulation types shared across the sim layer.

namespace eng::sim {

// Identifies a player. Opaque on purpose (ADR: "opaque PlayerId, local GUID now,
// SteamID64 later"). Today single-player uses a fixed local id; when multiplayer
// arrives (roadmap M3+) this becomes the network/Steam identity, and NOTHING
// that stores or compares a PlayerId has to change — that's the whole point of
// giving it a name now instead of using a raw int everywhere.
using PlayerId = std::uint32_t;

// The single local player's id in single-player. Real ids are assigned by the
// server at connect time later.
inline constexpr PlayerId kLocalPlayer = 1;

// A simulation tick number. The world advances in discrete fixed steps (ADR:
// "fixed 60 Hz tick"); this counts them. Inputs and, later, network snapshots
// are stamped with the tick they belong to.
using Tick = std::uint64_t;

// The fixed simulation rate. The world is stepped exactly this many times per
// second regardless of render frame rate (see simulation.hpp). A named constant,
// never a magic number sprinkled through the code.
inline constexpr int kTicksPerSecond = 60;
inline constexpr double kSecondsPerTick = 1.0 / kTicksPerSecond;

}  // namespace eng::sim
