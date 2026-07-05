# ADR-0010: EnTT is the ECS

**Status:** accepted (2026-07).

EnTT provides the ECS, used behind `engine/ecs/` as the world/entity substrate. An entity is an ID — no entity class hierarchies, ever; capabilities are components (`class Enemy : public Character` never appears). EnTT itself is a vocabulary library and is used bare in sim code, not wrapped (wrapping vocabulary libs is K5 astronautics); the `engine/ecs/` module owns world lifetime, scheduling glue, and serialization hooks.

Why: sparse-set component pools are the natural replication substrate — iterating "what changed in this pool" is exactly what snapshot generation needs. The design consequences are load-bearing elsewhere: NPC identity tiers (Named/Crew/Ambient) are component sets, so tier promotion is just adding components, and JSON `extends` composition rides on the same model.

Rejected: flecs (excellent, but EnTT's header-only C++ idioms fit a learner's single-language codebase better); hand-rolled ECS (K5 — on the never-hand-roll list); OOP scene graph (forecloses replication and data-driven variation).
