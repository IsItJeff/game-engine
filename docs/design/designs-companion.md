All key facts verified. Writing the design now (returned as text, no files — read-only mode).

---

# Companion (Second-Screen) Service Design

## 1. Topology — verdict: **(b) local direct-serve for v1**, hybrid relay as an explicit later option

The engine's authoritative sim process (which exists in *all* three topologies — single-player runs a local server, listen server, dedicated server) embeds a tiny HTTP+WebSocket listener. Phone scans a QR code shown in-game, browser opens `http://<lan-ip>:<port>/#<token>`, done.

| Concern | (a) Cloud relay | (b) LAN direct | (c) Hybrid |
|---|---|---|---|
| NAT/firewall | Solves NAT; needs backend | Same-LAN only; one Windows Firewall prompt (same prompt the listen server already triggers) | Both |
| Away-from-home | Yes | **No** | Yes |
| Privacy | Gameplay state transits your servers | Nothing leaves the house | Mixed |
| Running cost | You run infra forever, from day one | **$0, forever** | Deferred cost |
| TLS on LAN | Free (wss to cloud) | Plain HTTP — see below | Both |
| Single-player offline | **Broken** (no internet = no companion) | **Works** | Works |
| GDPR/accounts | Yes, immediately | None | Later |

Why (b) wins for v1:

- **The local-HTTPS pain is now avoidable, verified July 2026.** Chrome's Local Network Access permission (enforced ~v142) restricts *public-origin → local* requests, but explicitly does **not** restrict top-level navigation to a LAN IP, and a page *served from* a LAN IP makes same-origin fetch/WS calls with no prompt ([LNA explainer](https://github.com/WICG/local-network-access/blob/main/explainer.md), [Chrome blog](https://developer.chrome.com/blog/local-network-access)). So plain-HTTP direct serving works with zero certificate hassle. Cost: it's a non-secure context — no service worker/PWA install, no camera-in-browser. None needed: the phone's native camera scans the QR, and "open a browser tab" is the whole install story. Do **not** attempt self-signed TLS on LAN — cert warnings on phones are worse than HTTP, and the threat model (your own home Wi-Fi, token-gated) doesn't justify it.
- Second screens are physically next to the game 95% of the time. "Check inventory from the office" is a real but minor use case — that's what the hybrid upgrade is for, and the *protocol doesn't change*: the relay just forwards the same WebSocket frames, so (b) → (c) is additive, not a rewrite.
- Zero infra means the feature can't rot while you're busy shipping the engine. A relay backend is a second product with uptime obligations; don't acquire one before the first product ships.

**Which process serves it:** the sim authority only. Companion is a view/controller of the *simulation*, so on a dedicated server the companion endpoint lives there (players' phones reach it over the internet if the server is; that config is the server admin's problem, same as the game port).

## 2. Protocol — verdict: **one WebSocket, JSON, plus plain HTTP only to serve the page**

