# ADR-0017: Exceptions contained at boundaries, tl::expected across them

**Status:** accepted (2026-07).

Exceptions are contained at subsystem boundaries and converted to `tl::expected` there; gameplay code never writes `try`; invariant violations use `ENGINE_ASSERT`. Hot per-entity per-tick loops contain no exceptions at all (code-design rule 4: no virtual calls, allocation, strings, or exceptions in hot loops).

Why: one simple rule a C++ learner can apply everywhere without judgment calls. Dependencies (nlohmann, the standard library) throw, so exceptions cannot be disabled globally — but letting them propagate through sim code makes control flow invisible and determinism unauditable. `expected` at the seam makes every fallible boundary call explicitly handled at the call site, which is also where modder-facing error messages (schema path/expected/actual) get produced.

Rejected: exceptions everywhere (invisible control flow in sim code, banned from hot loops anyway); `-fno-exceptions` (fights every dependency); error codes everywhere (loses payload and composability that `expected` keeps).

Revisit trigger: `std::expected` replaces `tl::expected` when all three CI toolchains ship it complete — mechanical rename, same rule.
