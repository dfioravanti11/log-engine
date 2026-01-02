add_executable(bench_echo echo.cpp)
target_link_libraries(bench_echo PRIVATE
  logengine_runtime logengine_wire logengine_flags)
set_target_properties(bench_echo PROPERTIES OUTPUT_NAME echo)

find_package(Threads REQUIRED)
target_link_libraries(bench_echo PRIVATE Threads::Threads)
