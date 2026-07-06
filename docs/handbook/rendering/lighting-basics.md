# Lighting Basics

## What it is

**Blinn-Phong** is the cheapest lighting model that still makes a scene read as 3D. Each fragment's color is the sum of three terms — **ambient** (a floor so shaded faces aren't pure black), **diffuse** (faces toward the light are brighter), and **specular** (a tight highlight where light bounces toward the camera) — computed in the fragment shader from the mesh's **vertex normals** and one light direction. For the colony that light is a single directional **sun**: its rays are parallel everywhere on the map, so the light direction is one uniform, not a per-fragment subtraction.

## Why you care

Unlit, every face of a wall cube renders the same flat texel color and the map turns to mush; diffuse shading alone is what makes colonist meshes look solid. It is also the exact top of the v1 renderer: the K1 budget in the [master plan](../../design/master-plan.md) runs triangle → textured → camera → **blinn-phong** → one shadow cascade → skinning → tonemap, and stops. Like [Textures](textures.md), lighting is purely frame-side — the fixed 60 Hz tick never touches a normal, so nothing here can affect the server-authoritative sim.

## Quick start

Two inputs are new. First, every vertex needs a **normal** — a unit vector pointing away from the surface, baked into the vertex buffer next to position and UV ([meshes-on-the-gpu](meshes-on-the-gpu.md)). Second, the sun, pushed as a uniform each frame with SDL_GPU:

```cpp
// fragment — does not compile alone
struct SunUniform {                        // cbuffer rules: float3 pads to 16 bytes
    float dir_ws[3];        float pad0;    // direction sunlight travels, unit length
    float color[3];         float pad1;
    float camera_pos_ws[3]; float pad2;    // world space, from the camera each frame
};
static_assert(sizeof(SunUniform) == 48, "must mirror the HLSL cbuffer exactly");
SunUniform sun{ { 0.3f, -0.8f, 0.52f }, 0.f,
                { 1.f, 0.96f, 0.9f },   0.f,
                { cam.pos.x, cam.pos.y, cam.pos.z }, 0.f };
SDL_PushGPUFragmentUniformData(cmd, 0, &sun, sizeof(sun));
```

The fragment shader — bindings explained in [hlsl-shader-basics](hlsl-shader-basics.md); HLSL, offline-compiled via SDL_shadercross to DXIL/SPIR-V/MSL, never at runtime ([ADR-0009](../../engine/architecture/adr-0009-sdl-gpu-renderer.md)) — sums the three terms:

```hlsl
// HLSL fragment
cbuffer Sun : register(b0, space3)
{
    float3 sun_dir_ws;    // unit length, normalized on the CPU
    float3 sun_color;
    float3 camera_pos_ws;
};

float4 main(float3 normal_ws : TEXCOORD0,
            float3 pos_ws    : TEXCOORD1) : SV_Target
{
    float3 n = normalize(normal_ws);           // interpolation shrank it
    float3 l = -sun_dir_ws;                    // fragment -> sun
    float3 v = normalize(camera_pos_ws - pos_ws);
    float3 h = normalize(l + v);               // Blinn's halfway vector

    float ambient  = 0.1;
    float diffuse  = max(dot(n, l), 0.0);
    float specular = pow(max(dot(n, h), 0.0), 64.0);

    float3 albedo = float3(0.55, 0.55, 0.6);   // a texture sample later
    return float4((ambient + diffuse + specular) * sun_color * albedo, 1.0);
}
```

As everywhere in this track, examples use column-vector math like LearnOpenGL; the vertex shader feeding `normal_ws` rotates the normal with `mul(model_rotation, normal)`, and HLSL `mul()` argument order is where that convention bites.

!!! tip
    Colony assets use uniform scale only, so LearnOpenGL's "normal matrix" (`transpose(inverse(model))`) collapses to the model matrix's rotation part. Skip the inverse until someone ships a non-uniformly scaled mesh.

## How it works

```mermaid
flowchart LR
    A[Vertex buffer<br>position + normal] -->|vertex shader<br>rotates to world space| B[World-space<br>normal]
    B -->|rasterizer<br>interpolates| C[Per-fragment normal<br>not unit length]
    C --> D[normalize, then<br>ambient + diffuse + specular]
    D --> E[x albedo<br>into the frame]
```

Each term is one cheap vector operation:

