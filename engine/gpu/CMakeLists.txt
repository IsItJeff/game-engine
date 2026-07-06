find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# The graphics module. SDL3 and Dear ImGui are linked PUBLIC because the client
# that uses this renderer also calls ImGui:: and SDL keyboard functions directly.
# They are treated as SYSTEM headers by vcpkg, so their warnings don't count
# against our -Werror build.
add_library(eng_gpu STATIC renderer.cpp)
target_link_libraries(eng_gpu
  PUBLIC eng_core SDL3::SDL3 imgui::imgui
  PRIVATE eng_warnings
)
