# ADR-0015: Luau is the modding language

**Status:** accepted (2026-07).

Mods are scripted in Luau: one VM per mod, frozen-global sandbox, VFS jail, ~64 MB memory cap, interrupt-based CPU budgets with a poison-flag `pcall` defense, no bytecode loading of any kind (the engine compiles mod *source*; mod-supplied bytecode is the classic Lua escape vector), and codegen disabled on macOS. "Mods are Luau-only — no filesystem/network/FFI" is a written security invariant, enforced by the hostile-mod fixture suite (≥15 evil `.luau` files; every new binding lands with an abuse test). The mod API is defined once in an IDL/JSON descriptor from M6 day one — generating binding checks and the MkDocs reference — semver'd independently, with deprecations warning one minor version before removal.

Why Luau: it is the only embeddable language engineered against hostile code as its founding requirement (Roblox-hardened, funded security team), while staying teen-accessible.

Honest limit, never to be softened in marketing: the sandbox is containment, not OS-level isolation. Never market it as "secure." The claim is exactly: a friend adds an enemy with a JSON file + 20 lines of Luau, hash-verified into the co-op session, and a bad mod can't crash the game or corrupt saves. A memory-safety zero-day in the Luau VM means arbitrary code execution in the game process; dedicated servers running strangers' mods should run in a container/VM.

Rejected: LuaJIT (Apple Silicon/maintenance risk); Wasm tier (strongest containment but needs a compiler toolchain — deferred until real modders demand it; the IDL makes the binding mechanical later); QuickJS (JS is not the game-modding lingua franca); native C++ plugins (never — security invariant).
