# ADR-0021: All writes go under SDL_GetPrefPath

**Status:** accepted (2026-07).

Every write the engine or game performs — saves, settings, logs, mod persistent storage, and the `mods/` directory itself — goes under `SDL_GetPrefPath`. Nothing is ever written beside the executable.

Why: the install directory is not writable in the real world — Windows Program Files ACLs, macOS notarized app bundles and Gatekeeper translocation, and Steam's ownership of its own depot all break write-beside-exe silently or destructively (Steam can verify/overwrite depot files, eating saves). Pref-path also gives per-user separation and puts saves where OS backup tooling expects them. Mods remain self-contained, id-addressed directories mountable from any path, so keeping the default `mods/` under pref-path costs nothing.

Rejected: portable-app layout beside the executable (see every failure above); custom per-OS path logic (SDL3 already encodes the platform conventions — ADR-0008).
