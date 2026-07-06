# ADR-0022: Dear ImGui is the slice UI

**Status:** accepted (2026-07).

Dear ImGui renders all v1 UI: menus, HUD, and the colony-management UI (utilitarian, RimWorld-style — explicitly budgeted in M8a, with a custom theme/production pass and gamepad operability validated on Deck at M8b). Mods get a constrained Luau HUD API — text/bars/icons at anchors — not raw ImGui access. OFL fonts only. UI strings go through `tr("key")` string tables from the first UI text.

Why: a scriptable UI framework is a quarter-scale project in itself and is explicitly not being built in v1. ImGui is already in the stack as the M2 debug overlay, is proven for exactly this utilitarian management-UI genre, and the constrained HUD API keeps mods on a surface small enough to sandbox and document. The reference games' colony UIs are functional, not beautiful — matching that bar is a feature, not a compromise.

Rejected: RmlUi/NoesisGUI/custom retained-mode UI in v1 (the quarter-scale project); exposing raw ImGui to Luau (unsandboxable surface area).

Revisit trigger: the real-UI-stack go/no-go is pinned at the R1 exit — decided there, not before.
