include(FetchContent)

# Prefers a system GoogleTest when one exists (FIND_PACKAGE_ARGS), otherwise fetches.
# Keeps a clean-machine build working without asking the user to install anything,
# which acceptance criterion #1 requires.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  FIND_PACKAGE_ARGS NAMES GTest)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

function(logengine_test name)
  add_executable(${name} ${ARGN})
  # logengine_storage re-exports base + io + wire PUBLIC and logengine_runtime
  # re-exports base + io, so naming any of those here only duplicates them on the
  # link line — which the linker warns about.
  target_link_libraries(${name} PRIVATE
    logengine_sim logengine_runtime logengine_storage logengine_raft logengine_flags
    GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

logengine_test(test_crc32c      unit/test_crc32c.cpp)
logengine_test(test_buffer      unit/test_buffer.cpp)
logengine_test(test_frame       unit/test_frame.cpp)
logengine_test(test_event_loop  unit/test_event_loop.cpp)
logengine_test(test_disk        unit/test_disk.cpp)
logengine_test(test_network     unit/test_network.cpp)
logengine_test(test_random      unit/test_random.cpp)

# Week 2 — storage.
logengine_test(test_record_batch unit/test_record_batch.cpp)
logengine_test(test_sparse_index unit/test_sparse_index.cpp)
logengine_test(test_segment      unit/test_segment.cpp)
logengine_test(test_log          unit/test_log.cpp)

# Week 3 — the simulator.
logengine_test(test_scheduler    unit/test_scheduler.cpp)
logengine_test(test_sim_disk     unit/test_sim_disk.cpp)
logengine_test(test_sim_network  unit/test_sim_network.cpp)

# Week 4 — Raft elections. test_raft_election drives real elections with no event loop,
# no disk and no network at all, which is the point of keeping raft::Node pure.
logengine_test(test_raft_election   unit/test_raft_election.cpp)
logengine_test(test_raft_state_file unit/test_raft_state_file.cpp)

# Week 5 — replication. Same trick as the election tests: `raft::Node` holds no entries,
# so a "log" here is a vector<Term> and a "driver" is thirty lines of test harness.
logengine_test(test_raft_replication unit/test_raft_replication.cpp)

# Property tests: same harness, but each case drives a randomized input space from a
# fixed seed. RapidCheck is deliberately not a dependency yet — a seeded loop inside
# GoogleTest gives the same shrink-free coverage without a second library to build.
logengine_test(test_frame_property property/test_frame_roundtrip.cpp)

# Recovery is the property that matters most and the hardest to cover by hand: the
# interesting inputs are "a log truncated at byte N", for every N.
logengine_test(test_recovery_property property/test_segment_recovery.cpp)

# Simulation tests carry the `simulation` label, which the tsan preset excludes.
#
# That is ER-5 written down rather than a workaround: TSAN runs on the real runtime only,
# because the simulator is single-threaded by construction and has no data race to find.
# Until week 4 the exclusion existed only in the spec, so TSAN was spending its time
# proving nothing about code it could not fault — and once Raft added ticks, heartbeats
# and an fsync per vote, "nothing, slowly" became "nothing, past the 600 s timeout".
# The binaries are still fully covered by ASAN and UBSAN, which are the ones that can
# actually find something here.
#
# The determinism canary (I7) — a required check from week 3 on. When it fails, the
# fix is to diff two captured traces, not to stare at the code.
logengine_test(test_determinism simulation/test_determinism.cpp)
set_tests_properties(test_determinism PROPERTIES LABELS simulation)

# Week 4 — elections under faults, plus the test that deliberately breaks §13's fsync to
# show the I6 checker going red.
logengine_test(test_raft_cluster simulation/test_raft_cluster.cpp)
set_tests_properties(test_raft_cluster PROPERTIES TIMEOUT 600 LABELS simulation)
# 10 minutes, against ~3 s in the dev preset. The margin is for the sanitizer builds,
# where the same work legitimately takes ~3 minutes — and TSAN over the simulator is
# noise anyway (ER-5: the simulator is single-threaded by construction), so the timeout
# exists to stop a slow machine failing the build, not to bound anything meaningful.
set_tests_properties(test_determinism PROPERTIES TIMEOUT 600)

# Fuzzers link the code under test but not GoogleTest — libFuzzer supplies main().
if(LOGENGINE_BUILD_FUZZERS)
  add_executable(fuzz_batch_decode fuzz/fuzz_batch_decode.cpp)
  target_link_libraries(fuzz_batch_decode PRIVATE logengine_storage logengine_flags)
  target_compile_options(fuzz_batch_decode PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_batch_decode PRIVATE -fsanitize=fuzzer)
endif()
