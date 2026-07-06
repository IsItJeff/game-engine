# Testing policy: three lanes, not blanket TDD

Strict test-first for the deterministic core (~25% of the code carrying ~80% of the risk),
test-after for glue, and **zero automated tests for look and feel** — manual play plus a
recorded demo video is the QA artifact for anything whose failure mode is "feels wrong."
The backbone is a Factorio-style determinism/replay harness (see below): golden replays
recorded at each milestone are the cheap regression net for everything the three lanes
cover. Honest cost: +10–15% total engine time, front-loaded — bought back at M5/M6, the
milestones where solo multiplayer projects die.

## Module → lane

| Module | Lane |
|---|---|
| Bitstream serializer | **TDD** — and it is the M0 C++ graduation kata (round-trip property `read(write(x)) == x`, malformed-input fuzz loop, under ASan/UBSan) |
| Command funnel | **TDD** (validation/rejection/ordering — it's the trust boundary) |
| Protocol/replication state machines | **TDD** + integration over a **deterministic fake transport** (scripted latency/loss/reorder/duplication); server survives arbitrary bytes |
| Save/load | TDD per migration + golden fixture saves per schema version (old saves kept in-repo forever) |
| Prediction (M5) | Client + server in one process over fake transport; assert convergence after induced misprediction under 100–300 ms latency + 5% loss |
| Character controller | Simulation harness: seeded-input invariants on golden terrain + golden replays; feel stays manual |
| Luau sandbox | **Hostile-mod fixture suite** (≥15 evil `.luau` files; every new binding lands with an abuse test; each documented limit proven by a test) |
| BT interpreter | TDD with stub leaves |
| Renderer/audio/animation | None automated; headless boot-smoke only. **No golden-image testing** (GPU/driver variance tar pit); revisit M8+ only via Linux lavapipe, canary-not-gate |
| Whole engine | Headless smoke (boot listen server, 600 ticks of scripted bot input, clean exit, on all 3 platforms); nightly 1-hour ASan soak from M5 |

## Determinism/replay harness (M2)

- **State hash**: a per-tick hash of sim state, computed via the same bitstream serializer
  that writes saves and the wire — one serialization system, three uses.
- **Record/replay**: command streams (the funnel's output) are recorded to disk and can be
  replayed headlessly.
- **The invariant**: same inputs twice ⇒ identical per-tick hashes. This gates CI
  **per-platform**; cross-platform hash comparison runs as a **monitored, non-gating
  nightly** (cross-OS float determinism is not a gate — see master plan, "Explicitly NOT
  building").
- **Golden replays**: re-recorded at each milestone exit, run in CI on every push. A hash
  divergence is a determinism regression before it is anything else.

## Standing rules

- **No coverage gates, ever.** The deterministic core sits around 90% organically and
  presentation code near 0% — both correctly. `llvm-cov` is used occasionally as a map,
  never as a bar.
- **Property testing via Catch2 generators.** No rapidcheck (unmaintained as of 2026); no
  extra property-testing dependency.

## When each testing asset arrives

| Milestone | Asset |
|---|---|
| M0 | Catch2 + CTest wired into CI; bitstream serializer TDD kata; this file |
| M2 | Determinism/replay harness (state hash, record/replay, CI determinism test); command funnel test suite |
| M3 | Deterministic fake transport; handshake/join/leave state-machine tests; malformed-packet fuzz |
| M4 | Character-controller simulation harness; first golden replay |
| M5 | Prediction convergence tests under simulated latency/loss; nightly 1-hour ASan soak begins |
| M6 | Hostile-mod fixture suite (≥15 fixtures, grows with every binding) |
| M7 | BT interpreter suite; headless hauling-throughput soak joins the harness |
| M8b | Golden fixture saves + migration suite; 100-run kill-process-during-save fuzz (zero corruption); golden replays re-recorded; 1-hour multi-client soak |
