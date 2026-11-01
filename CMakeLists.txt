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
set(TTT_ITCHFH_COMMIT 36d7a92a6bcab383320697b8f072bf83bbfe4118)
set(TTT_FIXGW_COMMIT 4c1a05b2d1558156a04ba6f51a8a238c59bb3a8f)
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
FetchContent_Declare(fix_order_gateway
  GIT_REPOSITORY https://github.com/Mukachi/fix-order-gateway.git
  GIT_TAG ${TTT_FIXGW_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(lockfree_ipc itch_feed_handler fix_order_gateway)

# One queue and one harness (note 001): the feed handler must pin the same
# lockfree-ipc as this repo.
file(STRINGS ${itch_feed_handler_SOURCE_DIR}/CMakeLists.txt itchfh_pin REGEX "^set\\(ITCHFH_LFIPC_COMMIT ")
string(REGEX REPLACE "^set\\(ITCHFH_LFIPC_COMMIT ([0-9a-f]+)\\)$" "\\1" itchfh_pin "${itchfh_pin}")
if(NOT itchfh_pin STREQUAL TTT_LFIPC_COMMIT)
  message(FATAL_ERROR "itch-feed-handler pins lockfree-ipc at '${itchfh_pin}', this repo at ${TTT_LFIPC_COMMIT}")
endif()
file(STRINGS ${fix_order_gateway_SOURCE_DIR}/CMakeLists.txt fixgw_pin REGEX "^set\\(FIXGW_LFIPC_COMMIT ")
string(REGEX REPLACE "^set\\(FIXGW_LFIPC_COMMIT ([0-9a-f]+)\\)$" "\\1" fixgw_pin "${fixgw_pin}")
if(NOT fixgw_pin STREQUAL TTT_LFIPC_COMMIT)
  message(FATAL_ERROR "fix-order-gateway pins lockfree-ipc at '${fixgw_pin}', this repo at ${TTT_LFIPC_COMMIT}")
endif()

# The gateway's FIX 4.4 dictionary (its note 006), generated here as it is
# there: its script, the FIX Trading Community's orchestrations at the commit
# and hashes the gateway pins (checked against its CMakeLists), into build/.
set(TTT_ORCHESTRA_COMMIT cd24169a2abd8daba7c360987c7a46ca11873a12)
set(TTT_ORCHESTRA_FILES OrchestraFIX44.xml FIX44Session.xml)
set(TTT_ORCHESTRA_SHA256 a36262895e90bbcad2948e0c98072173a571a253ba63107a89617441c67a9f86
                         90232af493951c0d88447725712cb620056374e87cd019f4e26b11c51889e475)
file(READ ${fix_order_gateway_SOURCE_DIR}/CMakeLists.txt fixgw_cmake)
foreach(v ${TTT_ORCHESTRA_COMMIT} ${TTT_ORCHESTRA_SHA256})
  string(FIND "${fixgw_cmake}" "${v}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "fix-order-gateway no longer pins the orchestrations at ${v}")
  endif()
endforeach()
set(TTT_ORCHESTRA_DIR ${CMAKE_BINARY_DIR}/_deps/fix_orchestrations)
foreach(name hash IN ZIP_LISTS TTT_ORCHESTRA_FILES TTT_ORCHESTRA_SHA256)
  if(NOT EXISTS ${TTT_ORCHESTRA_DIR}/${name})
    file(DOWNLOAD "https://raw.githubusercontent.com/FIXTradingCommunity/orchestrations/${TTT_ORCHESTRA_COMMIT}/FIX%20Standard/${name}"
         ${TTT_ORCHESTRA_DIR}/${name} EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
  endif()
endforeach()
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/generated/fixgw/fix44_dictionary.hpp
  COMMAND Python3::Interpreter -I ${fix_order_gateway_SOURCE_DIR}/tools/gen_dictionary.py
          ${TTT_ORCHESTRA_DIR}/OrchestraFIX44.xml ${TTT_ORCHESTRA_DIR}/FIX44Session.xml
          ${TTT_ORCHESTRA_COMMIT} ${CMAKE_BINARY_DIR}/generated/fixgw/fix44_dictionary.hpp
  DEPENDS ${fix_order_gateway_SOURCE_DIR}/tools/gen_dictionary.py ${TTT_ORCHESTRA_DIR}/OrchestraFIX44.xml
          ${TTT_ORCHESTRA_DIR}/FIX44Session.xml
  COMMENT "Generating the gateway's FIX 4.4 dictionary")
add_custom_target(fix44_dictionary DEPENDS ${CMAKE_BINARY_DIR}/generated/fixgw/fix44_dictionary.hpp)

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
                                                 ${fix_order_gateway_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated
                                                 ${itch_feed_handler_SOURCE_DIR}/bench)  # its stamps.hpp (note 005)
target_compile_definitions(deps INTERFACE TTT_LFIPC_COMMIT="${TTT_LFIPC_COMMIT}" TTT_ITCHFH_COMMIT="${TTT_ITCHFH_COMMIT}"
                                          TTT_FIXGW_COMMIT="${TTT_FIXGW_COMMIT}")
add_dependencies(deps build_info fix44_dictionary)
target_link_libraries(ttt INTERFACE deps Threads::Threads)

# The sample day (note 001): not in the repo and never redistributed;
# itch-feed-handler's tools/fetch_day.py fetches it.
set(TTT_DATA_DIR ${CMAKE_SOURCE_DIR}/data CACHE PATH "Where the sample day is")

# Tests: plain executables under CTest. Each one added with its component.
set(TTT_TESTS strategy_test ring_test sink_test risk_test orders_test config_test)
foreach(t ${TTT_TESTS})
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE ttt)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# S5 of note 002: the hot paths allocate nothing and make no system call,
# checked in the compiled code (not under sanitizers, which add calls).
add_executable(hot_path_subject tests/hot_path_subject.cpp)
target_link_libraries(hot_path_subject PRIVATE ttt)
if(CMAKE_OBJDUMP AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  add_test(NAME check_hot_path
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_hot_path.py
                   ${CMAKE_OBJDUMP} $<TARGET_FILE:hot_path_subject>)
endif()
target_compile_definitions(risk_test PRIVATE TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
target_compile_definitions(config_test PRIVATE TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
# The feed handler's synthetic stream (its tests/), for S1 in small.
target_include_directories(sink_test PRIVATE ${itch_feed_handler_SOURCE_DIR}/tests)

# D5's tool: the instrument lists for the benchmarks, ranked over the open (config/open-<n>.txt).
add_executable(rank_open tools/rank_open.cpp)
target_link_libraries(rank_open PRIVATE ttt)
target_compile_definitions(rank_open PRIVATE TTT_DATA_DIR="${TTT_DATA_DIR}" TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")

# Over the sample day (label `data`: needs data/, so CI leaves it out).
add_executable(day_test tests/day_test.cpp)
target_link_libraries(day_test PRIVATE ttt)
target_include_directories(day_test PRIVATE tools)
target_compile_definitions(day_test PRIVATE TTT_DATA_DIR="${TTT_DATA_DIR}" TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
add_test(NAME day_test COMMAND day_test)
set_tests_properties(day_test PROPERTIES LABELS data TIMEOUT 14400)

# The benchmark (note 002, S6): writes into results/ only in bench mode.
add_executable(sink_bench bench/sink_bench.cpp)
target_link_libraries(sink_bench PRIVATE ttt)
target_include_directories(sink_bench PRIVATE tools)
target_compile_definitions(sink_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results" TTT_DATA_DIR="${TTT_DATA_DIR}"
                                              TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")

# One number everywhere: a stale RESULTS.md or README figure fails the suite.
add_test(NAME report_check COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/run.py report --check
                                  --lockfree-ipc ${lockfree_ipc_SOURCE_DIR})
add_executable(risk_day_test tests/risk_day_test.cpp)
target_link_libraries(risk_day_test PRIVATE ttt)
target_include_directories(risk_day_test PRIVATE tools)
target_compile_definitions(risk_day_test PRIVATE TTT_DATA_DIR="${TTT_DATA_DIR}" TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
add_test(NAME risk_day_test COMMAND risk_day_test)
set_tests_properties(risk_day_test PROPERTIES LABELS data TIMEOUT 14400)

# The risk checks' benchmark (note 003, S7): writes into results/ only in bench mode.
add_executable(risk_bench bench/risk_bench.cpp)
target_link_libraries(risk_bench PRIVATE ttt)
target_include_directories(risk_bench PRIVATE tools tests)
target_compile_definitions(risk_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results" TTT_DATA_DIR="${TTT_DATA_DIR}"
                                              TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")

# The program and its tools (note 004).
# The library target is `ttt`, so the program's target is ttt_program, its file `ttt`.
add_executable(ttt_program apps/ttt.cpp)
set_target_properties(ttt_program PROPERTIES OUTPUT_NAME ttt)
add_executable(ttt-ctl apps/ttt_ctl.cpp)
add_executable(ttt-monitor apps/ttt_monitor.cpp)
foreach(p ttt_program ttt-ctl ttt-monitor)
  target_link_libraries(${p} PRIVATE ttt)
endforeach()

# The peers, built here from their pinned sources (into build/): the feed
# handler's sender of the sample day and the gateway's venue (note 004).
add_executable(feed_sender ${itch_feed_handler_SOURCE_DIR}/bench/feed_sender.cpp)
target_link_libraries(feed_sender PRIVATE ttt)
target_include_directories(feed_sender PRIVATE ${itch_feed_handler_SOURCE_DIR}/bench)
target_compile_definitions(feed_sender PRIVATE ITCHFH_DATA_DIR="${TTT_DATA_DIR}")
add_executable(order_venue ${fix_order_gateway_SOURCE_DIR}/bench/order_venue.cpp)
target_link_libraries(order_venue PRIVATE ttt)
add_executable(itchfh_program ${itch_feed_handler_SOURCE_DIR}/apps/itchfh.cpp)
set_target_properties(itchfh_program PROPERTIES OUTPUT_NAME itchfh)
target_link_libraries(itchfh_program PRIVATE ttt)

# Through the programs (note 004, S1 and S6; label `data`: needs data/).
add_executable(program_day_test tests/program_day_test.cpp)
target_link_libraries(program_day_test PRIVATE ttt)
target_compile_definitions(program_day_test PRIVATE TTT_DATA_DIR="${TTT_DATA_DIR}" TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
  TTT_BUILD_DIR="${CMAKE_BINARY_DIR}" TTT_SENDER="$<TARGET_FILE:feed_sender>" TTT_VENUE="$<TARGET_FILE:order_venue>"
  TTT_ITCHFH="$<TARGET_FILE:itchfh_program>" TTT_PROGRAM="$<TARGET_FILE:ttt_program>")
add_dependencies(program_day_test feed_sender order_venue itchfh_program ttt_program)
add_test(NAME program_day_test COMMAND program_day_test)
set_tests_properties(program_day_test PROPERTIES LABELS data TIMEOUT 14400)
add_executable(program_s2_test tests/program_s2_test.cpp)
target_link_libraries(program_s2_test PRIVATE ttt)
target_compile_definitions(program_s2_test PRIVATE TTT_DATA_DIR="${TTT_DATA_DIR}" TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
  TTT_BUILD_DIR="${CMAKE_BINARY_DIR}" TTT_SENDER="$<TARGET_FILE:feed_sender>" TTT_VENUE="$<TARGET_FILE:order_venue>"
  TTT_PROGRAM="$<TARGET_FILE:ttt_program>" TTT_CTL="$<TARGET_FILE:ttt-ctl>")
add_dependencies(program_s2_test feed_sender order_venue ttt_program ttt-ctl)
add_test(NAME program_s2_test COMMAND program_s2_test)
set_tests_properties(program_s2_test PROPERTIES LABELS data TIMEOUT 3600)

# Tick to trade (note 005): writes into results/ only in bench mode.
add_executable(t2t_bench bench/t2t_bench.cpp)
target_link_libraries(t2t_bench PRIVATE ttt)
target_include_directories(t2t_bench PRIVATE tests)
target_compile_definitions(t2t_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results" TTT_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
  TTT_BUILD_DIR="${CMAKE_BINARY_DIR}" TTT_VENUE="$<TARGET_FILE:order_venue>" TTT_ITCHFH="$<TARGET_FILE:itchfh_program>"
  TTT_PROGRAM="$<TARGET_FILE:ttt_program>")
add_dependencies(t2t_bench order_venue itchfh_program ttt_program)
