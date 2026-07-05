# ADR-0011: Jolt physics; the character is CharacterVirtual

**Status:** accepted (2026-07).

Jolt is the physics library. The player character uses Jolt's `CharacterVirtual` — a kinematic controller that is re-simulable N times per frame — rather than a dynamic rigid body. Only the local character is predicted; props and other bodies replicate. Jolt types never leave `engine/physics/` (quarantine rule for replaceable libs).

Why: client prediction (ADR-0005) needs movement as a pure `(state, input) → state` function that reconciliation can re-run repeatedly; a dynamic-body character is coupled to the whole physics step and cannot be re-simulated in isolation. Jolt is modern, actively maintained (Horizon-proven), MIT, and has first-class character-controller samples.

Rejected: PhysX (integration heft, licensing gravity); Bullet (maintenance has moved on); dynamic rigid-body character (see above); hand-rolled character physics (K5).

Fallback (pre-authorized, K4): if controller feel stalls, ship the Jolt sample configuration as-is — the samples are the ground truth. Integration practice: copy samples, tune one parameter at a time, timeboxed.
