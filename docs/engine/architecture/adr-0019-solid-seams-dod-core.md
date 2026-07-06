# ADR-0019: SOLID at the seams, DOD at the core

**Status:** accepted (2026-07).

Interface-shaped design lives only at module seams (ITransport, the command funnel, `Serialize(Stream&)`); the simulation core is data-oriented. The binding rules, from the 12-rule charter in `code-design-rules.md`: an entity is an ID, never a class hierarchy; variation lives in data + scripts, not subclasses; hot loops are dispatch-free (no virtual calls, allocation, strings, or exceptions in per-entity per-tick code — virtual dispatch at boundaries crossed dozens of times per tick is fine); no interface without two real implementations today; quarantine replaceable libs (Jolt/GNS/miniaudio types never leave their module) but use vocabulary libs bare (GLM/EnTT/spdlog); systems are plain functions in one explicit ordered schedule — no ISystem base class, no self-registration; write the concrete thing first, abstract on the second use.

Why: the plan's seams already embody the useful parts of SOLID (ITransport = ISP/DIP/LSP, the funnel = SRP for state, ECS + JSON `extends` = composition/OCP) — this ADR stops the pattern from spreading into per-entity code where it costs frames and clarity (the Martin/Muratori consensus position).

Rejected: OOP entity frameworks (foreclosed by ADR-0010); dogmatic DOD everywhere (seams genuinely need substitution — three transports exist).

Enforcement: clang-tidy (`cppcoreguidelines-*`, `performance-*`, `modernize-*`), the ~20-line include-graph CI script (sim never includes gpu/platform), and the PR self-review checklist.
