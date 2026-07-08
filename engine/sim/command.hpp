#pragma once

#include "engine/core/math.hpp"
#include "engine/sim/types.hpp"

// The command funnel (code-design-rules rule 3: "all sim mutation goes through
// the command funnel").
//
// A Command is an INTENT to change the world — "player 1 wants to move left",
// "spawn a mote here". The rule the whole engine is built around: the world is
// NEVER mutated directly by input handlers, the network, the UI, or scripts.
// Instead every one of those produces a Command, and a single function
// (apply_command, in world.cpp) is the ONLY code that changes state.
//
// Why go to this trouble? Because one choke point for all mutation is what makes
// the hard features later possible:
//   - Multiplayer: the server can validate every command ("is player 1 allowed
//     to do this right now?") before applying it — clients can't cheat by
//     mutating state directly, because there is no direct path.
//   - Replay & networking: a recorded stream of commands replays the whole game
//     deterministically; that same stream is what gets sent over the network.
//   - Debugging: every change to the world flows through one function you can
//     log or breakpoint.
//
// For the skeleton there are two command kinds. We model them with a simple
// enum + struct (a "tagged" struct) rather than std::variant so the code stays
// easy to read; as the command set grows, std::variant becomes the more
// type-safe choice (see the handbook).

namespace eng::sim {

enum class CommandKind {
  MovePlayer,    // push a player's entity in a direction
  SpawnMote,     // create a new drifting entity
  DamagePlayer,  // subtract from a player's health
  Attack,        // strike the nearest hazard within reach
  Equip,         // wield the nearest dropped Weapon within reach
};

struct Command {
  CommandKind kind;

  // Which player issued this. Lets the server attribute and authorize commands.
  PlayerId player = kLocalPlayer;

  // MovePlayer: the desired move direction this tick, each axis in -1..1.
  Vec2 move_dir{0.0f, 0.0f};

  // SpawnMote: where to create the new entity, in world units.
  Vec2 spawn_pos{0.0f, 0.0f};

  // DamagePlayer: how much health to remove.
  float amount = 0.0f;
};

// Convenience constructors, so call sites read as intent, not struct-filling.
inline Command move_player(PlayerId player, Vec2 dir) {
  return Command{CommandKind::MovePlayer, player, dir, {}};
}
inline Command spawn_mote(Vec2 pos) {
  return Command{CommandKind::SpawnMote, kLocalPlayer, {}, pos};
}
inline Command damage_player(PlayerId player, float amount) {
  return Command{CommandKind::DamagePlayer, player, {}, {}, amount};
}
// Attack: the target is computed from the attacker's own position (nearest hazard
// in reach), so the intent carries only who is swinging.
inline Command attack(PlayerId player) {
  return Command{CommandKind::Attack, player, {}, {}, 0.0f};
}
// Equip: like Attack, the target (the nearest dropped weapon in reach) is computed
// server-side, so the intent carries only who is reaching for one.
inline Command equip(PlayerId player) {
  return Command{CommandKind::Equip, player, {}, {}, 0.0f};
}

}  // namespace eng::sim
