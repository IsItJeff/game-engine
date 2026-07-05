# ADR-0012: ozz-animation with a glTF-only pipeline

**Status:** accepted (2026-07).

Skeletal animation uses ozz-animation. The asset pipeline is Blender→glTF only; FBX is refused outright. Mixamo animation libraries are retargeted through ozz, and the retarget pipeline is prototyped at M4 with one rig before anything depends on it.

Why: skeletal animation is a named project-killer (K2) — hand-rolling skinning, blending, and retargeting is a multi-year detour. ozz is the maintained, engine-agnostic runtime; glTF-only keeps the importer surface to one battle-tested format (parsed via cgltf); and Mixamo retargeting is the content strategy's answer to "a solo dev cannot animate" — which is why the M4 prototype gate exists: prove the riskiest link in the content chain before M8's content push leans on it.

Rejected: hand-rolled animation runtime (K2/K5); FBX pipeline (proprietary SDK, per-exporter quirks — an entire category of bugs deleted by refusing the format); assimp (huge dependency, uneven output).
