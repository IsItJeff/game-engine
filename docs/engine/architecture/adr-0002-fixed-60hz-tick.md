# ADR-0002: Fixed 60 Hz simulation tick

**Status:** accepted (2026-07).

The simulation advances at a fixed 60 Hz tick; rendering interpolates between the last two sim states; the accumulator's dt is clamped at ~250 ms so a debugger pause or hitch cannot produce a spiral of death. Input is data: an action-mapping layer produces tick-stamped `InputCommand` values, never direct polling inside sim code.

Why this is load-bearing: the determinism/replay harness (state hashes, record/replay, same-inputs-twice ⇒ identical per-tick hashes — ADR-0018), client prediction/reconciliation (ADR-0005), and staggered NPC think scheduling (each NPC thinks at ~5–10 Hz round-robin inside the tick) all require a fixed, integer-addressable timeline. Tick-stamped commands are also the replication unit for the command funnel (ADR-0004).

Rejected: variable timestep (non-deterministic, kills replay and prediction); semi-fixed timestep (same problem in disguise); time acceleration in multiplayer (a shared world can't pause — night-skip is a sleep vote instead).
