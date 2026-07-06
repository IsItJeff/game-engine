# Vertex Skinning

## What it is

Vertex skinning is the last stage of the animation pipeline: deforming the mesh so it follows the posed skeleton. The industry-default technique is **linear blend skinning (LBS)**: each vertex stores up to four joint indices plus four weights that sum to 1, the CPU delivers one **skinning matrix per joint** (the palette), and the deformed position is the weighted sum of the vertex transformed by each of its joints' matrices. [Skeletal animation](./skeletal-animation.md) introduced the skeleton; [Bind pose](./bind-pose.md) explained the matrices in the palette. This page is only the per-vertex math.

## Why you care

Skeletal animation is roadmap project-killer K2, and skinning is its most GPU-shaped piece: a tiny weighted sum executed for every vertex of every visible colonist, every frame. The engine will run it in the vertex shader on SDL GPU ([ADR-0009](../../engine/architecture/adr-0009-sdl-gpu-renderer.md)), with palettes produced on the CPU by ozz-animation ([ADR-0012](../../engine/architecture/adr-0012-ozz-animation.md)) — so the shape of this math decides the vertex layout ([Meshes on the GPU](../rendering/meshes-on-the-gpu.md)) and one buffer upload per skinned mesh per frame.

## Quick start

LBS on one vertex, whole algorithm, compiles as pasted:

```cpp
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

struct Vec3 { float x, y, z; };

// Row-major 3x4 affine matrix: rotation in columns 0-2, translation in column 3.
struct Mat34 {
    float m[3][4];
    Vec3 transformPoint(Vec3 p) const {
        return { m[0][0]*p.x + m[0][1]*p.y + m[0][2]*p.z + m[0][3],
                 m[1][0]*p.x + m[1][1]*p.y + m[1][2]*p.z + m[1][3],
                 m[2][0]*p.x + m[2][1]*p.y + m[2][2]*p.z + m[2][3] };
    }
};

// One skinned vertex, exactly as glTF stores it.
struct SkinnedVertex {
    Vec3 position;                         // bind-pose position, model space
    std::array<unsigned char, 4> joints;   // JOINTS_0: palette indices
    std::array<float, 4> weights;          // WEIGHTS_0: must sum to 1
};

Vec3 skin(const SkinnedVertex& v, const Mat34* palette) {
    Vec3 out{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {          // weight 0 => joint contributes nothing
        Vec3 p = palette[v.joints[i]].transformPoint(v.position);
        out.x += v.weights[i] * p.x;
        out.y += v.weights[i] * p.y;
        out.z += v.weights[i] * p.z;
    }
    return out;
}

int main() {
    Mat34 identity{{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}}};
    Mat34 rotZ90  {{{0,-1,0,0}, {1,0,0,0}, {0,0,1,0}}};  // 90 degrees about Z
    Mat34 palette[2] = {identity, rotZ90};

    // An "elbow" vertex owned 50/50 by an unmoving joint and a bent one.
    SkinnedVertex v{{1,0,0}, {{0,1,0,0}}, {{0.5f,0.5f,0.0f,0.0f}}};
    Vec3 s = skin(v, palette);
    assert(std::fabs(s.x - 0.5f) < 1e-6f && std::fabs(s.y - 0.5f) < 1e-6f);

    // Both joints keep the vertex on the unit circle; the linear blend cuts
    // the corner and lands inside it. That shrinkage IS the LBS volume loss.
    float len = std::sqrt(s.x*s.x + s.y*s.y + s.z*s.z);
    std::printf("skinned = (%.2f, %.2f, %.2f), length %.3f\n", s.x, s.y, s.z, len);
    assert(len < 1.0f);
}
```

## How it works

Where this sits in the recurring pipeline — everything before it runs on the CPU each tick; skinning runs on the GPU each rendered frame:

```mermaid
flowchart LR
    A["clip sample"] --> B["blend"]
    B --> C["local-to-model"]
    C --> D["skin on GPU"]
    style D fill:#7c4dff,color:#fff,stroke:#4527a0,stroke-width:3px
```

