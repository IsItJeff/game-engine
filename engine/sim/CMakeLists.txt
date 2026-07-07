find_package(EnTT CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# The simulation library: world, components, systems, the fixed-timestep loop.
# Deliberately has NO dependency on windowing, GPU, or audio — that's what lets
# it run headless (dedicated server, tests) and be the shared core all three
# network modes reuse.
add_library(eng_sim STATIC
  world.cpp
  systems.cpp
  progression/curve.cpp
)

# EnTT and glm are "vocabulary" libraries used directly in our public headers,
# so they're PUBLIC — anything linking eng_sim gets them too.
target_link_libraries(eng_sim
  PUBLIC eng_core EnTT::EnTT glm::glm
  PRIVATE eng_warnings
)
