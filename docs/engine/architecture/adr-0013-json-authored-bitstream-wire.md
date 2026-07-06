# ADR-0013: JSON for authored data, engine bitstream for wire and saves

**Status:** accepted (2026-07).

Hand-authored data (scenes, NPC presets, recipes, mod manifests) is JSON with comments, parsed via nlohmann, with schema validation that reports path/expected/actual in every error message. Wire traffic and saves use the engine bitstream: one `Serialize(Stream&, T&)` template per type serves both read and write, and a `JsonWriteStream` implements the same interface for debug dumps. Scene files carry a `version` field from file #1.

Why: one serialization definition per type means no second reflection system to keep in sync — the classic drift bug is structurally impossible. The bitstream also powers the determinism harness's state hashes (ADR-0018), and it is the M0 TDD graduation kata (round-trip property `read(write(x)) == x`, malformed-input fuzz, under ASan/UBSan). Schema errors with paths are a modder-facing product feature, not a nicety (M6 exit criterion).

Rejected: protobuf/flatbuffers (an IDL compiler and second schema language for types we fully own); binary authored data (modders need diffable, hand-editable text); YAML (parser ambiguity in a trust boundary).
