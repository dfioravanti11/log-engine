cmake_minimum_required(VERSION 3.24)
project(logengine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(LOGENGINE_BUILD_TESTS "Build tests" ON)
option(LOGENGINE_BUILD_BENCH "Build benchmarks" ON)
# libFuzzer is a Clang built-in; the fuzz preset turns this on and nothing else does.
option(LOGENGINE_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
set(LOGENGINE_SANITIZER "none" CACHE STRING "none|address|undefined|thread|memory")

# Warnings + sanitizers are applied per-target, never globally: third-party code
# (GoogleTest) must not be compiled with -Werror.
add_library(logengine_flags INTERFACE)
target_compile_options(logengine_flags INTERFACE
  -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Werror)

if(NOT LOGENGINE_SANITIZER STREQUAL "none")
  set(_san_flags -fsanitize=${LOGENGINE_SANITIZER} -fno-omit-frame-pointer -g)
  if(LOGENGINE_SANITIZER STREQUAL "address")
    list(APPEND _san_flags -fsanitize-address-use-after-scope)
  endif()
  target_compile_options(logengine_flags INTERFACE ${_san_flags})
  target_link_options(logengine_flags INTERFACE -fsanitize=${LOGENGINE_SANITIZER})
  message(STATUS "logengine: sanitizer=${LOGENGINE_SANITIZER}")
endif()

add_subdirectory(src)
add_subdirectory(tools)

if(LOGENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LOGENGINE_BUILD_BENCH)
  add_subdirectory(bench)
endif()
