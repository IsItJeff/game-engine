find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(eng_tests
  core/test_version.cpp
  core/test_bitstream.cpp
  sim/test_simulation.cpp
)

# eng_net transitively brings eng_sim and eng_core, so linking it alone avoids
# duplicate-library warnings.
target_link_libraries(eng_tests
  PRIVATE eng_net Catch2::Catch2WithMain eng_warnings
)

catch_discover_tests(eng_tests)
