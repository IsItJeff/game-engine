# Code design rules

**SOLID at the seams, DOD at the core.** Interface discipline belongs at module boundaries
(transport, platform, script sandbox) where substitution is real; data-oriented design
belongs in the per-tick core where dispatch and allocation cost frames. These 12 rules are
the charter; the master plan's locked decisions already embody them.

## The 12 rules

1. **An entity is an ID.** Capabilities are components; no entity class hierarchies, ever.
   *Why:* hierarchies calcify the first taxonomy you guessed. *Prevents:*
   `class Enemy : public Character`.
2. **Variation lives in data + scripts, not subclasses.** *Why:* new content must not
   require recompiling the engine. *Prevents:* a C++ subclass per enemy/item/quest type.
3. **All sim mutation goes through the command funnel.** *Why:* one validated, PlayerId-tagged
   entry point is what makes multiplayer, replay, and mod security possible. *Prevents:*
   systems poking world state directly from input handlers, network callbacks, or scripts.
4. **Hot loops are dispatch-free.** No virtual calls, allocation, strings, or exceptions in
   per-entity per-tick code (virtual dispatch at boundaries crossed dozens of times per tick
   is fine). *Why:* per-entity costs multiply by entity count × 60 Hz. *Prevents:*
   `ISystem::update()` virtual per entity, `std::string` keys in the tick.
5. **No interface without two real implementations today.** *Why:* a one-impl interface is
   speculation with a maintenance bill. *Prevents:* `IRenderer` wrapping the only renderer.
6. **Quarantine replaceable libs; use vocabulary libs bare.** Jolt/GNS/miniaudio types never
   leave their module; GLM/EnTT/spdlog are used directly. *Why:* wrap what might be swapped,
   not the lingua franca. *Prevents:* both leaked physics types everywhere and a
   `Vec3`-wrapper layer over GLM (K5 astronautics).
7. **Every resource is RAII; no naked `new`/`delete`.** *Why:* leaks and double-frees are
   solved problems. *Prevents:* manual lifetime bookkeeping and cleanup-order bugs.
8. **Values by default; `unique_ptr` owns; `shared_ptr` needs a justifying comment.**
   *Why:* ownership you can read is ownership you can debug. *Prevents:* shared-ownership
   spaghetti where nothing knows what frees what.
9. **Module includes point one way** — sim never includes gpu/platform. *Why:* layering is
   only real if it is mechanically checked. *Prevents:* the headless dedicated server
   quietly linking the renderer.
10. **Seam implementations pass one shared test suite.** All 3 transports run the same
    tests; every wire type round-trips. *Why:* an interface is a contract only if every
    impl is held to it. *Prevents:* loopback working where GNS subtly doesn't.
11. **Systems are plain functions in one explicit ordered schedule.** No `ISystem` base
    class, no self-registration. *Why:* tick order is a correctness property you must be
    able to read in one place. *Prevents:* registration-order bugs and framework ceremony.
12. **Write the concrete thing first; abstract on the second use.** *Why:* the second use
    tells you the shape of the abstraction; the first is a guess. *Prevents:* frameworks
    built for callers that never arrive.

## Enforcement

Three mechanisms; anything they can't check is a judgment call caught by the checklist.

### clang-tidy (`.clang-tidy`, mirrors rules 7–8)

```
Checks: -*,
  cppcoreguidelines-special-member-functions,
  cppcoreguidelines-owning-memory,
  cppcoreguidelines-no-malloc,
  cppcoreguidelines-virtual-class-destructor,
  performance-*,
  modernize-use-nullptr, modernize-use-override, modernize-loop-convert,
  bugprone-use-after-move, bugprone-dangling-handle
HeaderFilterRegex: (engine|game|tools)/.*
```

### Include-graph check (rule 9)

`scripts/check_includes.py` — a ~20-line CI script that parses `#include` lines under
`engine/` and `game/` and fails the build if any include edge points against the layering
(e.g. anything in sim code including gpu/platform headers).

### PR self-review checklist (the solo substitute for a reviewer)

Ask on every self-PR, verbatim:

> new base class — why not a component? new interface — where's the second impl?
> virtual/alloc in a tick loop? mutation outside the funnel?
