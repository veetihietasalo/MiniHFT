# Tools around the benchmarks that need no compiler. bench/compare.py compares the harnesses'
# --json results (and Google Benchmark JSON) against a baseline; its unit tests run under ctest
# wherever Python 3 is found.
if(MINIHFT_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        enable_testing()
        # -B: no __pycache__ in the source tree.
        add_test(NAME BenchTools.ComparePy
                 COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/bench/test_compare.py")
    else()
        message(STATUS "Python 3 not found: bench/test_compare.py is not run by ctest")
    endif()
endif()
