All research verified. Delivering the report as my final output.

# Modding & Product Strategy — Verdicts First

**TL;DR:** Embed **Luau** (Roblox's open-source Lua dialect) as the single v1 scripting tier. Run one VM per mod, whitelist-only API, VFS jail, allocator-enforced memory caps, interrupt-based CPU budgets. Build first-party gameplay as a mod on the public API from day one. Behavior trees for NPCs, JSON-with-inheritance for NPC data. Folders + zips for distribution; design for Workshop, don't build it. Product truth: **the game is the wedge, not the engine** — "engine as product" gets earned later or not at all.

---

## 1. Runtime choice

| Candidate | Hostile-mod sandbox | Solo burden | 15-yr-old modder | Per-tick perf | macOS friction | Hot reload |
|---|---|---|---|---|---|---|
| **Luau** | **Best-in-class**: designed for millions of hostile Roblox scripts; documented safe-embedding guide; Roblox-funded security team + bounty | Low: C++ codebase, CMake, MIT, active | Excellent — Lua syntax, gradual typing catches errors, huge Roblox tutorial ecosystem | Fast interpreter (beats PUC-Lua ~2-3x), optional codegen | None (interpreter) | Easy |
| Lua 5.4 (PUC) | OK if you forbid bytecode + whitelist `_ENV`, but no budgets built in; escape history is all bytecode-loader abuse | Lowest | Good | Adequate | None | Easy |
| LuaJIT | **Bad**: JIT = huge attack surface; known bytecode escapes; effectively single-maintainer, "hasn't had time in years" per Luanti issue | Risky | Good | Fastest | **Bad**: MAP_JIT entitlements, hardened-runtime bugs, tiny arm64 JIT region ([LuaJIT #1280](https://github.com/LuaJIT/LuaJIT/issues/1280)) | Easy |
| QuickJS-ng | Decent (interrupt handler, no ambient I/O); active (0.15.0, May 2026) | Low-medium | JS familiarity is real, but JS is not the game-modding lingua franca | Slower than Luau for hot loops; GC pauses | None | Easy |
| Wasm (wasmtime) | **Strongest containment**: fuel metering, hard memory limits, memory-safety by construction; Component Model production-ready in 2026 | **High**: WIT interfaces, bindings per language, toolchain support | **Terrible** — needs a Rust/C/Go compiler toolchain; kills casual modding | Excellent | Library only, fine | Painful |

**Verdict: Luau.** It is the only option that is simultaneously (a) engineered against hostile code as its founding requirement, (b) accessible to teenagers (Roblox trained a generation of them), (c) fast enough for per-tick NPC scripts without a JIT, and (d) zero macOS notarization pain. Adoption outside Roblox (Alan Wake 2, Warframe, Farming Simulator 25, Second Life's SLua in Dec 2025) confirms it's a safe embed bet, not a Roblox-internal artifact.

**Honest security limits (put this in the docs verbatim):** Luau runs *in-process*. The containment promise is: mods cannot touch the filesystem, network, or OS except through APIs we expose; runaway CPU/memory is throttled; a buggy or casually-malicious mod cannot corrupt saves or other mods. The promise is **not** OS-level isolation — a memory-safety zero-day in the Luau VM means arbitrary code execution in the game process. Nobody embedding an in-process VM can promise more; Luau merely has the best-funded defense of any embeddable language. Mitigation guidance: players install mods they chose; dedicated servers running strangers' mods should run in a container/VM.

**Second tier:** Wasm components (wasmtime) for compiled, perf-critical, or strongly-isolated mods. **Add only when** real modders hit Luau's perf ceiling or server hosts demand hard isolation — not in v1. The one v1 obligation: keep the mod API definable as an IDL-ish surface (it must be anyway for docs), so a Wasm binding is mechanical later.

---

## 2. Sandbox design (concrete)

- **One Luau VM (`lua_State`) per mod.** No shared globals between mods; inter-mod communication only via the engine event bus. A misbehaving mod's VM is destroyed wholesale. Costs a few hundred KB per mod — cheap.
- **Environment:** per-VM global table whose `__index` points at a `table.freeze`-frozen builtin table (the pattern from [luau.org/sandbox](https://luau.org/sandbox/)). Exposed: `math`, `string`, `table`, `bit32`, safe subset of `os` (`clock`/`time`/`date`), and the engine API (`game`, `events`, `storage`, `npc`, …). Removed: `io`, `os.execute`, `require` (replaced by engine module loader scoped to the mod's own files), `loadstring`/bytecode loading of any kind — engine compiles mod *source* with its own Luau compiler; mod-supplied bytecode is never executed (this is the classic Lua escape vector).
- **VFS jail:** mods address only `mod://<id>/…` (read-only, their package) and dependency packages read-only. Path resolution happens in C++: reject absolute paths, `..`, symlinks. No raw file handles ever cross into script.
- **Memory:** per-VM custom allocator with a hard cap (default 64 MB, manifest can request more, user/server approves). Allocation over cap fails → Lua error → mod error policy below. Kills `string.rep(1e9)` bombs.
- **CPU:** Luau's interrupt callback fires on loop back-edges and calls; check a per-slice deadline (e.g. 2 ms/tick/mod, configurable). On breach: raise an error the mod cannot `pcall`-swallow — set a "poisoned" flag checked in the interrupt so re-entry keeps unwinding, then disable the offending handler. **Known evasion tricks to cover:** `pcall` wrapping the budget error (poison flag defeats it); long-running *C* functions that never hit the interrupt — audit every exposed C function for bounded runtime, and cap Lua pattern-matching complexity (Luau already bounds some; test `string.find` with pathological patterns); timer/event spam (cap subscriptions and queued events per mod); allocation inside the error handler (pre-reserve).
- **Persistence:** `storage.set(key, value)` / `get` — JSON-serializable values only, per-mod namespace, quota (default 10 MB), engine flushes atomically with the save file. No direct file writes, so saves can't be corrupted and cloud sync stays sane.
- **Repeated errors:** every script entry is wrapped. An error logs (with mod id + stack), fires a UI toast in dev mode. N consecutive errors from one handler in a window (say 5 in 10 s) disables that handler; a mod exceeding a total error budget is suspended for the session with a visible banner. Errors never crash the process. Server decides suspension in multiplayer and replicates it.
- **Multiplayer mod matching:** server's join handshake sends its mod list: `(id, version, SHA-256 of the canonical mod zip)`. Client must present identical hashes for every mod not flagged `client_only`; mismatch = clear rejection message naming the offending mod. `client_only` mods (UI skins, audio) load freely. Because the sim is server-authoritative, a modified client can only desync itself out of the game, not cheat the sim — hash matching is about compatibility and honesty, not a security boundary (never trust the client regardless).

---

## 3. Mod API design

- **Versioning:** semver on the engine; a single integer **API level** on the mod API (Factorio-style). Manifest declares `"api": 3`. Engine supports level N and N−1 (with deprecation shims + warnings), drops N−2. Pre-engine-1.0: API level may bump every minor release, breaking changes documented with migration notes — say this loudly and don't apologize. The *stability contract* ("N−1 supported, 6-month deprecation") activates at engine 1.0.
- **Entity handles:** 64-bit opaque handles (index + generation), never pointers. `entity:isValid()`; every op on a stale handle is a no-op returning `nil`, plus a dev-mode warning. This survives hot reload, replication, and save/load, and it's the only design that doesn't let scripts crash the engine.
- **Client/server tier split:** a mod declares up to three entry points — `server.luau` (authoritative sim: spawning, damage, inventory, quests), `client.luau` (presentation: UI, VFX, audio, prediction hints; sees read-only replicated state), `shared.luau` (pure data/functions both load). Server→client and client→server messaging via a typed, rate-limited `net.send/on` channel per mod. Genre logic lives server-side by default; the API docs teach this split as the *only* pattern.
- **Event model:** engine-emitted named events (`events.on("entity_damaged", fn)`) with typed payloads; per-entity component lifecycle callbacks; timers (`timer.every(0.5, fn)`). `on_tick` exists but docs steer modders to events/timers (Factorio's #1 perf lesson). Handler order = load order; deterministic, documented.
- **Hot reload:** dev-mode file watcher → tear down the mod's VM, recreate, re-run `on_load`, rebind handlers. Rule that makes this trivial: **VM state is disposable** — durable state lives in components and `storage`, never in script globals. Docs enforce it; hot reload is the carrot that makes modders comply.
- **First-party-as-a-mod:** the entire action-adventure gameplay layer ships as `base-action/`, a normal mod loaded via the normal loader with zero private hooks. Why this is the product's backbone: (1) the API is provably complete — if the base game can't be built on it, neither can mods; (2) breaking changes hurt you before they hurt users; (3) the base pack *is* the best documentation — modders read shipping code, not toy samples; (4) it forces the perf work (if Luau is too slow for your own NPCs, fix the API's C++ side, don't cheat). The only C++ in "gameplay" is engine systems: rendering, physics, nav, replication, sensors, BT ticking.

---

## 4. Genre presets & NPC presets

**Preset packs** are just maintained first-party mods on the public API: `preset-common` (shared utilities), `base-action` (v1: third-person controller glue, lock-on combat, health/damage, basic loot), later `preset-rpg` (dialogue trees, quest log, inventory UI) and `preset-rts` (selection, formations, command queue). Each versions independently (semver), declares its API level, and lives in the same repo structure as any mod. Genre extensibility = other people fork or depend on these packs. Nothing genre-specific ever enters the C++ core — that's the architectural guarantee that RTS/RPG stay possible without being built now.

**NPC data format:** JSON files with single inheritance and deep-merge override — boring, diffable, every language parses it.

**Behavior model verdict: behavior trees** for v1. FSMs tangle past ~6 states and don't compose for reuse; utility AI is powerful but nearly impossible for a novice to author or debug ("why did it do that?" has no answer a 15-year-old can find). BTs are composable in data, debuggable as a tree visualization, and have the deepest tutorial ecosystem. C++ owns the tick loop and structural nodes (selector/sequence/decorator/parallel); leaves are either built-in C++ actions (`move_to`, `play_anim`, `attack_melee`) or Luau functions.

**Sensors/blackboard split:** C++ systems (vision cones, hearing events, nav distance, threat table) run batched and write results into a per-NPC blackboard; scripts and BT conditions *read* the blackboard. Scripts never raycast per-tick. This keeps 200 NPCs cheap while scripts stay expressive.

**Zero-C++ new enemy, end to end:**

```json
// mods/my-goblins/npcs/bomb_goblin.npc.json
{
  "id": "my-goblins:bomb_goblin",
  "extends": "base-action:goblin_melee",
  "stats": { "health": 40, "move_speed": 4.5 },
  "model": "mod://my-goblins/assets/bomb_goblin.glb",
  "behavior": {
    "extends_tree": true,
    "insert_before": "melee_branch",
    "node": { "sequence": [
      { "condition": "bb.target_distance > 6" },
      { "action": "script:throw_bomb" },
      { "wait": 3.0 }
    ]}
  }
}
```
```lua
-- mods/my-goblins/server.luau
npc.action("throw_bomb", function(self, bb)
  local p = game.spawn_projectile("my-goblins:bomb", self:position(), bb.target_position)
  p:on_impact(function(hit) game.explode(hit.position, { radius = 3, damage = 25 }) end)
  return "success"
end)
```

Inherits stats, senses, animations, and the base BT from the preset's goblin; overrides two stats; grafts one branch; one scripted leaf. That's the product demo.

---

## 5. Packaging

`mods/<id>/mod.json`:

```json
{
  "id": "my-goblins", "name": "More Goblins", "version": "1.2.0",
  "api": 3,
  "dependencies": { "base-action": ">=2.0 <3", "preset-common": "^1.4" },
  "entry": { "server": "server.luau", "client": "client.luau" },
  "client_only": false,
  "permissions": []
}
```

- **Load order:** topological sort by dependencies; ties alphabetical; user overrides via a simple ordered list in the profile. Circular deps = load error naming the cycle. (Factorio's model; don't invent priorities/phases.)
- **Distribution v1:** loose folders and `.zip` in `mods/`, mounted read-only through the VFS. The whole "store" is a docs page listing community mods.
- **Workshop later:** the only design constraint to honor *now* is "a mod is one self-contained directory, addressed by id, mounted from any path." Workshop items then mount from the Workshop download dir with zero format changes. `permissions` array exists for future gated capabilities (e.g. companion-app network access) — empty and unenforced-beyond-none in v1.

---

## 6. Product strategy — the brutal part

**What a solo engine cannot win:** the general-purpose engine market. Godot is MIT, free, has thousands of contributors and 96%-positive social proof on its [Steam listing](https://store.steampowered.com/app/404790/Godot_Engine/); Unity/Unreal own hiring pipelines and asset stores. A solo generalist engine competes on none of these axes and loses on all. "People will build their games on my engine" is, today, the least credible sentence in this project's brief.

**What actually made moddable platforms succeed:** in every case you cited — Minecraft/Forge, Factorio, Garry's Mod, Roblox — **a compelling game or social platform came first and modding multiplied it.** Nobody adopted Forge for Forge's architecture; they were already playing Minecraft. Factorio's mod API is beloved *because* Factorio is beloved and Wube dogfoods and documents relentlessly. RPG Maker is the one "engine" on the list, and it won by narrowing genre so brutally that non-programmers ship complete games. The uniform lesson: **distribution comes from the game; the platform monetizes the retention.**

**Is the wedge credible?** "Moddable-by-default co-op 3D action engine with second-screen companion" — as an *engine pitch*, no; as a *game pitch*, yes, and it's genuinely underserved: a co-op action game where your friend group can add an enemy, a weapon, or a quest with a JSON file and 20 lines of Luau, hash-verified into your listen-server session, is a real and rare thing. Second-screen companion is a lovely demo and a differentiating bullet point, but it will not drive adoption by itself and it carries ongoing server/infra cost — build the inventory-on-phone demo, defer everything else about it.

**Honest sequencing:** ship the co-op vertical slice as a game people want; ship `base-action` as a visibly-normal mod; let the first hundred players discover that modding it is trivial. If a community forms, the "engine as product" story writes itself from evidence. If it doesn't, you still shipped a game and learned C++ — the engine-first framing risks a multi-year project with neither.

**Product v1 minimally requires:** the playable slice; base pack as a mod; mod API reference + five copy-paste sample mods (new enemy, new weapon, UI widget, quest, companion-screen panel); published versioning/deprecation policy; a mod template repo; GitHub issues + one Discord; the MkDocs site. **Explicitly do NOT build until real users exist:** visual editor/GUI tooling, mod portal/marketplace, the Wasm tier, RPG/RTS presets, C++ plugin SDK, localization, console anything, in-game mod browser.

**Steam verdict:** publish the **game** on Steam; Workshop rides on the game's appid — that's where mod-distribution demand will actually be. Do **not** publish the engine as a Steam tool at v1: Godot's Steam page is a mirror for an already-huge project, and RPG Maker sells there on decades of consumer brand; a solo engine listing would sit at zero reviews signaling abandonment. Engine/SDK goes on GitHub (source, issues, releases) and itch.io (free download, discoverability among hobbyists). Revisit a Steam tool listing only if the engine independently develops demand.

**Top risks, named:** (1) sandbox overclaiming — never market "secure against all mods"; market the concrete containment guarantees in §1; (2) API churn burning early modders — the API-level policy and migration notes are the antidote; (3) scope: every hour on RTS presets before the action slice ships is an hour against the project's survival.

---

**Sources:** [Luau sandbox guide](https://luau.org/sandbox/) · [Luau adoption (Wikipedia)](https://en.wikipedia.org/wiki/Luau_(programming_language)) · [LuaJIT Apple Silicon issue #1280](https://github.com/LuaJIT/LuaJIT/issues/1280) · [Luanti LuaJIT maintenance discussion](https://github.com/luanti-org/luanti/issues/14611) · [LuaJIT status](https://luajit.org/status.html) · [QuickJS-ng releases](https://github.com/quickjs-ng/quickjs/releases) · [wasmtime releases](https://github.com/bytecodealliance/wasmtime/releases) · [Component Model docs](https://component-model.bytecodealliance.org/running-components/wasmtime.html) · [Lua sandbox escape techniques (HackTricks)](https://hacktricks.wiki/en/generic-methodologies-and-resources/lua/bypass-lua-sandboxes/index.html) · [lua-users SandBoxes](http://lua-users.org/wiki/SandBoxes) · [Godot Engine on Steam](https://store.steampowered.com/app/404790/Godot_Engine/) · [wasvy (Bevy Wasm modding)](https://crates.io/crates/wasvy)