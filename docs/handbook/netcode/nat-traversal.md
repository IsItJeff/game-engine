# NAT Traversal

## What it is

Your home router does **Network Address Translation**: many devices share one IP, so on each outbound packet the router rewrites the private source `ip:port` to its own and remembers the mapping, so replies find their way home. The catch: a router **accepts an inbound packet only if it already saw a matching outbound one**. A packet from a stranger has no mapping, so it is dropped.

That rule is why two players behind home routers cannot just connect. **NAT traversal** is the escalation ladder that gets a direct link anyway: discover your own public address (STUN-style), then **UDP hole punching** (both sides send at once to open both mappings), and when that fails, fall back to a **relay** both peers can reach.

Mapped onto this engine (all planned — pre-M1, nothing beyond the toolchain exists yet): dev and dedicated servers need no traversal, and player-hosted co-op will inherit Valve's relay free at M9 ([ADR-0014](../../engine/architecture/adr-0014-gns-transport.md)).

## Why you care

This is the "why can't my friend join my game" problem. When both peers sit behind NAT, each is invisible to unsolicited traffic, so whoever connects first gets dropped — a deadlock neither can break alone.

Most of the time this engine will sidestep it. Single-player will be a client plus an embedded server over a loopback transport ([ADR-0003](../../engine/architecture/adr-0003-single-player-is-a-listen-server.md)) — no wire, no NAT. A dev or dedicated server will live at a public address, so clients dial it with plain UDP. Traversal only bites the hardest case: **two consumer machines, player-hosted, over the internet.** The plan is to not hand-roll it (K5, the never-hand-roll list) and buy the ladder from Valve later ([master plan](../../design/master-plan.md)).

## Quick start

A toy showing why the naive connection fails, then why hole punching fixes it. A `Nat` accepts inbound only from a remote it already sent to:

```cpp
#include <cstdint>
#include <cstdio>
#include <map>

// The public endpoint the internet sees after translation.
struct Endpoint {
    std::uint32_t ip;
    std::uint16_t port;
    bool operator<(const Endpoint& o) const {
        return ip < o.ip || (ip == o.ip && port < o.port);
    }
};

// A home router doing NAT. It only lets an inbound packet through if it
// already saw an outbound packet to that same remote endpoint.
struct Nat {
    Endpoint public_addr;              // the one address the internet sees
    std::map<Endpoint, bool> punched;  // remotes we have sent to

    Endpoint SendTo(Endpoint remote) {  // we sent out: open the mapping
        punched[remote] = true;
        return public_addr;
    }
    bool AcceptFrom(Endpoint remote) const {  // inbound: only if punched
        return punched.find(remote) != punched.end();
    }
};

int main() {
    Nat alice{{0x02020202, 4242}, {}};  // 2.2.2.2:4242
    Nat bob{{0x07070707, 5678}, {}};    // 7.7.7.7:5678

    // Alice fires at Bob's public endpoint (learned via STUN). Bob never
    // sent to her, so his router drops it — no mapping exists.
    std::printf("unsolicited accepted? %s\n",
                bob.AcceptFrom(alice.public_addr) ? "yes" : "no");

    // Hole punch: BOTH send outward at once, opening both mappings.
    alice.SendTo(bob.public_addr);
    bob.SendTo(alice.public_addr);

    bool open = alice.AcceptFrom(bob.public_addr)
             && bob.AcceptFrom(alice.public_addr);
    std::printf("after punch, both directions open? %s\n", open ? "yes" : "no");
    return 0;
}
```

It prints `no` then `yes`: the packet the router rejected cold gets through once each side has punched outward.

## How it works

The ladder needs a **coordination server** at a public address both peers can already reach (plain UDP, no traversal). It tells each peer the public `ip:port` the other's NAT presents — the STUN trick, "the server tells you what it saw you from" — then both punch at once.

