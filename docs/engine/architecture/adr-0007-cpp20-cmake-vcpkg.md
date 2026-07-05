# ADR-0007: C++20, CMake presets, vcpkg manifest mode

**Status:** accepted (2026-07).

The toolchain is C++20, CMake with presets (`dev` Debug+ASan/UBSan, `debug`, `release`, `ci` = dev + warnings-as-errors, `ci-msvc` for Windows/Ninja/MSVC), and vcpkg in manifest mode with a pinned baseline (`dfcb04008a5a9e038a4ae0d1af57909e30d21359`). Dependency updates happen on a quarterly bump branch that must pass CI, a 15-minute manual smoke on both OSes, and a release-notes read before merging.

Why: every dependency in the stack (SDL3, EnTT, GLM, spdlog, Catch2, tl::expected, and the milestone-gated Jolt/GNS/ozz/Luau) has a maintained vcpkg port, and manifest mode plus a pinned baseline makes builds reproducible across the three CI platforms and a fresh machine. Warnings-as-errors is CI-only so local iteration stays unblocked; MSVC runs `/permissive- /W4` from M0.

Rejected: Conan (second ecosystem for zero deps vcpkg lacks); git submodules/FetchContent (hand-maintained version graph, slow cold builds); C++23 (compiler support still uneven across MSVC/AppleClang/Clang in ways a solo dev shouldn't debug).
