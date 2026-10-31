cmake_minimum_required(VERSION 3.28)
project(tick-to-trade LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(ttt INTERFACE)
target_include_directories(ttt INTERFACE include)
target_compile_options(ttt INTERFACE -Wall -Wextra -Wpedantic -Werror)
# x86-64-v3, as the repos before it: a named level, recorded in every run's flags.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(TTT_ARCH_FLAGS -march=x86-64-v3)
  target_compile_options(ttt INTERFACE ${TTT_ARCH_FLAGS})
endif()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()

# The repos before it (note 001), each fetched at a pinned commit into
# build/_deps, headers only: SOURCE_SUBDIR names no directory, so their own
# tests, benchmarks and baselines are not built here.
include(FetchContent)
set(TTT_LFIPC_COMMIT c733131c95198f08cc9032b7ae105cfaf6f7cd88)
set(TTT_ITCHFH_COMMIT a1a3d013ccf243b93ae763df2f99be3504af871d)
FetchContent_Declare(lockfree_ipc
  GIT_REPOSITORY https://github.com/Mukachi/lockfree-ipc.git
  GIT_TAG ${TTT_LFIPC_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR none)
FetchContent_Declare(itch_feed_handler
  GIT_REPOSITORY https://github.com/Mukachi/itch-feed-handler.git
  GIT_TAG ${TTT_ITCHFH_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(lockfree_ipc itch_feed_handler)

# One queue and one harness (note 001): the feed handler must pin the same
# lockfree-ipc as this repo.
file(STRINGS ${itch_feed_handler_SOURCE_DIR}/CMakeLists.txt itchfh_pin REGEX "^set\\(ITCHFH_LFIPC_COMMIT ")
string(REGEX REPLACE "^set\\(ITCHFH_LFIPC_COMMIT ([0-9a-f]+)\\)$" "\\1" itchfh_pin "${itchfh_pin}")
if(NOT itchfh_pin STREQUAL TTT_LFIPC_COMMIT)
  message(FATAL_ERROR "itch-feed-handler pins lockfree-ipc at '${itchfh_pin}', this repo at ${TTT_LFIPC_COMMIT}")
endif()

# Build provenance for this repository, written by lockfree-ipc's script into
# lfipc/build_info.hpp, where its environment capture reads it (lockfree-ipc
# note 004).
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
add_custom_target(build_info ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
          -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
          "-DFLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} -Wall -Wextra -Wpedantic -Werror ${TTT_ARCH_FLAGS}"
          -P ${lockfree_ipc_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
  COMMENT "Recording build provenance")

add_library(deps INTERFACE)
target_include_directories(deps SYSTEM INTERFACE ${lockfree_ipc_SOURCE_DIR}/include ${itch_feed_handler_SOURCE_DIR}/include
                                                 ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(deps INTERFACE TTT_LFIPC_COMMIT="${TTT_LFIPC_COMMIT}" TTT_ITCHFH_COMMIT="${TTT_ITCHFH_COMMIT}")
add_dependencies(deps build_info)
target_link_libraries(ttt INTERFACE deps Threads::Threads)

# The sample day (note 001): not in the repo and never redistributed;
# itch-feed-handler's tools/fetch_day.py fetches it.
set(TTT_DATA_DIR ${CMAKE_SOURCE_DIR}/data CACHE PATH "Where the sample day is")

# Tests: plain executables under CTest. Each one added with its component.
set(TTT_TESTS strategy_test ring_test sink_test)
foreach(t ${TTT_TESTS})
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE ttt)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# The feed handler's synthetic stream (its tests/), for S1 in small.
target_include_directories(sink_test PRIVATE ${itch_feed_handler_SOURCE_DIR}/tests)
