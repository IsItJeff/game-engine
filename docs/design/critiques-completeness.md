# Adversarial Review — COMPLETENESS Lens

All five documents are strong on what they cover. This review is about what **nobody** covers. Findings ordered by severity.

---

## BLOCKERS

**1. No document picks the game's UI system — an unowned subsystem that three docs depend on.**
Targets: architecture (only Dear ImGui, explicitly "debug overlay"), roadmap (M8 requires "menus/HUD"), modding-product (§3 `client.luau` does "UI"; §6 lists "UI widget" as one of five flagship sample mods; §4 promises `preset-rpg` ships "inventory UI").
The modding doc's product demo *sells a moddable UI API that no doc designs*. Game UI is a notoriously deep subsystem (layout, text rendering/shaping, gamepad focus navigation, styling), and its Luau surface is part of the "APIs are forever" product contract. Discovering this at M8 means either shipping ImGui as the *product's* UI (visually damning for a trailer-driven strategy) or a multi-month unplanned detour.
**Fix:** Decide now. Recommendation: Dear ImGui for slice menus/HUD (accept the look, art-direct around it), and *cut "UI widget" from the v1 sample-mod list* — expose only a constrained HUD API (text, bars, icons at anchors) to Luau. Real UI stack (RmlUi or custom) becomes a named post-slice milestone.

**2. Engine license and third-party license compliance are absent — for a project whose product IS the engine.**
Targets: all five. Roadmap mentions "license chosen" as a two-word M10 bullet — *after* M6-M9 invite modders and Steam testers to consume the code, and after the modding doc's plan puts the SDK "on GitHub" (unlicensed = all-rights-reserved; nobody can legally fork your preset packs, which §4 says is the extensibility model). Nobody owns: a NOTICES file aggregating vcpkg-transitive licenses (GNS pulls OpenSSL + protobuf; Atkinson Hyperlegible has a font license; ozz, Recast, Luau, Jolt all require attribution), GPL/LGPL screening of transitive deps, or Steam's requirement that you have redistribution rights for everything in the depot. Also unanswered: what license makes "fork my genre presets" legal while keeping any commercial option open?
**Fix:** Decide at M0, not M10: engine + presets under MIT or Apache-2.0 (Apache-2.0 preferred — patent grant matters for a product); add a CI step that dumps vcpkg's license fields and fails on copyleft; cooker emits `THIRDPARTY-NOTICES.txt` into every package.

