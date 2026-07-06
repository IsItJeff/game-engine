# The networking library. Everything here is header-only for now (an interface,
# a loopback queue, and the server that ties transport + world together), so
# it's an INTERFACE library — no .cpp to compile, just headers and their deps.
# Real UDP/GameNetworkingSockets code arrives at roadmap M3 and turns this into
# a normal compiled library.
add_library(eng_net INTERFACE)
target_link_libraries(eng_net INTERFACE eng_sim)
