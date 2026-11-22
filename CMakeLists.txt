cmake_minimum_required(VERSION 3.20)
project(tickbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -DTICKBOOK_SANITIZE=address | undefined | thread | address,undefined
set(TICKBOOK_SANITIZE "" CACHE STRING "Sanitizers to enable")

add_library(tickbook INTERFACE)
target_include_directories(tickbook INTERFACE include)
target_compile_features(tickbook INTERFACE cxx_std_20)

add_library(tickbook_flags INTERFACE)
target_compile_options(tickbook_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
if(TICKBOOK_SANITIZE)
  target_compile_options(tickbook_flags INTERFACE -fsanitize=${TICKBOOK_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(tickbook_flags INTERFACE -fsanitize=${TICKBOOK_SANITIZE})
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_library(tickbook_csv_converter STATIC tools/csv_to_bin.cpp)
target_link_libraries(tickbook_csv_converter PUBLIC tickbook tickbook_flags ZLIB::ZLIB)

add_executable(csv_to_bin tools/csv_to_bin_main.cpp)
target_link_libraries(csv_to_bin PRIVATE tickbook_csv_converter tickbook_flags)

add_executable(replay tools/replay_main.cpp)
target_link_libraries(replay PRIVATE tickbook tickbook_flags Threads::Threads)

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
    tests/test_sequencer.cpp
    tests/test_csv_to_bin.cpp
    tests/test_binary_replay.cpp
    tests/test_replay.cpp)
  target_link_libraries(tickbook_tests PRIVATE tickbook tickbook_csv_converter tickbook_flags
                                               GTest::gtest_main Threads::Threads)
  include(GoogleTest)
  gtest_discover_tests(tickbook_tests)
endif()