```mermaid
sequenceDiagram
    participant A as Alice (behind NAT)
    participant Co as Coordinator (public)
    participant B as Bob (behind NAT)
    A->>Co: hello (NAT rewrites source → Co learns Alice's public ip:port)
    B->>Co: hello (Co learns Bob's public ip:port)
    Co-->>A: Bob is at 7.7.7.7:5678
    Co-->>B: Alice is at 2.2.2.2:4242
    Note over A,B: hole punch — both fire at once (~1 RTT)
    A->>B: punch (opens Alice's mapping; Bob's NAT drops this one)
    B->>A: punch (opens Bob's mapping; Alice's NAT drops this one)
    A->>B: game packet — mapping now exists, accepted
    B->>A: game packet — accepted
    Note over A,B: symmetric NAT / UDP blocked → give up, relay instead
```

Punching wins on most home routers, which reuse one external port per internal socket, so the address STUN reported is where the punch arrives. **Symmetric NAT** breaks that: it assigns a **fresh** external port per destination, so the port the coordinator saw is not the one Bob's packet uses — the punch misses. Firewalls that block UDP outright fail the same way.

When punching fails, the fallback is a **relay**: both peers hold an outbound link to a public relay box that forwards packets between them. It always works (both links are ordinary outbound flows the NAT maps) at the cost of latency and someone's bandwidth bill.

### The engine's answer

Rather than run STUN servers, signaling, and relays, the engine plans to swap the GameNetworkingSockets transport for **Steam Sockets** at M9 ([ADR-0014](../../engine/architecture/adr-0014-gns-transport.md)), which unlocks **Steam Datagram Relay** — Valve's backbone doing traversal, routing, encryption, and DDoS shielding in one, free for games on Steam.

!!! info
    Open-source GNS can punch via WebRTC's ICE, but the engine's vcpkg build will leave standalone ICE off, so before the M9 Steam Sockets swap there is no player-hosted traversal — only public-address servers ([ADR-0014](../../engine/architecture/adr-0014-gns-transport.md)).

## Pros / Cons

Relay-backed service (SDR) versus hand-rolled direct punching:

| Pros | Cons |
| --- | --- |
| Just works: symmetric NAT and UDP-blocked networks still connect | Relay adds a latency hop versus a direct punch |
| No STUN, signaling, or relay servers to run (K5) | Ties player-hosted co-op to Steam and its SDK |
| SDR bundles encryption, DDoS shielding, IP hiding — free | Nothing works for strangers until the M9 swap lands |
| No traversal for dev and dedicated servers | Relay bandwidth costs someone when punching fails |

## What to expect

Through M3 and M5 you will never touch traversal: netcode will run over loopback or against a public dedicated server (a VPS enters at M5), so plain UDP is enough ([master plan](../../design/master-plan.md)). Playtest friends connect to a hosted address, not each other.

Player-hosted "invite a stranger" co-op arrives with M9's Steam Sockets swap, where the ~6-function transport seam gains SDR underneath and NAT traversal stops being your problem. Expect a transport-layer change only: nothing in gameplay code above the seam sees it — exactly why ADR-0014 picked GNS in the first place.

## Go deeper

- [Client-server model](./client-server-model.md) — why there is a server to dial
- [Transport reliability](./transport-reliability.md) — what the connection carries once open
- [Serialization basics](../architecture/serialization-basics.md) — packing the bytes that cross the wire
- [ADR-0014](../../engine/architecture/adr-0014-gns-transport.md) — GNS behind a minimal transport, Steam Sockets + SDR at M9
- [master plan](../../design/master-plan.md) — rows M3/M5/M9 and the K5 never-hand-roll list

**Sources**

- Tailscale — How NAT traversal works — https://tailscale.com/blog/how-nat-traversal-works — accessed 2026-07-06
- Bryan Ford, Pyda Srisuresh, Dan Kegel — Peer-to-Peer Communication Across Network Address Translators — https://bford.info/pub/net/p2pnat/ — accessed 2026-07-06
- Steamworks Documentation — Steam Datagram Relay — https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay — accessed 2026-07-06
- ValveSoftware/GameNetworkingSockets — README (P2P, ICE, symmetric connect) — https://github.com/ValveSoftware/GameNetworkingSockets — accessed 2026-07-06
