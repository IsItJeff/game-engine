# ADR-0014: GameNetworkingSockets behind a minimal transport interface

**Status:** accepted (2026-07).

Networking uses Valve's GameNetworkingSockets (GNS) behind a ~6-function transport interface with three implementations: Loopback, GNS, and (at M9) Steam Sockets. The loopback transport supports simulated latency and loss, so every net milestone is tested under 100 ms + 5% loss, not just localhost. M0 CI proves GNS builds on all three platforms; first real use is M3. GNS types never leave `engine/net/` (quarantine rule), and all three transports pass one shared test suite.

Why: GNS's API *is* Steamworks networking, so swapping to Steam Sockets at M9 gains Steam Datagram Relay — solving player-hosted NAT traversal for free — without touching code above the transport seam. This is the cheapest path from "friends over loopback" to "strangers over Steam" that exists.

Rejected: ENet (fine library, no SDR upgrade path); raw UDP + hand-rolled reliability (K5 — on the never-hand-roll list); WebRTC (the game never runs in browsers; the companion app uses plain WS).
