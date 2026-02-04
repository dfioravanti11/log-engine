add_executable(bench_echo echo.cpp)
target_link_libraries(bench_echo PRIVATE
  logengine_runtime logengine_wire logengine_flags)
set_target_properties(bench_echo PROPERTIES OUTPUT_NAME echo)

find_package(Threads REQUIRED)
target_link_libraries(bench_echo PRIVATE Threads::Threads)

# Week 8. Failover time is measured *in the simulator* — see the comment at the top of
# failover.cpp for why that is a defensible choice rather than a convenient one.
add_executable(bench_failover failover.cpp)
target_link_libraries(bench_failover PRIVATE logengine_sim logengine_flags)
target_include_directories(bench_failover PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_target_properties(bench_failover PROPERTIES OUTPUT_NAME failover)