**3. Save/mod/protocol compatibility matrix exists nowhere; the pieces contradict.**
Targets: architecture §4 ("version header and per-type migration" — one sentence), modding-product §2 (mod `storage` "flushes atomically with the save file"), §3 (API level N/N−1), roadmap M8 ("save/load versioned from day one").
Unowned questions: loading a save whose mods are missing, downgraded, or API-level-bumped — refuse, strip, or quarantine? Does the save header record the mod list + versions + hashes? (The *join* handshake does; the save doesn't.) Architecture §6's connect handshake specifies mod hashes but **no engine/protocol version field** — the companion doc reserves `protocolVersion` for phones but the actual game protocol has none; post-Steam, auto-updated clients will hit older dedicated servers immediately. Also missing: crash-safe save writes (write-temp-rename + rolling backup) — "atomic flush" is asserted, not designed.
**Fix:** One page, owned by architecture: save header = {engine ver, API level, mod list w/ versions+hashes, sim tick}; policy = refuse-load with named culprit pre-1.0 (cheapest honest option); protocol version int in the first connect packet, exact-match required pre-1.0; save round-trip + corrupted-file test in CI next to the determinism replay.

**4. Input rebinding and gamepad support: unowned, and Steam Deck is never mentioned.**
Targets: architecture §5 ("data-driven bindings file" — one clause), roadmap (absent from all milestones), modding-product (mods presumably can bind actions — undefined).
A third-person action game on Steam in 2026 without gamepad support and a rebind screen fails review-section table stakes; Steam Deck is a large share of exactly this genre's Steam audience and imposes verification requirements (glyphs, no keyboard assumptions) that touch the UI (#1) and input systems. Also undefined: deadzones, can mods add actions to the bindings namespace, Steam Input vs SDL3 gamepad policy.
**Fix:** Gamepad path (SDL3 gamepad API) enters at M4 with the character controller — controller feel *is* K4; rebind UI is an M8 exit criterion; Steam Input adoption decision is an M9 line item; explicitly state Deck as target or non-target.

---

## MAJORS

**5. Game accessibility is entirely absent while docs accessibility got a full spec.**
Targets: roadmap M8, docs-site (the irony: the *website* is ADHD/dyslexia-friendly; the *game* has no subtitle, text-scale, or colorblind line anywhere). For an engine-as-product, accessibility primitives are engine features your users inherit — or inherit the absence of.
**Fix:** M8 exit criteria: subtitles for all voiced/barked audio, UI text scale setting, never color-alone information, camera shake/motion toggle. Engine ships these as defaults so every derived game gets them.

**6. Localization *readiness* is zero — and the modding doc actively defers the wrong thing.**
Targets: modding-product §6 (defers "localization" wholesale), architecture (no string policy). Deferring *translations* is right; deferring *readiness* (UTF-8 everywhere, string keys instead of literals, no sentence concatenation, fonts beyond Latin) is the classic irreversible mistake — and the mod API will freeze it: if `mod.json` and the Luau UI/HUD API pass display literals, every mod ever written bakes in English.
**Fix:** `tr("key")` string-table API from the first UI text; mods ship a `strings/en.json`; costs days now, unpayable later.

**7. Crash reporting has no design; symbol archiving is never mentioned.**
Targets: roadmap M9 ("crash reporting" — two words), architecture §7 (fatal-error policy ends at "log + fatal"). Missing: minidump tech (Crashpad/Breakpad/Sentry), where dumps upload to (a server! — which contradicts companion's "no infra before the relay" stance and creates the project's first real GDPR surface: opt-in consent, IP handling), per-release PDB/dSYM archiving (without it, all shipped-build crashes are forever unreadable), and — critical for a *modding* product — crash reports must carry the mod list, or every hostile-mod crash becomes an "engine is unstable" Steam review.
**Fix:** Crashpad + Sentry free tier (or a dumb HTTPS inbox on the M5 VPS); CI archives symbols on every tagged build starting M8; crash metadata = engine ver + API level + mod list; explicit opt-in dialog.

**8. Settings persistence and platform-correct paths: unowned.**
Targets: architecture, companion, modding-product. Nobody says where settings, saves, logs, mod `storage`, and the user's `mods/` directory live. Windows: game under Program Files is read-only. macOS: a notarized .app is translocated and unwritable; writes go to `~/Library/Application Support`. Modding §5 says mods load from `mods/` — relative to *what* in a Steam install? SDL3's `SDL_GetPrefPath` solves all of this and no document claims it.
**Fix:** One path-map table in architecture (pref path for all writes; `mods/` under pref path; portable-mode flag later); settings file gets the same version header as saves (#3).

**9. macOS distribution cost/pipeline is a bullet point hiding real work, with one latent contradiction.**
Targets: roadmap M9, modding-product §1. Missing: $99/yr Apple Developer Program (the project's first mandatory recurring cost), hardened-runtime entitlements, notarization automation in CI (App Store Connect API key), and signing the *dedicated server* and *cooker* binaries too. The contradiction: modding-product touts Luau's "optional codegen" and "zero macOS notarization pain" in the same table — Luau native codegen generates executable code at runtime, which under hardened runtime needs the JIT entitlement, exactly the pain LuaJIT was rejected for.
**Fix:** Budget $99/yr now; M9 adds a notarize-all-binaries CI job; decree "Luau codegen disabled on macOS" (interpreter-only) in the modding doc so the sandbox story stays entitlement-free.

**10. Binary asset storage strategy is unowned; GitHub LFS free tier won't hold a 3D slice.**
Targets: architecture §8 (`assets_src/` with glTF/PNG/EXR/WAV committed), roadmap M4+ (Mixamo clips, terrain, biome art). GitHub LFS free tier is [1 GB storage / 1 GB bandwidth per month, hard-blocked on overage at $0 budget](https://docs.github.com/billing/managing-billing-for-git-lfs/about-billing-for-git-large-file-storage); a vertical slice's source art exceeds that within months, and every CI clone of the 3-job matrix burns bandwidth. This intersects finding 12: CI runs on *every push* on three OSes.
**Fix:** Decide before first art commit: LFS with a $5/50GB data budget, or assets out of git entirely (B2/S3 bucket + hash manifest the cooker already produces). Add `.gitattributes` day one regardless.

**11. Slice asset licensing: no bookkeeping, and sample-mod assets must be *redistributable*.**
Targets: roadmap K2 ("Mixamo clips — zero hand animation"), modding-product §6 (five sample mods + template repo containing models like `bomb_goblin.glb`). Mixamo's terms permit use *in games* but are murkier on redistribution as loose editable assets — which is exactly what a public template repo does. Nobody owns a per-asset license manifest or credits screen (CC-BY assets require it).
**Fix:** Cooker manifest gains a `license` + `attribution` field per source asset, CI fails on empty; sample/template mods use CC0 sources only (Kenney, PolyHaven, Quaternius); Mixamo confined to the game depot.

**12. CI cost collides with the docs-site's "repo can go private later" plan.**
Targets: docs-site §5 (chose Cloudflare Pages *specifically* so the repo can go private), architecture §1 (3-OS matrix on every push + weekly MSVC ASan). Private repos get [2,000 free minutes/month with macOS billed at a 10× multiplier](https://docs.github.com/en/billing/reference/actions-runner-pricing) — a 20-minute macOS build per push exhausts the entire monthly quota in ~10 pushes. The docs doc paid a hosting-choice premium for an option that the CI design makes unaffordable, and nobody noticed the interaction.
**Fix:** Decree the repo stays public (it's an open engine product — privacy conflicts with the strategy anyway), and delete the private-repo contingency from the docs doc; if privacy is truly wanted later, budget runner spend explicitly then.

**13. Mod ecosystem governance: sandbox is designed, the human layer isn't.**
Targets: modding-product §2, §5 ("the whole store is a docs page listing community mods"). Unowned: who vets that docs-page listing for malware/IP theft (a curated-by-you list is an implicit endorsement — worse reputationally than an open one), no revocation mechanism for a discovered-malicious mod already in circulation (you have hashes in the handshake; there's no blocklist they check against), no security disclosure channel despite marketing hostile-mod containment as *the* differentiator.
**Fix:** SECURITY.md + disclosure email at M6; a signed blocklist of known-bad mod hashes shipped with engine updates (the handshake already computes hashes — checking them against a list is ~20 lines); an explicit "listings are unvetted" disclaimer on the docs page.

**14. Window focus, device loss, and audio device changes: unowned lifecycle edges.**
Targets: architecture §5/§7. Missing: alt-tab policy (SP pauses; listen-server host alt-tabbing must NOT pause the sim their friends play on — a design decision with netcode implications, not a detail), borderless-vs-exclusive fullscreen policy, audio default-device hot-swap (miniaudio supports device-change routing but not automatically in all cases — must be configured and tested; AirPods disconnecting is *the* macOS bug report), and GPU device-removed being "log + fatal" — for a product engine, driver-update-mid-session on Windows deserves at least an emergency save before dying.
**Fix:** One "app lifecycle" page in architecture: focus/pause matrix per topology, borderless default, miniaudio device-follow enabled + tested, fatal path attempts autosave.

---

## MINORS

**15. GDPR/privacy beyond the companion.** Companion §4 handles LAN correctly, but: dedicated-server logs contain IPs (personal data — a *server admin's* GDPR problem your docs should mention), crash uploads (#7) need consent, and Steam requires accurate privacy disclosure. **Fix:** one privacy page covering all three, written at M9.

**16. Pre-Steam distribution/patching.** M5 ("deploy to VPS") and M8 (friends playtest) precede Steam, yet no doc covers producing signed installable builds or migrating user data between slice versions. **Fix:** a `package` CI job at M5 producing zip + notarized dmg; that's the whole design.

**17. Sandbox abuse-test suite.** Modding §2 lists evasion tricks ("known evasion tricks to cover") but no doc turns them into tests. A security differentiator without regression tests degrades on the first Luau upgrade. **Fix:** each listed evasion becomes a Catch2 case running a hostile `.luau` fixture in CI at M6.

**18. Companion on dedicated servers ships tokens over plain HTTP on the open internet.** Companion §1 scopes plain-HTTP to LAN correctly, then §1 also says on dedicated servers "players' phones reach it over the internet" — the missing piece is any TLS/reverse-proxy guidance for that topology. **Fix:** docs state companion-over-internet requires the admin to front it with Caddy/TLS, or the feature is LAN-only until the relay exists.

---

## What survives scrutiny (agree, keep)

- **SDL3 + SDL_GPU as sole v1 renderer; no premature RHI** (architecture) — the strongest single call in the set.
- **M3 client/server split before physics/animation; SP = loopback listen server, one code path** (roadmap, architecture) — the reorder rationale is correct and the retrofit-cost framing is exactly right.
- **Luau over Wasm/LuaJIT for v1, with the honest in-process containment disclaimer** (modding-product) — including the demand that the disclaimer go in the docs verbatim.
- **First-party gameplay as a normal mod on the public API** (modding-product) — the single best product-integrity mechanism proposed anywhere in these docs.
- **"The game is the wedge, not the engine"** (modding-product §6) — the most honest paragraph in the whole set; the synthesizer should treat it as binding.
- **One command funnel for all sim mutations** (companion §5) — correctly identified as the expensive-to-retrofit item; it also quietly solves replay, console, and mod-input trust.
- **Fixed 60 Hz timestep, predict-own-character-only, interpolation fallback pre-authorized** (architecture, roadmap K3).
- **Determinism replay test in CI** (architecture §7) — cheap, guards the most fragile contract.
- **Docs: one site, two sections, fixed page template, hard word caps, link-don't-embed video** (docs-site) — enforceable rules, not aspirations.
- **Cook step from day one; glTF as the only interchange** (architecture).
- **STOP rule / demo-video ratchet / track-switching** (roadmap §5) — the motivation engineering is as load-bearing as any technical choice for a solo multi-year project.

Sources: [GitHub Actions runner pricing](https://docs.github.com/en/billing/reference/actions-runner-pricing) · [GitHub Actions 2026 pricing change](https://github.blog/changelog/2025-12-16-coming-soon-simpler-pricing-and-a-better-experience-for-github-actions/) · [Git LFS billing/quota](https://docs.github.com/billing/managing-billing-for-git-large-file-storage/about-billing-for-git-large-file-storage) · [LFS quota discussion](https://github.com/orgs/community/discussions/171335)