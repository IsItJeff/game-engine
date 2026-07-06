# Cameras

## What it is

A camera answers one question: how does a vertex in a colonist mesh's **local space** become a pixel on screen? The answer is a chain of matrices — **model** (local → world), **view** (world → the camera's eye space), **projection** (eye space → clip space) — combined into one **MVP** matrix that the vertex shader applies to every vertex. SDL_GPU has no camera object at all. In this engine a camera is plain data in the ECS: a component holding an eye position, a target, and a field of view, from which the render system derives the view and projection matrices each frame (eye/target feed the view, fov feeds the projection). Examples on this page use column-vector math like LearnOpenGL — transforms read right to left — and HLSL `mul()` order is where that convention bites ([hlsl-shader-basics](hlsl-shader-basics.md)).

## Why you care

The colony sim needs two cameras from day one: a third-person camera behind your character and a zoomable tactical camera for base layout ([master plan](../../design/master-plan.md)). If the camera is an object that "draws the scene", supporting both means surgery; if it is a component, switching cameras is reading a different entity. And because a camera only affects what **you** see, it is purely client-local state: the server never replicates it, and the fixed 60 Hz tick never reads it — like [textures](textures.md), it lives entirely on the **frame** side.

## Quick start

```cpp
// fragment — does not compile alone
// The camera is a component that yields a view matrix — not a thing that draws.
struct Camera {
    Vec3  eye;                    // set by the follow / tactical systems
    Vec3  target;                 // the followed colonist, or the map focus
    float fov_y  = 1.0f;          // radians; ignored by the orthographic path
    float near_z = 0.1f, far_z = 200.0f;
};

// Render system, once per frame:
auto& cam  = registry.get<Camera>(active_camera);
Mat4  view = look_at(cam.eye, cam.target, Vec3{0, 1, 0});
Mat4  proj = perspective(cam.fov_y, aspect, cam.near_z, cam.far_z);

for (auto [entity, transform, mesh] : registry.view<Transform, MeshRef>().each()) {
    Mat4 mvp = proj * view * transform.to_matrix();   // right to left
    SDL_PushGPUVertexUniformData(cmd, 0, &mvp, sizeof(mvp));
    // ... bind the mesh from meshes-on-the-gpu, then draw
}
```

The vertex shader — HLSL, offline-compiled via SDL_shadercross to DXIL/SPIR-V/MSL — receives the MVP through a uniform buffer:

```hlsl
// HLSL fragment
cbuffer Camera : register(b0, space1)   // slot rules: hlsl-shader-basics
{
    float4x4 mvp;
};

float4 main(float3 pos_local : POSITION) : SV_Position
{
    return mul(mvp, float4(pos_local, 1.0));
}
```

## How it works

```mermaid
flowchart LR
    A[Local space<br>colonist mesh] -->|model matrix| B[World space<br>map coordinates]
    B -->|view matrix| C[View space<br>camera at origin]
    C -->|projection matrix| D[Clip space<br>homogeneous coords]
    D -->|divide by w → NDC, viewport| E[Screen space<br>pixels]
```

**Model** comes from each entity's `Transform` component: every wall cube shares one mesh, placed by its own model matrix. **View** hides the key trick: you can never move a camera, only the world. The view matrix is the **inverse** of the camera's own transform — camera flies 12 units up, the whole map moves 12 units down. **Projection** defines the visible frustum and maps it into clip space — still homogeneous; only the hardware's divide-by-`w` squeezes it into the NDC box (depth runs 0→1 on SDL_GPU's backends). A perspective projection scales `w` with distance, so that divide is what makes far colonists small. What the near/far planes do to depth precision is the [depth-buffer](depth-buffer.md) page's problem.

!!! tip
    You never need a general 4×4 inverse. `look_at(eye, target, up)` builds the inverted matrix directly from three perpendicular axes plus the negated position — the Gram-Schmidt construction in the LearnOpenGL Camera chapter.

The inverse relationship is small enough to prove:

```cpp
#include <array>
#include <cassert>

// Column-major 4x4, column-vector math (like LearnOpenGL): out = m * v.
using Mat4 = std::array<float, 16>;   // m[col * 4 + row]
using Vec4 = std::array<float, 4>;

Vec4 mul(const Mat4& m, const Vec4& v) {
    Vec4 out{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            out[row] += m[col * 4 + row] * v[col];
    return out;
}

Mat4 translate(float x, float y, float z) {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = x; m[13] = y; m[14] = z;
    return m;
}

int main() {
    Mat4 model = translate(10.0f, 0.0f, 4.0f);    // colonist stands at (10,0,4)
    Mat4 view  = translate(-10.0f, 0.0f, -12.0f); // camera at (10,0,12): negate!
    Vec4 head{0.0f, 1.8f, 0.0f, 1.0f};            // top of the head, local space
    Vec4 world = mul(model, head);
    Vec4 eye   = mul(view, world);
    assert(world[0] == 10.0f && world[2] == 4.0f);
    assert(eye[0] == 0.0f && eye[2] == -8.0f);    // 8 units in front of the lens
}
```

!!! warning
    With column vectors the product is `proj * view * model` — reversed, it usually renders nothing, because every vertex lands outside clip space. D3D tutorials often use row vectors and write the same chain backwards; port the math, not the order.

## Pros / Cons

| Projection | Pro | Con |
|---|---|---|
| Perspective (third-person) | Real depth cues; distance readable at a glance | Sizes on screen vary — precise tile placement is harder |
| Orthographic (tactical option) | Blueprint-exact: one tile is one size everywhere | No foreshortening; scenes can read as flat |

The tactical camera can also stay perspective with a steep pitch and narrow FOV — decide by feel in M7, not now.

## What to expect

First-frame failures map to causes cleanly: a black screen is usually the camera inside or behind the mesh, a flipped multiply order, or `w = 0` instead of 1 on positions; a stretched picture means the aspect ratio didn't follow a window resize; a colonist that vibrates while followed means the camera reads raw tick positions — the fix is [render-interpolation](render-interpolation.md), not this page.

!!! info
    Frustum culling, camera shake, and split-screen are not in v1 — the K1 renderer budget stops at tonemap ([master plan](../../design/master-plan.md)).

## Go deeper

- [meshes-on-the-gpu](meshes-on-the-gpu.md) — the vertex data the MVP transforms
- [hlsl-shader-basics](hlsl-shader-basics.md) — `mul()` order, cbuffer registers and spaces
- [depth-buffer](depth-buffer.md) — what near/far choices cost in depth precision
- [render-interpolation](render-interpolation.md) — smooth camera motion between ticks
- [lighting-basics](lighting-basics.md) — view space returns for specular highlights
- [Value semantics](../cpp/value-semantics.md) — why `Camera` is a copyable aggregate, not a class hierarchy

**Sources**

- Coordinate Systems — LearnOpenGL, https://learnopengl.com/Getting-started/Coordinate-Systems — accessed 2026-07-06
- Camera — LearnOpenGL, https://learnopengl.com/Getting-started/Camera — accessed 2026-07-06
- Video: Linear transformations and matrices | Chapter 3, Essence of linear algebra — 3Blue1Brown, https://www.youtube.com/watch?v=kYB8IZa5AuE — 11 min; watch all of it before this page if matrices still feel like magic
