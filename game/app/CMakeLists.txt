find_package(SDL3 CONFIG REQUIRED)

add_executable(game
  main.cpp
)

target_link_libraries(game
  PRIVATE eng_core SDL3::SDL3 eng_warnings
)
