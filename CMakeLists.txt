cmake_minimum_required(VERSION 3.28)

project(game-engine VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENG_WARNINGS_AS_ERRORS "Treat warnings as errors (CI sets this ON)" OFF)
option(ENG_SANITIZERS "Enable ASan+UBSan (non-MSVC)" OFF)
option(ENG_BUILD_TESTS "Build tests" ON)

# Warnings apply to our targets only, never to vcpkg dependencies.
add_library(eng_warnings INTERFACE)
if(MSVC)
  target_compile_options(eng_warnings INTERFACE /W4 /permissive-)
  if(ENG_WARNINGS_AS_ERRORS)
    target_compile_options(eng_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(eng_warnings INTERFACE -Wall -Wextra -Wconversion -Wshadow)
  if(ENG_WARNINGS_AS_ERRORS)
    target_compile_options(eng_warnings INTERFACE -Werror)
  endif()
endif()

if(ENG_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(engine/core)
add_subdirectory(engine/sim)
add_subdirectory(engine/net)
add_subdirectory(engine/gpu)
add_subdirectory(game/app)

if(ENG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
