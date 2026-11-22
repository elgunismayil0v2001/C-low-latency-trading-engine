cmake_minimum_required(VERSION 3.20)
project(tickbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -DTICKBOOK_SANITIZE=address | undefined | thread | address,undefined
set(TICKBOOK_SANITIZE "" CACHE STRING "Sanitizers to enable")
option(TICKBOOK_BUILD_FUZZERS "Build libFuzzer parser targets (Clang only)" OFF)
option(TICKBOOK_BUILD_BENCHMARKS "Build Release-only replay benchmarks" OFF)
option(TICKBOOK_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)

add_library(tickbook INTERFACE)
target_include_directories(tickbook INTERFACE include)
target_compile_features(tickbook INTERFACE cxx_std_20)

add_library(tickbook_flags INTERFACE)
target_compile_options(tickbook_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
if(TICKBOOK_WARNINGS_AS_ERRORS)
  target_compile_options(tickbook_flags INTERFACE -Werror)
endif()
if(TICKBOOK_SANITIZE)
  target_compile_options(tickbook_flags INTERFACE -fsanitize=${TICKBOOK_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(tickbook_flags INTERFACE -fsanitize=${TICKBOOK_SANITIZE})
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_library(tickbook_csv_converter STATIC tools/csv_to_bin.cpp)
target_link_libraries(tickbook_csv_converter PUBLIC tickbook tickbook_flags ZLIB::ZLIB)

add_library(tickbook_synthetic_generator STATIC tools/gen_synthetic.cpp)
target_include_directories(tickbook_synthetic_generator PUBLIC tools)
target_link_libraries(tickbook_synthetic_generator PUBLIC tickbook_flags ZLIB::ZLIB)

add_executable(csv_to_bin tools/csv_to_bin_main.cpp)
target_link_libraries(csv_to_bin PRIVATE tickbook_csv_converter tickbook_flags)

add_executable(gen_synthetic tools/gen_synthetic_main.cpp)
target_link_libraries(gen_synthetic PRIVATE tickbook_synthetic_generator tickbook_flags)

add_executable(compare_books tools/compare_books.cpp)
target_link_libraries(compare_books PRIVATE tickbook tickbook_flags)

add_executable(replay tools/replay_main.cpp)
target_link_libraries(replay PRIVATE tickbook tickbook_flags Threads::Threads)

add_executable(alloc_check tools/alloc_check.cpp)
target_link_libraries(alloc_check PRIVATE tickbook tickbook_flags Threads::Threads)

if(TICKBOOK_BUILD_BENCHMARKS)
  if(TICKBOOK_SANITIZE)
    message(FATAL_ERROR "Replay benchmarks must be built without sanitizers")
  endif()
  add_executable(replay_latency bench/replay_latency.cpp)
  target_link_libraries(replay_latency PRIVATE tickbook tickbook_flags Threads::Threads)
  target_compile_options(replay_latency PRIVATE -O3 -DNDEBUG)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(tickbook_tests
    tests/test_types.cpp
    tests/test_binary_file.cpp
    tests/test_fixed.cpp
    tests/test_ring_buffer.cpp
    tests/test_book_map.cpp
    tests/test_flat_book.cpp
    tests/test_offset_book.cpp
    tests/test_books_differential.cpp
    tests/test_sequencer.cpp
    tests/test_csv_to_bin.cpp
    tests/test_csv_line.cpp
    tests/test_gen_synthetic.cpp
    tests/test_latency_histogram.cpp
    tests/test_binary_replay.cpp
    tests/test_replay.cpp)
  target_link_libraries(tickbook_tests PRIVATE tickbook tickbook_csv_converter
                                               tickbook_synthetic_generator tickbook_flags
                                               GTest::gtest_main Threads::Threads)
  include(GoogleTest)
  gtest_discover_tests(tickbook_tests)
  if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(TICKBOOK_TEST_TMP_DIR "$ENV{TMPDIR}")
  else()
    set(TICKBOOK_TEST_TMP_DIR "/tmp")
  endif()
  string(SHA256 TICKBOOK_BUILD_ID "${CMAKE_CURRENT_BINARY_DIR}")
  string(SUBSTRING "${TICKBOOK_BUILD_ID}" 0 12 TICKBOOK_BUILD_ID)
  add_test(NAME allocation_check
    COMMAND alloc_check "${TICKBOOK_TEST_TMP_DIR}/tickbook-allocation-check-${TICKBOOK_BUILD_ID}.bin")
endif()

if(TICKBOOK_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "TICKBOOK_BUILD_FUZZERS requires Clang with libFuzzer")
  endif()

  foreach(fuzzer IN ITEMS parse_fixed csv_line binary_file sequencer)
    add_executable(fuzz_${fuzzer} fuzz/fuzz_${fuzzer}.cpp)
    target_link_libraries(fuzz_${fuzzer} PRIVATE tickbook)
    target_compile_options(fuzz_${fuzzer} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow
      -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_options(fuzz_${fuzzer} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()