- **HTTP GET /** serves a single static HTML file embedded in the engine binary. This kills version skew: page and engine ship together, always matching.
- **One WebSocket** carries both directions. SSE rejected: it's push-only, so you'd need REST for commands anyway — two channels instead of one. REST-polling for the map rejected: it's a push problem.
- **Server→phone:** topic-based. Phone sends `subscribe("map.players")`; server pushes snapshot then deltas, throttled per topic (map at 2–5 Hz — a companion map does not need tick rate). JSON; measure before optimizing (a few players' positions at 2 Hz is nothing).
- **Phone→server:** commands `{seq, verb, args}` → `{seq, ok|error}` reply. **Trust model: identical to a network client.** Companion commands enter the exact same validated command funnel as remote player input — deserialized, schema-checked, authority-checked in the sim tick ("does this PlayerId own that item slot, is the move legal *now*"). No side door that mutates state directly. This is the single most important design rule in this document.
- **Pairing UX:** in-game overlay shows a QR (encodes URL + 128-bit random token) plus a 6-character short code fallback for manual entry. Token binds the socket to a PlayerId (whoever opened the pairing overlay), is valid for the game session, revoked on exit or via an in-game "disconnect devices" button. v1: re-scan each session. Later nicety: remember paired devices. Never print the token in logs.
- **Rate limiting:** cap paired devices (4), cap inbound messages (~10/s per socket, drop connection on abuse), cap message size (a few KB). Ten lines of code; do them — this is a trust boundary.
- **Versioning:** hello message carries `{protocolVersion: 1, gameId, moduleSchemaHash}`. Locally it's dead weight (page ships with engine); it exists so the relay and any third-party client can negotiate later. One integer reserved now, no machinery.

Implementation: **cpp-httplib** — header-only, and as of 2026 it has server-side WebSocket support (RFC 6455) using blocking I/O, one thread per socket ([README-websocket.md](https://github.com/yhirose/cpp-httplib/blob/master/README-websocket.md)). Thread-per-connection is disqualifying for a web service and completely fine for ≤4 phones. Alternative if it disappoints: CivetWeb (embeddable C, WS support, heavier). Do not take uWebSockets' async complexity for four connections.

## 3. Engine-side API as product feature — the companion channel schema

Modules/games declare, in their existing data-driven manifest (no C++, same place NPC presets etc. live):

```yaml
companion:
  topics:
    map.players: { source: component:Transform, filter: per-session-visibility, rate: 2 }
  commands:
    inventory.move: { params: {from: slot, to: slot}, handler: action:inventory_move }
```

- **Topics** bind to state the replication system already knows how to serialize — companion export piggybacks on the netcode schema, it is not a second reflection system. The one engine-side requirement: the replication layer must be able to emit JSON from its schema, not only its binary wire form.
- **Commands** bind to the same script-defined actions mods already register for gameplay. They execute inside the existing mod sandbox with a PlayerId — hostile phone + hostile mod introduces no new attack-surface *class*, because both boundaries already exist.
- Genre modules extend naturally: RPG module ships `inventory.*` topics/commands, an RTS module ships `minimap.units` — same mechanism, zero new engine code.

**Honest value assessment:** as a purchase driver, modest — maybe one in ten prospective users will care that companion apps are declarative and free. No mainstream engine ships this out of the box, so it's genuinely differentiating and it demos spectacularly well (live map on a phone is trailer material for the *engine*). But its largest real value is internal discipline: it forces the clean "declared state out, validated commands in" boundary that netcode, modding, and save systems all want anyway. Build it because it's nearly free *on top of that boundary*; don't build the boundary for its sake.

## 4. Stack

- **Web app (v1):** one static HTML file, vanilla JS, Canvas for the map, no framework, no build step, embedded in the engine binary as an asset. A map and an inventory grid do not justify a Node toolchain in a repo a C++ dev touches quarterly. If the inventory UI outgrows this, vendor Preact as a single file — still no build step.
- **Backend (later, hybrid phase only):** **Cloudflare Workers + Durable Objects** as the relay — one DO per game session, WebSocket hibernation makes idle sessions free; free tier is ~3M requests/month, paid floor $5/month ([pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/), [WS guide](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)). Near-zero ops, near-zero cost, real TLS for free, and a relay is embarrassingly parallel — a perfect DO fit. Alternative: a Go binary behind Caddy on a $5 VPS (more control, but *you* are now on-call). Not chosen: anything with Kubernetes in the sentence.
- **Identity:** an opaque `PlayerId` with a provider behind it. v1: local profile GUID. Steam phase: SteamID64, verified server-side via [`AuthenticateUserTicket`](https://partner.steamgames.com/doc/webapi/isteamuserauth) (publisher key, must be called from your backend — which conveniently only exists once the relay does; the two arrive together).
- **GDPR:** v1 has no accounts, no server, no personal data leaving the LAN — nothing to comply with, and say so in the docs as a privacy feature. Relay phase: keep the relay *stateless about gameplay* — it forwards frames and stores only `(SteamID, paired-device tokens)`. Then account deletion is one row-delete endpoint and the privacy policy is a paragraph. Never persist gameplay/chat through the relay; the moment you store it, you own it.

## 5. Phasing

**Decide/reserve EARLY (during engine core & netcode — retrofitting these is expensive):**

1. **One command funnel.** Every sim mutation from every source — local input, network client, companion, dev console, future scripting — enters as a validated command tagged with a PlayerId. If server-authoritative netcode is built this way (it should be regardless), companion is free. If local input bypasses it, retrofit touches every gameplay system. *This is the expensive one.*
2. **Opaque PlayerId + identity provider seam.** Local GUID now, SteamID later. Leaks into saves, replication, and permissions — near-impossible to retrofit cheaply.
3. **Replication schema queryable at runtime and JSON-capable.** Needed for modding tooling anyway; companion topics are a consumer, not a driver.
4. **Manifest reserves a `companion:` key** (may be empty for the whole vertical slice) and the WS hello reserves `protocolVersion`. Cost: ~zero.
5. Save format: explicitly **not** on this list — companion reads live state, not saves. Don't couple them.

**Build LATE (post-vertical-slice):** the HTTP/WS embed, the HTML page, QR pairing, topic throttling, inventory commands. Later still, only on demand: relay, Steam identity, remembered devices. **Never:** native mobile apps — a browser tab is the product.

**Smallest impressive demo:** live world map on a phone — scan QR, watch both co-op players' dots move in real time at 2 Hz. Scope: cpp-httplib embed + one topic + one HTML page ≈ 1–2 weeks *given* the command funnel and replication schema exist. Slot it immediately after the vertical slice's co-op netcode stabilizes: it doubles as an integration test of the state-export boundary and is the best 30 seconds of any engine trailer. Inventory commands come second — they're the first real exercise of untrusted command injection, worth doing before any third party does it for you.

**Sources:** [cpp-httplib WebSocket README](https://github.com/yhirose/cpp-httplib/blob/master/README-websocket.md) · [cpp-httplib](https://github.com/yhirose/cpp-httplib) · [CivetWeb](https://github.com/civetweb/civetweb) · [Chrome Local Network Access](https://developer.chrome.com/blog/local-network-access) · [LNA explainer](https://github.com/WICG/local-network-access/blob/main/explainer.md) · [Steam AuthenticateUserTicket](https://partner.steamgames.com/doc/webapi/isteamuserauth) · [Steam auth overview](https://partner.steamgames.com/doc/features/auth) · [Durable Objects pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/) · [Durable Objects WebSockets](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)