#include "engine/core/log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace eng::log {

void Init() {
  auto logger = spdlog::stdout_color_mt("eng");
  spdlog::set_default_logger(std::move(logger));
  spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
#ifndef NDEBUG
  spdlog::set_level(spdlog::level::debug);
#endif
}

}  // namespace eng::log
