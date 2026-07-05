find_package(spdlog CONFIG REQUIRED)

add_library(eng_core STATIC
  log.cpp
)

target_include_directories(eng_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(eng_core
  PUBLIC spdlog::spdlog
  PRIVATE eng_warnings
)
