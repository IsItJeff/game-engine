#pragma once

#include <spdlog/spdlog.h>

namespace eng::log {

// Call once at startup, before any logging.
void Init();

using spdlog::debug;
using spdlog::error;
using spdlog::info;
using spdlog::warn;

}  // namespace eng::log