Per glTF 2.0, each skinned vertex carries two extra attributes: **JOINTS_0** (four unsigned byte/short palette indices) and **WEIGHTS_0** (four normalized weights). Four influences is the format's baseline and the practical sweet spot — a fixed-size, GPU-friendly record. Unused slots hold weight 0, and the spec recommends index 0 there too.

The vertex shader the renderer will use is the same loop, unrolled — blending the four matrices first, then transforming once, which is algebraically identical and cheaper:

```hlsl
// fragment — does not compile alone
StructuredBuffer<float4x4> JointPalette : register(t0, space0);

float4 SkinPosition(float3 pos, uint4 joints, float4 weights)
{
    float4x4 skinMat = weights.x * JointPalette[joints.x]
                     + weights.y * JointPalette[joints.y]
                     + weights.z * JointPalette[joints.z]
                     + weights.w * JointPalette[joints.w];
    return mul(skinMat, float4(pos, 1.0));
}
```

Normals get the same treatment with the matrix's rotation part. Fragment syntax and bindings are [HLSL shader basics](../rendering/hlsl-shader-basics.md)' territory.

!!! warning
    The classic bug: **unnormalized weights**. If the four weights do not sum to 1, the vertex is silently scaled — the mesh breathes, swells, or collapses as joints move. glTF exporters are supposed to normalize; defend anyway by renormalizing in the importer, because a mostly-right mesh that wobbles is far harder to diagnose than an assert.

Linearly averaging rigid transforms is not rigid, and that produces LBS's two famous artifacts: **volume loss** (elbows and knees pinch — the demo's shrinking length) and **candy-wrapper twist** (twist a wrist 180 degrees and the forearm collapses to a point, because opposite rotations average toward zero). Fixes exist — dual-quaternion skinning, corrective joints, blend shapes — but LBS remains the default everywhere because it is cheap, simple, and artists rig around its flaws. ozz ships exactly this model ([ADR-0012](../../engine/architecture/adr-0012-ozz-animation.md)), same "wrap the maintained library" play as [Jolt](../physics/jolt-overview.md).

## Pros / Cons

| Pros | Cons |
|---|---|
| Constant per-vertex cost: 4 matrix transforms, ideal GPU work | Volume loss at bent joints |
| Fixed-size vertex record — trivial buffer layout | Candy-wrapper twist under large rotations |
| Universally supported: glTF, ozz, every DCC tool | Artifacts need rig-side workarounds (more joints, correctives) |
| Palette is the only per-frame upload per mesh | Weights must be normalized or everything subtly scales |

## What to expect

- Where the palette's matrices come from each tick — sampling clips with lerp/slerp: [Animation clips](./animation-clips.md).
- Palettes are per-tick sim output; frames render between ticks, so skinned characters need the same treatment as [Render interpolation](../rendering/render-interpolation.md).
- ozz's own SIMD skinning job and its role in the planned runtime: [ozz overview](./ozz-overview.md).

## Go deeper

- [Bind pose](./bind-pose.md) — how the palette matrices are built (inverse bind matrices).
- [Skeletal animation](./skeletal-animation.md) — the K2 framing and the joint hierarchy.
- [Meshes on the GPU](../rendering/meshes-on-the-gpu.md) — vertex buffer layout and upload for JOINTS_0/WEIGHTS_0.
- [Render pipeline](../rendering/render-pipeline.md) — where the skinning vertex shader will slot in (ADR-0009).
- [ADR-0012](../../engine/architecture/adr-0012-ozz-animation.md) — ozz-animation decision record.

**Sources**

- glTF 2.0 Specification — Skinned Mesh Attributes — https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#skinned-mesh-attributes — accessed 2026-07-06
- Skinning: Real-time Shape Deformation (SIGGRAPH 2014 course) — https://skinning.org/ — accessed 2026-07-06
- ozz-animation — Skinning sample — https://guillaumeblanc.github.io/ozz-animation/samples/skinning/ — accessed 2026-07-06
