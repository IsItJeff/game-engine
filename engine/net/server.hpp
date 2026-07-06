#pragma once

#include "engine/net/transport.hpp"
#include "engine/sim/world.hpp"

// The Server: the authority that owns the world and advances it.
//
// This is the "single simulation, three wirings" idea (adr-0003) made concrete.
// The SAME Server runs in all three modes the engine will support:
//   - single-player: one client, connected by a Loopback (this skeleton),
//   - listen server: the host's client plus remote clients over the network,
//   - dedicated server: a headless process with only remote clients.
//
// It knows nothing about rendering, windows, or input devices — it only receives
// Commands through a transport and steps the world. That is exactly why a
// headless dedicated server is "nearly free": it's this class with no client
// attached. The client is a separate thing that reads the world to draw it.

namespace eng::net {

class Server {
 public:
  explicit Server(ITransport& transport) : transport_(transport) {}

  // Advance one tick: drain every input message received since last tick into
  // the world's command funnel, then step the simulation once. The server is
  // the ONLY thing that steps the world — clients just send input and observe.
  void tick() {
    Message msg;
    while (transport_.poll(msg)) {
      world_.submit(msg.command);
    }
    world_.step();
  }

  // Read-only access for a co-located client to render from. Over a real network
  // this becomes "send a snapshot to the client" (roadmap M3); in-process the
  // client can read the authoritative world directly.
  const sim::World& world() const { return world_; }

 private:
  ITransport& transport_;
  sim::World world_;
};

}  // namespace eng::net
