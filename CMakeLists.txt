cmake_minimum_required(VERSION 3.25)
project(MiniHFT VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIHFT_BUILD_TESTS "Build unit tests (fetches GoogleTest)" ON)
option(MINIHFT_BUILD_BENCHMARKS "Build benchmarks (fetches Google Benchmark)" ON)
set(MINIHFT_SANITIZER "" CACHE STRING "GCC/Clang sanitizer to build everything with: thread, address, undefined")

# Sanitizers go on every target, dependencies included: TSan needs all code
# that touches shared memory to be instrumented to avoid false reports.
if(MINIHFT_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "MINIHFT_SANITIZER is only supported with GCC/Clang")
    endif()
    add_compile_options(-fsanitize=${MINIHFT_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${MINIHFT_SANITIZER})
endif()

find_package(Threads REQUIRED)

# Header-only core: programs, tests and benchmarks link this for the include path and warnings.
add_library(minihft_core INTERFACE)
target_include_directories(minihft_core INTERFACE include)
target_link_libraries(minihft_core INTERFACE Threads::Threads)
if(MSVC)
    target_compile_options(minihft_core INTERFACE /W4)
else()
    target_compile_options(minihft_core INTERFACE -Wall -Wextra -Wpedantic)
endif()

# One executable per program: each file in src/ has its own main().
set(MINIHFT_PROGRAMS main gen_itch itch_test hw_bench ring_bench)
if(WIN32)
    list(APPEND MINIHFT_PROGRAMS net_test) # Network.hpp is Winsock-only until it's ported (W09)
endif()
foreach(prog IN LISTS MINIHFT_PROGRAMS)
    add_executable(${prog} src/${prog}.cpp)
    target_link_libraries(${prog} PRIVATE minihft_core)
endforeach()
set_target_properties(main PROPERTIES OUTPUT_NAME MiniHFT)

include(FetchContent)

if(MINIHFT_BUILD_TESTS)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # match MSVC's default /MD runtime
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(minihft_tests
        tests/ring_buffer_test.cpp
        tests/thread_utils_test.cpp)
    target_link_libraries(minihft_tests PRIVATE minihft_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(minihft_tests DISCOVERY_MODE PRE_TEST)
endif()

if(MINIHFT_BUILD_BENCHMARKS)
    FetchContent_Declare(benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(ring_buffer_bench bench/ring_buffer_bench.cpp)
    target_link_libraries(ring_buffer_bench PRIVATE minihft_core benchmark::benchmark)
endif()
