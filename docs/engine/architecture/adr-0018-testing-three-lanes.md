# ADR-0018: Testing in three lanes, not blanket TDD

**Status:** accepted (2026-07).

Testing runs in three lanes: strict test-first for the deterministic core (bitstream, command funnel, protocol state machines, save migrations, BT interpreter — ~25% of code carrying ~80% of risk); test-after for glue; zero automated tests for look/feel, where manual play plus a recorded demo is the QA artifact. The backbone is a Factorio-style determinism/replay harness (M2): state hashes via the bitstream serializer, record/replay of command streams, same-inputs-twice ⇒ identical per-tick hashes, gating CI per-platform. Cross-platform hash comparison is a monitored non-gating nightly — cross-OS determinism is explicitly not a gate. Golden replays per milestone are the regression net.

Why: solo multiplayer projects die at M5/M6 (netcode and modding); the harness and the TDD core are aimed exactly there. Honest cost, stated up front: +10–15% total engine time, front-loaded.

Rejected: blanket TDD (tests for look/feel are theater); coverage gates, ever (core sits ~90% organically, presentation ~0%, both correctly); golden-image render tests (GPU/driver variance tar pit — revisit M8+ only via Linux lavapipe, canary-not-gate); rapidcheck (unmaintained — property tests use Catch2 generators).

The full module-by-lane table lives in `TESTING.md`.
