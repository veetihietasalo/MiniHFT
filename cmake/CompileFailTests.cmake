# Compile-fail tests: proof that a real misuse of a constrained API doesn't compile, and fails
# for the expected reason. A static_assert(!Concept<X>) shows what a trait answers; these show
# that a call, an instantiation or an ignored [[nodiscard]] result is refused by the compiler.
#
# Each file in tests/compile_fail/ holds one misuse and is built twice:
#   - As is, in an EXCLUDE_FROM_ALL target. Its ctest test builds that target and passes only
#     if the build fails AND the output matches EXPECT: the concept's name, the static_assert's
#     message, or the warning's name.
#   - With MINIHFT_COMPILE_FAIL_CONTROL defined, in the normal build. That swaps the misuse for
#     its correct form, so the file must compile. A typo or a missing include then breaks the
#     build instead of passing the test by failing for the wrong reason.
#
# Each file is a small program (its own main(), so nothing in it is unused); both builds only
# compile it, nothing is linked.
#
# This file is also the script the tests run (cmake -P); that part comes first.

if(CMAKE_SCRIPT_MODE_FILE)
    # Build TARGET in the build tree BUILD_DIR. -j1: the target is one file, and the test then
    # doesn't compete for cores with the unit tests running next to it. For MSBuild it also
    # means no worker nodes, which could otherwise outlive the build and hold its output open.
    set(command "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}" --parallel 1)
    if(CONFIG)
        list(APPEND command --config "${CONFIG}") # multi-config generators (Visual Studio)
    endif()
    set(ENV{MSBUILDDISABLENODEREUSE} 1)
    execute_process(COMMAND ${command} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)

    # Both conditions, so a build that succeeds with only a warning can't pass by matching.
    if(result EQUAL 0)
        message(FATAL_ERROR "${TARGET} compiled, but it must not:\n${output}")
    endif()
    if(NOT output MATCHES "${EXPECT}")
        message(FATAL_ERROR "${TARGET} failed to compile, but not with an error matching \"${EXPECT}\":\n${output}")
    endif()
    message(STATUS "${TARGET} failed to compile as expected, matching \"${CMAKE_MATCH_0}\"")
    return()
endif()

# A compile error doesn't depend on the sanitizer, so the sanitizer builds skip all of this.
if(MINIHFT_SANITIZER)
    return()
endif()

# The control build of every compile-fail source: each must compile with the misuse removed.
add_library(minihft_compile_fail_controls OBJECT)
target_compile_definitions(minihft_compile_fail_controls PRIVATE MINIHFT_COMPILE_FAIL_CONTROL)
target_link_libraries(minihft_compile_fail_controls PRIVATE minihft_core)

