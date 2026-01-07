# Tools sit outside the ER-1 line on purpose: they are allowed to print, parse argv,
# and touch the real disk directly. Nothing above the io/ seam may link against them.

add_executable(tool_log_dump log_dump.cpp)
target_link_libraries(tool_log_dump PRIVATE logengine_storage logengine_flags)
set_target_properties(tool_log_dump PROPERTIES OUTPUT_NAME log-dump)

add_executable(tool_crash_demo crash_demo.cpp)
target_link_libraries(tool_crash_demo PRIVATE logengine_storage logengine_flags)
set_target_properties(tool_crash_demo PROPERTIES OUTPUT_NAME crash-demo)

add_executable(tool_sim sim.cpp)
target_link_libraries(tool_sim PRIVATE logengine_sim logengine_flags)
set_target_properties(tool_sim PROPERTIES OUTPUT_NAME sim)

# Week 4: sim-replay (§14.4) — `--break-at N` for a debugger session. Deferred on
# purpose: it is a tool for reproducing bugs from the journal, and the journal is empty
# until Raft gives the checker something to catch. `sim --seed X --dump-trace` covers
# reproduction until then.
