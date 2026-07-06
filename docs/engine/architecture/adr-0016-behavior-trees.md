# ADR-0016: Behavior trees for NPC decision-making

**Status:** accepted (2026-07).

NPC decisions use behavior trees: C++ owns the tick loop and structural nodes (selector/sequence/decorator/parallel); leaves are built-in C++ actions or Luau functions; trees are authored in JSON with `extends`/`insert_before` grafting so mods can modify shipped trees without copying them. Sensors and the blackboard are C++. The BT interpreter is built TDD with stub leaves (M7).

Why: BTs are composable in data, moddable by graft, and — decisively for this project — inspectable: "why did it do that?" has a visual answer a novice (or a 15-year-old modder) can find by looking at the tree. That debuggability requirement is the tiebreaker.

Rejected: FSMs (tangle past ~6 states, don't compose for reuse); utility AI (powerful but nearly impossible for a novice to author or debug — no visual answer to "why"); GOAP/HTN planners (same opacity, more machinery).
