# ADR-0009: SDL3 GPU API is the sole v1 renderer

**Status:** accepted (2026-07).

The SDL3 GPU API is the only v1 renderer. Shaders are authored in HLSL and offline-compiled via SDL_shadercross to DXIL, SPIR-V, and MSL — one shader language, three backends, zero runtime compilation. There is no RHI abstraction and none will be written until a second backend actually exists (code-design rule: no interface without two real implementations).

Why: native Metal on macOS (no MoltenVK layer to debug), roughly a fifth of Vulkan's concepts for a first-time graphics programmer, and Valve-backed maintenance. Honest caveat, kept verbatim: SDL_shadercross is technically still "preview" (3.0.0-preview2, Apr 2026) but widely used; the risk is confined to a build-time tool, not the runtime. The hard renderer budget (K1) — triangle → textured → camera → blinn-phong → one shadow cascade → skinning → tonemap — IS the whole v1 renderer.

Rejected: Vulkan + MoltenVK (concept count and translation-layer debugging for one person); bgfx/wgpu (another team's abstraction between us and the driver); OpenGL (deprecated on macOS).

Fallback (pre-authorized): cut renderer features — flat-shaded art — never swap APIs mid-project.
