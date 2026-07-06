# The game client executable. It links the renderer (eng_gpu) and the networking
# layer (eng_net, which transitively brings the simulation and core). This is the
# only target that pulls in the GPU/window stack — the headless dedicated server
# (a future target) will link eng_net WITHOUT eng_gpu.
add_executable(game
  main.cpp
)

target_link_libraries(game
  PRIVATE eng_gpu eng_net eng_warnings
)
