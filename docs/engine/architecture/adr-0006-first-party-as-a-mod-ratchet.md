# ADR-0006: First-party-as-a-mod is a ratchet, not a day-one rule

**Status:** accepted (2026-07).

First-party gameplay migrates onto the public mod API as a ratchet: each milestone from M6 onward (when the API first exists) moves at least one shipped feature onto the public API, into `mods/`. The rule is hard-enforced only when a real external modder exists.

Why the ratchet direction: building on your own public API proves it is complete (if the base game can't be built on it, neither can mods), makes breaking changes hurt us before they hurt users, and makes the base pack the best documentation — modders read shipping code, not toy samples. Why not day-one: before M6 there is no API to build on, and dogmatic dogfooding would block the slice — the game-first strategy (ADR-0001) outranks architectural purity.

Rejected: strict "everything is a mod from day one" (blocks progress pre-M6); never dogfooding (ships an API nobody, including us, has used in anger).

Revisit trigger: the first real external modder converts the ratchet into a hard rule.
