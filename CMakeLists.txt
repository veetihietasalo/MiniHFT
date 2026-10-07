cmake_minimum_required(VERSION 3.25)
project(MiniHFT VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIHFT_BUILD_TESTS "Build unit tests (fetches GoogleTest)" ON)
option(MINIHFT_BUILD_BENCHMARKS "Build benchmarks (fetches Google Benchmark)" ON)
option(MINIHFT_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(MINIHFT_WARNINGS_AS_ERRORS "Treat warnings in MiniHFT's own code as errors" OFF)
set(MINIHFT_SANITIZER "" CACHE STRING "GCC/Clang sanitizers to build everything with, e.g. thread or address,undefined")

# Sanitizers go on every target, dependencies included: TSan needs all code
# that touches shared memory to be instrumented to avoid false reports.
# -fno-sanitize-recover makes the first report stop the program, so a test can't pass after one.
if(MINIHFT_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "MINIHFT_SANITIZER is only supported with GCC/Clang")
    endif()
    add_compile_options(-fsanitize=${MINIHFT_SANITIZER} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${MINIHFT_SANITIZER})
endif()

find_package(Threads REQUIRED)

# Header-only core: programs, tests and benchmarks link this for the include path and warnings.
# The warnings apply to our own code only; dependencies are included as SYSTEM headers.
add_library(minihft_core INTERFACE)
target_include_directories(minihft_core INTERFACE include)
target_link_libraries(minihft_core INTERFACE Threads::Threads)
if(MSVC)
    # C4324 "structure was padded due to alignment specifier": that padding is the point of alignas(64).
    target_compile_options(minihft_core INTERFACE /W4 /wd4324)
    # Standard fopen/tmpfile, not MSVC's non-portable *_s replacements.
    target_compile_definitions(minihft_core INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(MINIHFT_WARNINGS_AS_ERRORS)
        target_compile_options(minihft_core INTERFACE /WX)
    endif()
else()
    target_compile_options(minihft_core INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wdouble-promotion
        -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2 -Wimplicit-fallthrough
        # -Wnull-dereference runs after inlining, and GCC reports it inside GoogleTest's own
        # assertion code even from a SYSTEM header, so the test binary goes without it.
        $<$<NOT:$<STREQUAL:$<TARGET_PROPERTY:NAME>,minihft_tests>>:-Wnull-dereference>)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(minihft_core INTERFACE -Wduplicated-cond -Wlogical-op)
    endif()
    if(MINIHFT_WARNINGS_AS_ERRORS)
        target_compile_options(minihft_core INTERFACE -Werror)
    endif()
endif()

# Every header must compile on its own: one generated source file per header, which includes
# only that header. A header that leans on its includer's includes fails here, and headers no
# program includes still get compiled with the warnings above.
file(GLOB MINIHFT_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
if(NOT WIN32)
    list(FILTER MINIHFT_HEADERS EXCLUDE REGEX "/Network\\.hpp$")
endif()
foreach(header IN LISTS MINIHFT_HEADERS)
    get_filename_component(name "${header}" NAME)
    set(check "${CMAKE_CURRENT_BINARY_DIR}/header_check/${name}.cpp")
    file(CONFIGURE OUTPUT "${check}" CONTENT "#include \"${name}\"\n")
    list(APPEND MINIHFT_HEADER_CHECKS "${check}")
endforeach()
add_library(minihft_header_check OBJECT ${MINIHFT_HEADER_CHECKS})
target_link_libraries(minihft_header_check PRIVATE minihft_core)

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

# Latency harnesses: TSC timestamps into LatencyHistogram, reported as percentiles.
foreach(harness IN ITEMS ring_latency ring_study orderbook_latency itch_replay)
    add_executable(${harness} bench/${harness}.cpp)
    target_link_libraries(${harness} PRIVATE minihft_core)
endforeach()

include(FetchContent)

if(MINIHFT_BUILD_TESTS)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM) # its headers can't trigger our warnings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # match MSVC's default /MD runtime
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    set(MINIHFT_TEST_SOURCES
        tests/itch_parser_test.cpp
        tests/kernel_bypass_test.cpp
        tests/l3_order_book_test.cpp
        tests/latency_histogram_test.cpp
        tests/listener_test.cpp
        tests/matrix_statistics_test.cpp
        tests/object_pool_test.cpp
        tests/order_index_test.cpp
        tests/ring_buffer_test.cpp
        tests/ring_variants_test.cpp
        tests/thread_utils_test.cpp
        tests/tsc_test.cpp)
    # The zero-allocation tests replace global operator new/delete for the test binary. Sanitizer
    # runtimes define their own, so those tests are left out of sanitizer builds.
    if(NOT MINIHFT_SANITIZER)
        list(APPEND MINIHFT_TEST_SOURCES tests/allocation_counter.cpp tests/zero_alloc_test.cpp)
    endif()
    add_executable(minihft_tests ${MINIHFT_TEST_SOURCES})
    target_link_libraries(minihft_tests PRIVATE minihft_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(minihft_tests DISCOVERY_MODE PRE_TEST)

    # Deliberately broken queue (publish() relaxed). In a ThreadSanitizer build this test
    # passes only if TSan reports the data race: proof that the tool catches the bug.
    add_executable(relaxed_publish_demo tests/relaxed_publish_demo.cpp)
    target_link_libraries(relaxed_publish_demo PRIVATE minihft_core)
    if(MINIHFT_SANITIZER STREQUAL "thread")
        add_test(NAME TSan.CatchesRelaxedPublish COMMAND relaxed_publish_demo 20000)
        set_tests_properties(TSan.CatchesRelaxedPublish PROPERTIES
            PASS_REGULAR_EXPRESSION "WARNING: ThreadSanitizer: data race")
    endif()
endif()

if(MINIHFT_BUILD_BENCHMARKS)
    FetchContent_Declare(benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
        URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM) # its headers can't trigger our warnings
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(ring_buffer_bench bench/ring_buffer_bench.cpp)
    target_link_libraries(ring_buffer_bench PRIVATE minihft_core benchmark::benchmark)
endif()

# Coverage-guided fuzzing of the ITCH reader and dispatch (clang-fuzz preset).
if(MINIHFT_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MINIHFT_BUILD_FUZZERS needs Clang (libFuzzer)")
    endif()
    add_executable(itch_fuzz tests/fuzz/itch_fuzz.cpp)
    target_link_libraries(itch_fuzz PRIVATE minihft_core)
    target_compile_options(itch_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(itch_fuzz PRIVATE -fsanitize=fuzzer)
endif()

include(cmake/BenchTools.cmake)
