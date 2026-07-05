# ADR-0004: One command funnel for all sim mutation

**Status:** accepted (2026-07).

Every simulation mutation from every source — local input, networked clients, the companion phone app, the console, and mods — enters as a validated, PlayerId-tagged command through a single funnel. PlayerId is opaque: a local GUID now, SteamID64 later, with no code depending on its structure. Nothing mutates sim state outside the funnel (code-design rule 3).

Why: the funnel is the trust boundary (so it is built TDD — validation, rejection, ordering), the replication unit (commands are what the wire carries), the replay format (record/replay for the determinism harness), and the mod-safety choke point (a mod can only *ask*, the funnel decides). One mechanism serves security, netcode, testing, and modding simultaneously.

Rejected: direct state mutation from input/UI/scripts (unauditable, unreplayable, unreplicable); per-subsystem command paths (N trust boundaries instead of one).
