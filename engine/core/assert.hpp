#pragma once

#include <cstdlib>

#include "engine/core/log.hpp"

// Programmer-error check: logs and aborts in dev builds, compiled out in ship
// builds (adr-0017). Never use for conditions that can occur in normal play.
#ifndef NDEBUG
#define ENG_ASSERT(cond, msg)                                                          \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      eng::log::error("ASSERT FAILED: {} ({}:{}) {}", #cond, __FILE__, __LINE__, msg); \
      std::abort();                                                                    \
    }                                                                                  \
  } while (false)
#else
#define ENG_ASSERT(cond, msg) ((void)0)
#endif
