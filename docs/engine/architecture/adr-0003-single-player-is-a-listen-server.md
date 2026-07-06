# ADR-0003: Single-player is a listen server over loopback

**Status:** accepted (2026-07).

Single-player runs a client plus an embedded server communicating over the loopback transport. There is exactly one sim code path for single-player, listen server, and dedicated server; no `if (offline)` branch exists anywhere in gameplay code. The loopback transport supports simulated latency and loss, so networked behavior is testable without a second machine.

Why: two sim code paths diverge silently — multiplayer bugs then only reproduce in multiplayer, which a solo developer tests least. Paying the client/server split early (M3, "before it's expensive") makes every subsequent feature multiplayer-correct by construction. The headless `game/ded` target linking without gpu/audio is the CI proof that the layering holds.

Rejected: offline fast path with networking bolted on later (the graveyard pattern for co-op retrofits); peer-to-peer/lockstep (explicitly out of scope alongside rollback netcode).
