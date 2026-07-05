# ADR-0001: Game-first strategy — the engine is earned through the game

**Status:** accepted (2026-07).

We are building a moddable co-op embodied colony-sim; the engine is the long-term product, earned through the game. Every technical decision is judged by whether it ships the game slice, not by engine-product ambitions. "Engine as product" becomes a time-boxed 60-day experiment at M10 (mod-docs polish, template repo, "a stranger builds a mod using only the docs"). No takers means the game is the product and the experiment closes as *answered*, not failed.

The load-bearing why: "moddable co-op 3D engine" is not a credible engine pitch from a solo first-time developer, but the same capabilities are a credible and underserved *game* pitch. The game provides the wedge, the proof, and the revenue (MIT engine, proprietary game content, Steam sales — see ADR-0020).

Rejected: engine-first development (no adoption wedge, no revenue, the classic solo-engine death spiral); Steam tool listing at v1 (engine/SDK stays on GitHub).

Revisit trigger: M10 outcome decides platform investment. Off-ramp: M7 public demo draws zero interest AND >24 months elapsed → re-scope to ship-the-game, archive platform ambitions.
