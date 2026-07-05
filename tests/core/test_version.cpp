#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "engine/core/version.hpp"

TEST_CASE("version constants are consistent", "[core]") {
  const std::string_view v{eng::kVersionString};
  REQUIRE_FALSE(v.empty());

  const auto expected = std::to_string(eng::kVersionMajor) + "." +
                        std::to_string(eng::kVersionMinor) + "." +
                        std::to_string(eng::kVersionPatch);
  REQUIRE(v == expected);
}
