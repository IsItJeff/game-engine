find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(eng_tests
  core/test_version.cpp
  core/test_bitstream.cpp
)

target_link_libraries(eng_tests
  PRIVATE eng_core Catch2::Catch2WithMain eng_warnings
)

catch_discover_tests(eng_tests)
