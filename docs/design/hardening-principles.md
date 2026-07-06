# SOLID, OOP, and Data-Oriented Design — the researched synthesis for this engine

## Recommendation (one position)

**Adopt "SOLID at the seams, DOD at the core."** SOLID enters this project as *module-boundary discipline*, not class-design dogma. Entity/gameplay state stays pure ECS (locked). Engine services stay RAII/OOP. The plan's existing seams (ITransport, command funnel, `Serialize(Stream&)`) already *are* the useful parts of SOLID — the work is to write down the rules so a C++ learner doesn't import the harmful parts from mainstream C++ literature. **No locked decision changes.**

Honest cons of this position: (a) mainstream C++/SOLID books will contradict the charter, so the handbook must pre-empt the conflict; (b) "no interface until a second implementation exists" occasionally forces a later refactor — accepted, because refactoring a concrete class is cheaper than maintaining a wrong abstraction; (c) several charter rules aren't machine-checkable and depend on self-review discipline, the weakest link for a solo dev.

---

## 1. The real debate

**Nystrom's Component chapter** is the canonical statement of the trap you've already avoided: a `GameObject` base class grows a hierarchy (Zone adds collision, Decoration adds rendering, Prop needs both) and hits the deadly-diamond wall; components dissolve the hierarchy into composable parts ([gameprogrammingpatterns.com/component.html](https://gameprogrammingpatterns.com/component.html)). EnTT is this pattern industrialized. Key nuance: Nystrom's argument is *structural* (inheritance can't express orthogonal capabilities), independent of performance.

**Acton (CppCon 2014)** and **Fabian's DOD book** make the *performance* argument: programs exist to transform data; design for the many-case, not the one-case; OOP organizes memory around a real-world mental model instead of around access patterns, and cache misses cost 10x+ ([Acton's talk](https://www.youtube.com/watch?v=rX0ItVEVjHc), [dataorienteddesign.com/dodbook](https://www.dataorienteddesign.com/dodbook/)). Fabian's "existence-based processing" — process only entities that have the component, eliminating per-entity branching ([node4](https://www.dataorienteddesign.com/dodmain/node4.html)) — is exactly what an EnTT view gives you for free. Your 60 Hz tick over networked entities is squarely their target domain.

**Muratori's "Clean Code, Horrible Performance"** ([computerenhance.com](https://www.computerenhance.com/p/clean-code-horrible-performance)) showed 20–25x slowdowns from Clean-Code-style polymorphism-per-shape versus a switch/table over flat data. The strongest counters: [Evan Teran](https://blog.codef00.com/2023/04/13/casey-muratori-is-wrong-about-clean-code) argues the measured cost is *virtual dispatch in a tight loop*, not "clean code" broadly — factoring, naming, and small modules cost nothing; and in the [Martin–Muratori dialogue](https://github.com/unclebob/cmuratori-discussion/blob/main/cleancodeqa.md), Martin largely concedes the performance point while both converge on: **dispatch cost matters where call count is high and per-call work is small; it's noise at boundaries crossed once per frame.** That sentence is the whole synthesis, and it maps directly onto your architecture: virtual calls in per-entity tick loops = forbidden; virtual call on `ITransport::Send` a few dozen times per tick = irrelevant.

**Modern C++ (CppCon) guidance** is not on OOP-dogma's side either. Sean Parent's ["Inheritance Is the Base Class of Evil"](https://sean-parent.stlab.cc/papers-and-presentations/) and Klaus Iglberger's [Modern C++ Software Design](https://meetingcpp.com/mcpp/training/trainingslisting.php?tid=2) teach value semantics, composition over inheritance, and RAII as C++'s actual superpowers. So "introducing OOP properly" in 2026 C++ already means mostly what your plan does — the community moved past inheritance-centric design a decade ago.

**Shipping engines confirm the split.** Unreal keeps UObject/Actor OOP as the *authoring* model but built [MassEntity](https://dev.epicgames.com/documentation/en-us/unreal-engine/mass-entity-in-unreal-engine) — archetype ECS, fragments with no virtual methods or UObject overhead, processors over cache-friendly chunks — because Actors stop scaling in the thousands. Godot inverts it: an OOP node tree as the user-facing API, but the internals are data-oriented *servers* (RenderingServer, PhysicsServer); their own [explainer](https://godotengine.org/article/why-isnt-godot-ecs-based-game-engine/) admits ECS's linear-memory systems bring "huge performance improvements" and defends nodes purely on usability grounds. **Lesson: every serious engine separates a friendly authoring surface from a data-oriented simulation core.** Your plan already has this shape — EnTT + command funnel inside, JSON + Luau as the authoring surface.

One fairness note from the gamedev debate ([OOP is dead, long live OOP](https://gamedev.net/blogs/entry/2265481-oop-is-dead-long-live-oop/)): ECS advocacy often strawmans OOP — deep inheritance was *already* an anti-pattern by OOP's own rules ("prefer composition" is in the 1994 GoF book). So SOLID's underlying intents survive translation; only the class-centric idioms don't.

---

## 2. SOLID, translated honestly for this codebase

**SRP — Single Responsibility → module boundaries, not tiny classes.**
Transfers as: one subsystem = one reason to change. `engine/net` knows nothing about rendering; `engine/gpu` knows nothing about Jolt; `engine/sim` is deterministic and platform-free (that's also what makes the headless-Linux CI target work). The command funnel is SRP for *state*: exactly one place mutates the sim.
Dogma to reject: "every class does one thing" pulverized into 50-line class confetti with dependency-injected everything. SRP here operates at the directory level.

**OCP — Open/Closed → extend via data and scripts, never via inheritance.**
Transfers as: new enemy = new JSON (with `extends` for data inheritance) + Luau server logic + BT graft. Zero engine recompile, zero new C++ types. The first-party-gameplay-as-a-mod ratchet is OCP made structural: if you can't build it through the mod API, the *API* is what gets extended.
Dogma to reject: OCP via virtual hook methods and template-method base classes — that's how Nystrom's GameObject hierarchy forms.

**LSP — Liskov Substitution → only where interfaces exist, and there it's load-bearing.**
`ITransport` (Loopback / GNS / Steam) is the poster child: all three must honor identical delivery, ordering, and error semantics or milestone-5 prediction code will work on loopback and break on real networks. Practical consequences: (1) run the *same* transport test suite against all implementations; (2) loopback must support simulated latency/loss, because a loopback that's "perfect" is a semantic LSP violation waiting to bite. Same logic applies to `Serialize(Stream&)`: read and write are the same function, so they can't drift.
Dogma to reject: none — LSP is fully valid; it just applies to ~3 seams in the whole codebase.

**ISP — Interface Segregation → your 6-function transport IS the principle.**
Small, purpose-built seams: 6 functions for transport, one function signature for serialization, one command type for mutation. Clients depend on exactly what they use.
Dogma to reject: fat `ISystem`/`IEngine` base classes that every subsystem must implement; "manager" interfaces with 30 methods.

**DIP — Dependency Inversion → gameplay depends on seams, not on vendors.**
Transfers as a concrete include rule: **third-party types don't cross module boundaries.** Jolt types stay inside `engine/physics` (the sim sees positions/velocities as components); GNS types stay behind `ITransport`; miniaudio handles stay in `engine/audio`. Exception, stated explicitly: **vocabulary libraries — GLM, EnTT, spdlog, nlohmann at asset-load boundaries — are used directly everywhere.** Wrapping GLM in `MyVec3` or EnTT in `IEntityManager` is precisely the K5 "engine astronautics" killer: an abstraction with one implementation, built for a swap that will never happen, taxing every line forever.

**Where dogmatic SOLID actively hurts here** (the list to print out):
- Virtual `update()` per entity per tick — Muratori's measured 20x, in your hottest loop.
- Interface + factory for a single implementation — pure ceremony (K5).
- "We might swap the physics engine" abstraction layers — you won't; Jolt is locked; the quarantine rule gives you 90% of the swap-safety for 0% of the tax.
- DI containers / self-registering systems — magic a learner can't debug at 3am; an explicit ordered list of system calls in `main` is better in every way.
- Exceptions as control flow in the tick — fine at load time, banned in the loop.

---

## 3. Where OOP belongs, and where it's banned

**OOP/RAII belongs (this is *why* C++):**
- **Subsystem lifetimes**: `GpuDevice`, `AudioEngine`, `NetHost` — construct-acquire, destruct-release, non-copyable, movable. Startup/shutdown ordering becomes declaration ordering.
- **Resources**: GPU buffers, sounds, sockets, file handles — `std::unique_ptr` with custom deleters or small RAII wrappers. No `new`/`delete` in application code, ever.
- **Cold-path polymorphism**: the 3 transports, debug UI panels, tool/editor code — few objects, called rarely; virtual dispatch is free at this frequency (the Martin–Muratori consensus point).
- **Value types**: commands, snapshots, component structs — plain data, copyable, `Serialize`-able (Parent/Iglberger's value-semantics guidance).

**OOP is banned:**
- **Entity/gameplay state.** State the rule verbatim in the charter: *"No entity class hierarchies — ever. An entity is an ID. Variation comes from component composition + JSON data inheritance (`extends`) + Luau/BT behavior. The words `class Enemy : public Character` never appear in this codebase."*
- **Systems as objects.** Systems are free functions (or stateless structs) over EnTT views, invoked from an explicit, ordered schedule. No `ISystem` base class, no virtual `Update()`, no registration framework — the standard EnTT idiom, and it makes tick order greppable.

---

## 4. Deliverable: the Code Design Rules charter

**`docs/engine/code-design-rules.md` — ~1 page, numbered, learner-applicable:**

1. **An entity is an ID.** No entity base classes or hierarchies; capabilities are components. *Why:* orthogonal capabilities can't be expressed in one tree. *Prevents:* the GameObject deadly diamond.
2. **Variation lives in data and scripts, not subclasses.** New content = JSON (`extends`) + Luau + BT grafts. *Why:* extension without recompilation is the moddability promise. *Prevents:* engine recompiles per enemy type; C++ leaking into content.
3. **All sim mutation goes through the command funnel.** No system writes another system's state directly. *Why:* one choke point = replayable, serializable, cheat-checkable. *Prevents:* untraceable state corruption; netcode that can't replay.
4. **Hot loops are dispatch-free.** Per-entity per-tick code: no virtual calls, no allocation, no strings, no exceptions. *Why:* dispatch+cache misses cost 10–25x exactly here. *Prevents:* the Muratori benchmark, live in your tick.
5. **No interface without two real implementations, today.** `ITransport` (3 impls) is the model; a second impl earns the interface, not a hunch. *Why:* single-impl abstractions are pure tax. *Prevents:* K5 engine astronautics.
6. **Quarantine replaceable libs; use vocabulary libs bare.** Jolt/GNS/miniaudio types never leave their module; GLM/EnTT/spdlog are used directly everywhere. *Why:* isolation where leakage hurts, zero ceremony where it doesn't. *Prevents:* both vendor lock-in *and* wrapper bloat.
7. **Every resource is RAII.** Acquire in constructor, release in destructor; rule of zero, else rule of five; no naked `new`/`delete`. *Why:* leaks and half-init states become impossible by construction. *Prevents:* shutdown-order bugs, GPU handle leaks.
8. **Prefer values; `unique_ptr` for ownership; `shared_ptr` needs a justifying comment.** *Why:* clear ownership is most of C++ correctness. *Prevents:* ownership spaghetti, accidental ref-count webs.
9. **Module dependencies point one way.** `sim` never includes `gpu`/`platform`; module include-graph is documented and CI-checked. *Why:* keeps sim deterministic and headless-testable. *Prevents:* the everything-includes-everything ball.
10. **Seam implementations pass one shared test suite.** Same tests run against Loopback and GNS; `Serialize` round-trips every wire type. *Why:* substitutability tested, not assumed (LSP, executable). *Prevents:* "works on loopback" milestone-5 bugs.
11. **Systems are plain functions in an explicit ordered schedule.** No system base class, no self-registration. *Why:* tick order is the spec of the simulation; keep it in one readable place. *Prevents:* framework magic, nondeterministic ordering.
12. **Write the concrete thing first; abstract on the second use.** Duplication once is cheaper than the wrong abstraction. *Why:* you can't design a seam you haven't felt twice. *Prevents:* speculative generality (K5 again — it's the killer worth two rules).

**Enforcement:**
- *clang-tidy (already in the stack’s toolchain via CI):* `cppcoreguidelines-special-member-functions` (rule 7; [docs](https://clang.llvm.org/extra/clang-tidy/checks/cppcoreguidelines/special-member-functions.html)), `cppcoreguidelines-owning-memory` + `cppcoreguidelines-no-malloc` (rules 7–8), `cppcoreguidelines-virtual-class-destructor`, `performance-*`, `modernize-*`, `misc-include-cleaner`. Rules 1, 2, 5, 11, 12 are judgment calls — not machine-checkable.
- *CI layering check:* a ~20-line script grepping `#include` across module directories against an allowlist (rules 6, 9). Cheaper and clearer than any tooling dependency.
- *PR self-review checklist* (solo dev's substitute for a reviewer, in the PR template): "New base class — why isn't this a component/data? New interface — where's the second impl? New wrapper — vocabulary or quarantine? Virtual/alloc in a tick loop? New mutation path outside the funnel?"

**Handbook pages this generates** (fits the existing MkDocs learning-handbook push):
1. *SOLID in game engines — what transfers and what doesn't* (this document, expanded; pre-empts the conflicting-advice problem).
2. *Composition over inheritance* (Nystrom's trap → components → ECS as the industrial version).
3. *Data-oriented design basics* (cache lines, SoA, existence-based processing, "design for the many-case").
4. *RAII — the reason C++ exists* (rules 7–8 with before/after examples).
5. *The seams of this engine* (transport, command funnel, serializer — the three interfaces and why there are only three).
6. *Value vs reference semantics* (Parent/Iglberger distilled for a learner).

---

## 5. Does this change any locked decision? **No.**

The plan's seams already embody every part of SOLID that survives contact with a data-oriented engine: **ITransport** is ISP+DIP+LSP in six functions; the **command funnel** is SRP+DIP for state; **EnTT ECS + JSON `extends`** is composition-over-inheritance and OCP done the way Unreal Mass and Godot's servers validate; the **mod-API ratchet** is OCP as a project rule rather than a class pattern. Introducing SOLID as class-level dogma would fight the ECS, re-open the K5 killer, and slow the hot path — so we don't. Introducing it as boundary discipline changes nothing because the boundaries were already drawn correctly; the only new artifacts are the charter, the CI/tidy enforcement, and six handbook pages, all inside the existing M0 docs scope.

**Sources:** [Nystrom — Component](https://gameprogrammingpatterns.com/component.html) · [Acton — DOD & C++, CppCon 2014](https://www.youtube.com/watch?v=rX0ItVEVjHc) · [Fabian — DOD book](https://www.dataorienteddesign.com/dodbook/), [existence-based processing](https://www.dataorienteddesign.com/dodmain/node4.html) · [Muratori — "Clean" Code, Horrible Performance](https://www.computerenhance.com/p/clean-code-horrible-performance) · [Martin–Muratori discussion](https://github.com/unclebob/cmuratori-discussion/blob/main/cleancodeqa.md) · [Teran — Muratori is wrong (but also right)](https://blog.codef00.com/2023/04/13/casey-muratori-is-wrong-about-clean-code) · [Parent — papers & talks](https://sean-parent.stlab.cc/papers-and-presentations/) · [Iglberger — Modern C++ Software Design](https://meetingcpp.com/mcpp/training/trainingslisting.php?tid=2) · [Unreal MassEntity docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/mass-entity-in-unreal-engine) · [Godot — Why isn't Godot an ECS?](https://godotengine.org/article/why-isnt-godot-ecs-based-game-engine/) · [OOP is dead, long live OOP](https://gamedev.net/blogs/entry/2265481-oop-is-dead-long-live-oop/) · [clang-tidy special-member-functions](https://clang.llvm.org/extra/clang-tidy/checks/cppcoreguidelines/special-member-functions.html) · [Rating SOLID for game dev](https://banalitywars.com/2020/02/06/rating-the-solid-principles-for-game-development/)