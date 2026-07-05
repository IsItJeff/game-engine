# ADR-0020: MIT license, public repo

**Status:** accepted (2026-07).

The engine is MIT-licensed in a public GitHub repo, free forever. Game content (art, audio, campaign data, the shipped mods' assets) is proprietary; revenue is game sales on Steam. The engine/SDK lives on GitHub — not a Steam tool listing at v1. `THIRDPARTY-NOTICES` is CI-generated from vcpkg at M2 and the job fails on copyleft licenses entering the dependency graph.

Why: the game-first strategy (ADR-0001) means the engine's job is adoption and trust, not licensing revenue — MIT is the no-friction option for modders and the M10 experiment. A public repo also buys free GitHub Actions CI on three platforms, which the testing strategy depends on. The engine/content split keeps the commercial asset untouched while the platform stays open.

Rejected: GPL/LGPL (viral terms poison the SDK story and scare modders); source-available/BSL (trust tax with zero revenue upside at this scale); closed source (forfeits CI, contributions, and the M10 experiment entirely); dual licensing (bookkeeping for a market that doesn't exist yet).