- **Ambient** — a constant like `0.1`, a stand-in for indirect bounce light so the dark side of a wall cube stays visible.
- **Diffuse** — `max(dot(n, l), 0)`. Faces square-on to the sun get full light, grazing faces fade, and faces pointing away clamp to zero instead of going negative and stealing light.
- **Specular** — `pow(max(dot(n, h), 0), shininess)`. The exponent tightens the highlight; 64 is a sane start for the colony's stylized look.

**Why Blinn beats plain Phong**: Phong compares the view direction against the **reflected** light ray, and once that angle passes 90° — a low evening sun grazing the floor tiles — the dot product goes negative, clamps to zero, and the highlight cuts off with a hard visible edge. Blinn instead compares the normal against the **halfway vector** `normalize(l + v)`, whose angle to the normal can't exceed 90°, so the highlight fades smoothly. Provable on paper:

```cpp
#include <cassert>
#include <cmath>

struct Vec3 { float x, y, z; };
float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 normalize(Vec3 a) {
    float len = std::sqrt(dot(a, a));
    return {a.x / len, a.y / len, a.z / len};
}

int main() {
    Vec3 n{0.f, 1.f, 0.f};                    // flat colony floor tile
    Vec3 l = normalize({0.99f, 0.15f, 0.f});  // toward a low evening sun
    Vec3 v = normalize({0.95f, 0.32f, 0.f});  // grazing camera, same side

    float nl = dot(n, l);                     // Phong: reflect l about n
    Vec3 r{2.f*nl*n.x - l.x, 2.f*nl*n.y - l.y, 2.f*nl*n.z - l.z};
    assert(dot(v, r) < 0.f);                  // clamps to 0: highlight cut off

    Vec3 h = normalize({l.x + v.x, l.y + v.y, l.z + v.z});
    assert(dot(n, h) > 0.f);                  // Blinn: highlight survives
}
```

The one cost: for a matching highlight size, Blinn needs a shininess exponent roughly 2–4× Phong's.

## Pros / Cons

| | Pro | Con |
|---|---|---|
| Blinn-Phong | a few dot products per fragment; no cutoff artifact | needs 2–4× Phong's exponent |
| Plain Phong | textbook-familiar `reflect()` | hard specular edge at grazing angles |
| Per-fragment shading | smooth across big wall quads | more fragment work |
| Per-vertex (Gouraud) | cheaper | highlights wobble on low-poly meshes |

## What to expect

First-run symptoms map to causes: an all-black mesh means normals are missing, zero, or the sun direction was never negated; lighting that follows the camera means normals stayed in view space while `camera_pos_ws` is world space — pick one space ([Cameras](cameras.md)); blotchy specular on colonist faces means the interpolated normal wasn't re-normalized.

!!! warning
    Every vector in the dot products must be unit length. Normalize `sun_dir_ws` once on the CPU and re-normalize the interpolated normal per fragment — a 0.9-length normal silently dims diffuse by 10% and skews specular, with no error anywhere.

!!! info
    v1 stops at Blinn-Phong plus one shadow cascade (shadow mapping gets its own page when the cascade lands). Point lights, normal maps, and PBR are not in v1 — the pre-authorized fallback is flat-shaded art, never a fancier lighting model ([master plan](../../design/master-plan.md)).

## Go deeper

- [meshes-on-the-gpu](meshes-on-the-gpu.md) — adding the normal attribute to the vertex layout
- [Textures](textures.md) — the albedo that replaces the hard-coded `float3`, and why lighting math needs linear color
- [hlsl-shader-basics](hlsl-shader-basics.md) — cbuffer packing and the register spaces behind the sun uniform
- [Cameras](cameras.md) — where `camera_pos_ws` comes from
- [depth-buffer](depth-buffer.md) — why the lit fragment that survives is the nearest one
- [value-semantics](../cpp/value-semantics.md) — why the little `Vec3` above is passed by value
- [ADR-0009](../../engine/architecture/adr-0009-sdl-gpu-renderer.md) — the offline shader pipeline this page's HLSL rides

**Sources**

- Basic Lighting — LearnOpenGL, https://learnopengl.com/Lighting/Basic-Lighting — accessed 2026-07-06
- Advanced Lighting (Blinn-Phong) — LearnOpenGL, https://learnopengl.com/Advanced-Lighting/Advanced-Lighting — accessed 2026-07-06
- Video: Healthbars, SDFs & Lighting • Shaders for Game Devs [Part 2] — Freya Holmér, https://www.youtube.com/watch?v=mL8U8tIiRRg (210 min) — watch the lighting chapter from 2:08:29 to ~3:25 (Lambert → Phong → Blinn-Phong → compositing), roughly the final 80 min, once your first Blinn-Phong shader is on screen
