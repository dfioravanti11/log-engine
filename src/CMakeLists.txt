# One target per module. The link edges below ARE the dependency rules from
# docs/architecture.md §4 — a cycle or an illegal edge fails the build rather than
# waiting to be noticed in review.

add_library(logengine_base
  base/crc32c.cpp)
target_include_directories(logengine_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logengine_base PRIVATE logengine_flags)

add_library(logengine_io
  io/real/real_clock.cpp
  io/real/real_random.cpp
  io/real/real_network.cpp
  io/real/real_disk.cpp)
target_link_libraries(logengine_io PUBLIC logengine_base PRIVATE logengine_flags)

add_library(logengine_wire
  wire/frame.cpp)
# wire/ must NOT link io/: framing is pure byte manipulation, and keeping the OS out
# of it is what lets the fuzzer drive the decoder with no I/O at all.
target_link_libraries(logengine_wire PUBLIC logengine_base PRIVATE logengine_flags)

add_library(logengine_runtime
  runtime/event_loop.cpp)
target_link_libraries(logengine_runtime PUBLIC logengine_base logengine_io
                      PRIVATE logengine_flags)

# storage/ links io/ for the *interfaces* only — it never sees io/real/. The link edge
# below is what lets the simulator swap in io/sim/ in week 3 without storage/ noticing.
add_library(logengine_storage
  storage/record_batch.cpp
  storage/sparse_index.cpp
  storage/segment.cpp
  storage/log.cpp)
target_link_libraries(logengine_storage PUBLIC logengine_base logengine_io logengine_wire
                      PRIVATE logengine_flags)

# raft/ links io/ for the Random *interface* and nothing else — election-timeout jitter
# has to come from the one seed like everything else. No Disk, no Network, not even a
# Clock: the state machine counts ticks and asks the driver to act on its behalf, so the
# ER-1 line here is not "goes through io/" but "has no way to reach the OS at all".
add_library(logengine_raft
  raft/node.cpp
  raft/state_file.cpp
  raft/codec.cpp)
target_link_libraries(logengine_raft PUBLIC logengine_base logengine_io logengine_wire
                      PRIVATE logengine_flags)

# server/ is the broker: storage + raft + the wiring, over injected io/ interfaces.
#
# **It links no simulator code and the simulator links it.** That direction is the whole
# architectural bet — the driver that implements §13's persist-then-send exists once, and
# `sim/` runs *that object* against `io/sim/` while the real binary runs it against
# `io/real/`. A second copy here would mean the simulator validates a sibling of the
# shipped code rather than the shipped code.
add_library(logengine_server
  server/broker.cpp)
target_link_libraries(logengine_server PUBLIC logengine_raft logengine_storage
                      logengine_runtime PRIVATE logengine_flags)

# The simulator. io/sim/ and sim/ are one target on purpose: a simulated Disk with a
# private notion of time would not be a simulation of anything. They share one clock,
# one event queue, and one seed, so splitting them would only buy a circular link edge.
#
# It links storage/ because the week 3 workload drives the real log through the
# simulated disk — the whole point is that the code under test is unmodified.
add_library(logengine_sim
  sim/trace.cpp
  sim/scheduler.cpp
  sim/simulation.cpp
  sim/workload.cpp
  io/sim/sim_disk.cpp
  io/sim/sim_network.cpp)
target_link_libraries(logengine_sim PUBLIC logengine_storage logengine_runtime
                      logengine_raft logengine_server PRIVATE logengine_flags)

# The product. One broker process, `io/real/` wired into the same `server::Broker` the
# simulator drives — that substitution *is* the architectural bet, and this target is
# where it either compiles or doesn't.
add_executable(logengine main/logengine.cpp)
target_link_libraries(logengine PRIVATE logengine_server logengine_io logengine_flags)
# For bench/histogram.h — the open-loop benchmark lives in the broker binary because the
# producer does, and the producer does because there is no client library yet.
target_include_directories(logengine PRIVATE ${CMAKE_SOURCE_DIR})

# Week 7+: logengine_client. Each links only downward.
