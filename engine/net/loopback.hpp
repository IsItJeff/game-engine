#pragma once

#include <deque>

#include "engine/net/transport.hpp"

// LoopbackTransport: the single-player / same-process implementation of the
// transport seam.
//
// There is no network here at all — send() just puts a message in a queue and
// poll() takes it out. But because the game talks to it through the ITransport
// interface, the code above it is IDENTICAL to what it will run when the queue
// is replaced by real UDP packets. That's the payoff: single-player exercises
// the exact same "client sends input, server receives it" path as multiplayer,
// so the multiplayer version can't diverge into an untested code path.

namespace eng::net {

class LoopbackTransport : public ITransport {
 public:
  void send(const Message& msg) override { queue_.push_back(msg); }

  bool poll(Message& out) override {
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.pop_front();
    return true;
  }

 private:
  // A deque so poll() removes from the front in O(1). Messages are delivered in
  // the order they were sent — a real transport can't always promise that, which
  // is one of the things the networking milestones will teach.
  std::deque<Message> queue_;
};

}  // namespace eng::net
