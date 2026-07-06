#pragma once

#include "engine/sim/command.hpp"

// The transport seam (ADR: "GameNetworkingSockets behind a small ITransport
// interface; Loopback / GNS / Steam implementations").
//
// This is one of the engine's three load-bearing interfaces (the others are the
// command funnel and serialization). It exists so that the game logic never
// knows HOW messages travel — same-process queues today, UDP over the internet
// later. The rule (code-design-rules rule 5) is "no interface without two real
// implementations": we have Loopback now and a real network transport is a
// named future milestone, so the abstraction earns its place.
//
// Why it matters so much: because single-player already talks through this seam
// (client sends input to a server that lives in the same process, over a
// Loopback), adding real multiplayer is swapping the implementation — NOT
// rewriting the game. "Multiplayer is never retrofitted" (adr-0003) is enforced
// by this one interface existing from day one.

namespace eng::net {

// A message travelling from a client to the server. For the walking skeleton it
// carries a sim Command directly, because Loopback is same-process so there's
// nothing to serialize. When a real (over-the-wire) transport arrives, this
// becomes a buffer of bytes and the bitstream serializer turns Commands into
// those bytes — the exact serializer built in the M0 kata plugs in right here.
struct Message {
  sim::Command command;
};

// The minimal transport interface. A real one grows reliable/unreliable
// channels and connection events, but send + poll is the core both directions
// need.
class ITransport {
 public:
  ITransport() = default;
  virtual ~ITransport() = default;

  // A transport is held by reference and owns a live connection, so copying or
  // moving it makes no sense — delete those. (For a class with a virtual
  // destructor, C++ wants you to say what should happen to the other four
  // special members; deleting them is the correct answer for an interface, and
  // it prevents accidental object slicing. Handbook: "RAII / rule of five".)
  ITransport(const ITransport&) = delete;
  ITransport& operator=(const ITransport&) = delete;
  ITransport(ITransport&&) = delete;
  ITransport& operator=(ITransport&&) = delete;

  // Client side: hand a message to the transport for delivery to the server.
  virtual void send(const Message& msg) = 0;

  // Server side: pull the next received message into `out`. Returns false when
  // there are none left this tick.
  virtual bool poll(Message& out) = 0;
};

}  // namespace eng::net
