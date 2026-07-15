#pragma once

#include <random>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/sim/command.hpp"
#include "engine/sim/types.hpp"

// The World: the entire simulation state, and the code that advances it.
//
// This is the heart of the engine. It owns:
//   - an EnTT registry (all entities and their components),
//   - a queue of pending Commands (the command funnel),
//   - the current tick number,
//   - a deterministic random-number generator.
//
// The world advances ONLY through step(), and changes ONLY through Commands
// submitted via submit(). Nothing outside reaches in and mutates entities. That
// discipline is what makes the world replayable and network-ready later.
//
// Roadmap position: this is the M2 "engine core" in miniature. It will grow
// scene loading, more systems, and network replication, but the shape — submit
// commands, step the world, read state for rendering — stays the same.

namespace eng::sim {

// Size of the play field in world units. The field wraps around at the edges
// (toroidal) so entities never leave view. A constant now; a real world uses a
// camera and a much larger space.
inline constexpr float kFieldWidth = 1280.0f;
inline constexpr float kFieldHeight = 720.0f;

// Keep a steady threat coming: every kCreatureSpawnInterval seconds the world spawns
// a hunting creature, up to kMaxCreatures alive at once, so the fight never runs dry.
// kMaxCreatures matches the count of hand-placed opener archetypes in build_scene (brute,
// swarmer, spitter, leech, warden, knitflesh, bomber), so the scene starts exactly at cap.
inline constexpr float kCreatureSpawnInterval = 6.0f;
inline constexpr int kMaxCreatures = 7;

// Keep the colony alive too: colonists wander in on a SLOWER timer (creatures now hunt
// NPCs, so without replenishment the field slowly empties of the people whose skirmishes
// make the world feel alive). Deliberately slower than the creature interval so the world
// stays net-hostile — reinforcements, not safety. Tuning knobs.
inline constexpr float kNpcSpawnInterval = 12.0f;
inline constexpr int kMaxNpcs = 6;

class World {
 public:
  // Builds the opening scene: one player-controlled entity plus a few drifting
  // motes, so there is something moving the moment the app starts.
  World();

  // Enqueue a command to apply at the start of the next step(). This is the
  // ONLY way to change the world from outside — input, network, UI and scripts
  // all funnel through here (command.hpp explains why).
  void submit(const Command& cmd);

  // Advance the simulation by exactly one fixed tick: snapshot positions for
  // interpolation, apply queued commands, run the systems, increment the tick.
  void step();

  // Read-only views for the renderer and debug UI. Presentation code observes
  // the world; it never mutates it (code-design-rules: renderer reads snapshots).
  const entt::registry& registry() const { return registry_; }
  entt::registry& registry() { return registry_; }  // for tools/tests only
  Tick tick() const { return tick_; }
  entt::entity player() const { return player_; }

 private:
  // The single function that turns a Command into a state change — the one
  // place the world is allowed to mutate in response to intent.
  void apply_command(const Command& cmd);

  entt::registry registry_;
  std::vector<Command> pending_;  // the command funnel's queue
  Tick tick_ = 0;

  // Deterministic PRNG: seeded with a fixed value so a given command stream
  // always produces the same world. Determinism is what makes replay and
  // (later) lockstep-free networking debuggable — never call rand().
  std::mt19937 rng_{1234};

  // A SEPARATE deterministic stream for the NPC spawner's own draws (when/where a colonist
  // arrives), so those rolls stay off the rng_ stream the creatures + combat use. It keeps
  // the spawner's placement/timing from directly perturbing the creature waves that tests
  // pin; the NPCs it spawns can still touch rng_ later (combat dodge rolls), so it's not
  // full invariance to colony tuning. Different seed = an independent, reproducible stream.
  std::mt19937 npc_spawn_rng_{5678};

  // A THIRD independent stream, for rolling a fine drop's quality in handle_deaths (a slain brute's
  // steel, a sentinel's plate). Kept off rng_ for the same reason as the spawner's: these loot
  // rolls must not perturb the creature/combat waves that tests pin — so adding rolled loot leaves
  // every existing dodge, spawn and wave bit-identical, only the dropped quality varies. Its own
  // seed = an independent, reproducible stream.
  std::mt19937 drop_rng_{9012};

  // Counts down each step; when it reaches 0 the world spawns a creature (if under
  // the cap) and resets. Starts at a full interval so the opening two hunters get a
  // head start before reinforcements arrive.
  float creature_spawn_timer_ = kCreatureSpawnInterval;

  // The colony's own reinforcement timer, mirroring creature_spawn_timer_.
  float npc_spawn_timer_ = kNpcSpawnInterval;

  entt::entity player_ = entt::null;
};

}  // namespace eng::sim