# minihft_compile_fail_test(NAME <name> SOURCE <file> EXPECT <regex> [WARNING])
#
# Adds the test CompileFail.<name>, labelled compile-fail. EXPECT should name the constraint
# (a concept, a static_assert message, a warning) rather than a compiler's wording, so it holds
# for GCC, Clang and MSVC. WARNING marks a misuse that is only a warning, such as an ignored
# [[nodiscard]] result: it fails the build only with MINIHFT_WARNINGS_AS_ERRORS, so without
# that the test is left out (the control is still built).
function(minihft_compile_fail_test)
    cmake_parse_arguments(PARSE_ARGV 0 arg "WARNING" "NAME;SOURCE;EXPECT" "")
    if(NOT arg_NAME OR NOT arg_SOURCE OR NOT arg_EXPECT)
        message(FATAL_ERROR "minihft_compile_fail_test needs NAME, SOURCE and EXPECT")
    endif()

    target_sources(minihft_compile_fail_controls PRIVATE ${arg_SOURCE})
    if(arg_WARNING AND NOT MINIHFT_WARNINGS_AS_ERRORS)
        return()
    endif()

    set(target compile_fail_${arg_NAME})
    add_library(${target} OBJECT EXCLUDE_FROM_ALL ${arg_SOURCE})
    target_link_libraries(${target} PRIVATE minihft_core)
    # Broken on purpose, so it stays out of compile_commands.json and clang-tidy never sees
    # it. The control build of the same file stays in and is analysed.
    set_target_properties(${target} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)

    add_test(NAME CompileFail.${arg_NAME}
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DTARGET=${target}"
            "-DCONFIG=$<CONFIG>"
            "-DEXPECT=${arg_EXPECT}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    # The test builds in this build tree. Two builds in one tree at once could corrupt Ninja's
    # log or collide on MSBuild's files, so these tests run one at a time; the unit tests still
    # run in parallel with them.
    set_tests_properties(CompileFail.${arg_NAME} PROPERTIES
        LABELS compile-fail
        RESOURCE_LOCK minihft_build_tree)
endfunction()

# The tests. block() keeps these variables out of the including scope.
block(SCOPE_FOR VARIABLES)
    # What each misuse must be refused with. A concept or static_assert error must name the
    # concept or the message. On MSVC, "no matching overloaded function" (C2672) and
    # "constraints not satisfied" (C7602) are accepted too, in case a version leaves the
    # concept's name out of its notes: the control still proves the misuse is all that's wrong.
    set(itch_handler "ItchHandler|C2672")
    set(listener "TopOfBookListener|C7602")
    set(floating_point "floating_point|C2672")
    set(power_of_two "Size must be power of 2")
    # GCC and Clang report an ignored [[nodiscard]] result as -Wunused-result, MSVC as C4834.
    set(nodiscard "unused-result|C4834")

    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail")

    # Concepts
    minihft_compile_fail_test(NAME DispatchRejectsHandlerMissingAMethod
        SOURCE ${dir}/itch_handler_missing_method.cpp EXPECT "${itch_handler}")
    minihft_compile_fail_test(NAME DispatchRejectsHandlerTakingTheWrongMessage
        SOURCE ${dir}/itch_handler_wrong_message.cpp EXPECT "${itch_handler}")
    minihft_compile_fail_test(NAME BookBuilderRejectsNonListener
        SOURCE ${dir}/listener_missing_method.cpp EXPECT "${listener}")
    minihft_compile_fail_test(NAME BookBuilderRejectsListenerReturningAValue
        SOURCE ${dir}/listener_returns_value.cpp EXPECT "${listener}")
    minihft_compile_fail_test(NAME StatisticsMeanRejectsIntegers
        SOURCE ${dir}/statistics_mean_integer.cpp EXPECT "${floating_point}")
    minihft_compile_fail_test(NAME StatisticsVarianceRejectsIntegers
        SOURCE ${dir}/statistics_variance_integer.cpp EXPECT "${floating_point}")
    minihft_compile_fail_test(NAME StatisticsStdDevRejectsIntegers
        SOURCE ${dir}/statistics_stddev_integer.cpp EXPECT "${floating_point}")
    minihft_compile_fail_test(NAME StatisticsCovarianceRejectsIntegers
        SOURCE ${dir}/statistics_covariance_integer.cpp EXPECT "${floating_point}")
    minihft_compile_fail_test(NAME StatisticsMovingAverageRejectsIntegers
        SOURCE ${dir}/statistics_moving_average_integer.cpp EXPECT "${floating_point}")

    # static_asserts
    minihft_compile_fail_test(NAME RingBufferRejectsSizeNotPowerOfTwo
        SOURCE ${dir}/ring_buffer_size_not_power_of_two.cpp EXPECT "${power_of_two}")
    minihft_compile_fail_test(NAME RingBufferRejectsSizeZero
        SOURCE ${dir}/ring_buffer_size_zero.cpp EXPECT "${power_of_two}")
    minihft_compile_fail_test(NAME RingV0RejectsSizeZero
        SOURCE ${dir}/ring_v0_size_zero.cpp EXPECT "${power_of_two}")
    minihft_compile_fail_test(NAME RingV1RejectsSizeZero
        SOURCE ${dir}/ring_v1_size_zero.cpp EXPECT "${power_of_two}")
    minihft_compile_fail_test(NAME RingV2RejectsSizeZero
        SOURCE ${dir}/ring_v2_size_zero.cpp EXPECT "${power_of_two}")
    minihft_compile_fail_test(NAME RingV2RejectsBatchLargerThanSize
        SOURCE ${dir}/ring_v2_batch_larger_than_size.cpp EXPECT "Batch must be in \\[1, Size\\]")
    minihft_compile_fail_test(NAME ObjectPoolRejectsOverAlignedType
        SOURCE ${dir}/object_pool_over_aligned.cpp EXPECT "default alignment")
    minihft_compile_fail_test(NAME ObjectPoolRejectsBlockSizeZero
        SOURCE ${dir}/object_pool_block_size_zero.cpp EXPECT "BlockSize must be at least 1")
    # Deleted functions: GCC "use of deleted function", Clang "call to deleted constructor",
    # MSVC C2280 "attempting to reference a deleted function".
    minihft_compile_fail_test(NAME OrderIndexCannotBeMoved
        SOURCE ${dir}/order_index_move.cpp EXPECT "deleted|C2280")

    # [[nodiscard]]: calls that can fail, whose result must not be dropped
    minihft_compile_fail_test(NAME PinThreadResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_pin_thread.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME RingBufferClaimResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_ring_claim.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME RingBufferPeekResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_ring_peek.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME OrderIndexTryInsertResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_order_index_try_insert.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME OrderIndexFindResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_order_index_find.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME ObjectPoolAcquireResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_object_pool_acquire.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME KernelBypassNicReceiveResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_nic_receive.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME KernelBypassPollResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_nic_poll.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME ItchReaderNextResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_itch_reader_next.cpp EXPECT "${nodiscard}")
    minihft_compile_fail_test(NAME DispatchResultMustBeUsed WARNING
        SOURCE ${dir}/nodiscard_dispatch.cpp EXPECT "${nodiscard}")
endblock()
