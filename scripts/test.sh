#!/usr/bin/env bash
#
# Build the dev preset (Debug + ASan/UBSan) and run tests.
#
#   ./scripts/test.sh                 # build, then run ALL tests (summary)
#   ./scripts/test.sh "[bitstream]"   # build, then run tests matching a Catch2
#                                     #   tag/name filter, showing actual vs
#                                     #   expected values — the TDD inner loop
#
# Any arguments are passed straight through to the test binary as Catch2
# filters, so "[bitstream]", a test name, or -? (Catch2 help) all work.
set -euo pipefail

# Homebrew's ninja/cmake aren't on a fresh shell's PATH; add them if present.
[ -d /opt/homebrew/bin ] && export PATH="/opt/homebrew/bin:$PATH"
# Point at your vcpkg checkout; override by exporting VCPKG_ROOT yourself.
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"

# Configure once (cheap no-op if already configured), then build.
cmake --preset dev >/dev/null
cmake --build --preset dev

if [ "$#" -gt 0 ]; then
  # Filtered: run the binary directly for per-assertion detail.
  ./build/dev/tests/eng_tests "$@"
else
  # Unfiltered: CTest summary of the whole suite.
  ctest --preset dev
fi
