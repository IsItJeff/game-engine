# ADR-0005: Predicted movement is C++, never scripted

**Status:** accepted (2026-07).

Client-side predicted movement is a C++ engine system configured by data. Mods script authoritative server logic and presentation, never anything on the predicted path. Only the local player's character is predicted; props and NPCs replicate via snapshot interpolation.

Why: prediction requires a pure `(state, input) → state` function that can be re-simulated N times per frame during reconciliation (this is also why the character controller is Jolt `CharacterVirtual` — ADR-0011). A Luau call inside that loop breaks determinism guarantees, blows the CPU budget, and would force mod code to be bit-identical across client and server — an impossible contract to offer modders. Keeping mods sim-authoritative-plus-presentation (the M6 acceptance mod is deliberately "clean of dynamics") means mod authors never touch the hardest problem in netcode.

Rejected: scriptable movement (see above); predicting scripted abilities (deferred indefinitely — server-authoritative with interpolation is the contract).

Fallback (pre-authorized, K3): if prediction/reconciliation stalls the project, ship interpolation-only with ~100 ms input delay.
