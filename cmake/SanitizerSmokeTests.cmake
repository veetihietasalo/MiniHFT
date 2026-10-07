# Tests that run the project's threaded code under more schedules, and every program at least
# once. Included from CMakeLists.txt inside the MINIHFT_BUILD_TESTS block.
#
#   Stress.<queue>.<schedule>   every build, label "stress": each SPSC hand-off (RingBuffer, the
#                               W03 variants, KernelBypass) under a busy-spin schedule and under
#                               seeded random delays, with 1024-slot and 2- or 4-slot queues.
#                               No seed is given, so each run picks one and prints it.
#   TSan.CatchesRelaxedConsume  ThreadSanitizer builds: passes only if TSan reports the race in a
#                               queue whose consume() is relaxed (tests/RelaxedConsumeRingBuffer.hpp)
#   Smoke.<program>             sanitizer builds, label "smoke": every program with small inputs,
#                               so the harnesses' own threads and handshakes run under the sanitizer
#
# Hammer the lock-free code: ctest --preset clang-tsan -L stress --repeat until-fail:10

add_executable(spsc_stress tests/spsc_stress.cpp)
target_link_libraries(spsc_stress PRIVATE minihft_core)

# Under ThreadSanitizer 100,000 messages take about 0.2-0.5 s per test.
set(MINIHFT_STRESS_MESSAGES 100000)
foreach(queue IN ITEMS ring ring-tiny v0 v0-tiny v1 v1-tiny v2 v2-one-line v2-batch4 v2-tiny-batch kernel-bypass)
    foreach(schedule IN ITEMS tight random)
        add_test(NAME Stress.${queue}.${schedule} COMMAND spsc_stress ${queue} ${schedule} ${MINIHFT_STRESS_MESSAGES})
        set_tests_properties(Stress.${queue}.${schedule} PROPERTIES LABELS stress TIMEOUT 60)
    endforeach()
endforeach()

# The race needs the producer to reuse a slot the consumer has read, so it shows from the third
# message in a 2-slot queue, whatever the schedule.
if(MINIHFT_SANITIZER STREQUAL "thread")
    add_test(NAME TSan.CatchesRelaxedConsume COMMAND spsc_stress relaxed-consume-tiny tight 1000 1)
    set_tests_properties(TSan.CatchesRelaxedConsume PROPERTIES
        PASS_REGULAR_EXPRESSION "WARNING: ThreadSanitizer: data race"
        TIMEOUT 60)
endif()

# Each program runs in a few seconds or less under ThreadSanitizer. The ring harnesses pin their
# threads (cores 2 and 3; ring_bench 1 and 2). Without those cores they print "NOT pinned" and
# carry on. They fail on out-of-order messages or on a negative TSC delta (a message received
# before it was sent: the cores' TSCs disagree); a lost message hangs the run until TIMEOUT.
if(MINIHFT_SANITIZER)
    set(MINIHFT_SMOKE_DIR "${CMAKE_CURRENT_BINARY_DIR}/smoke")
    file(MAKE_DIRECTORY "${MINIHFT_SMOKE_DIR}")

    add_test(NAME Smoke.ring_latency.paced COMMAND ring_latency --messages=20000 --warmup=1000)
    add_test(NAME Smoke.ring_latency.burst COMMAND ring_latency --messages=20000 --warmup=1000 --interval-ns=0)
    # Core -1 exists nowhere: both threads report the failed pin (to std::cerr, at the same time)
    # and the run must still pass.
    add_test(NAME Smoke.ring_latency.unpinned
             COMMAND ring_latency --messages=20000 --warmup=1000 --producer=-1 --consumer=-1)
    add_test(NAME Smoke.ring_study COMMAND ring_study --burst=20000 --reps=2 --paced=2000)
    add_test(NAME Smoke.ring_study.busy_work
             COMMAND ring_study --burst=5000 --reps=1 --paced=1000 --producer-work-ns=100 --consumer-work-ns=300)
    add_test(NAME Smoke.ring_bench COMMAND ring_bench)
    set(MINIHFT_PINNED_SMOKE_TESTS
        Smoke.ring_latency.paced Smoke.ring_latency.burst Smoke.ring_study Smoke.ring_study.busy_work Smoke.ring_bench)
    # Two busy-spinning threads each, pinned to the same cores: one at a time.
    set_tests_properties(${MINIHFT_PINNED_SMOKE_TESTS} PROPERTIES RUN_SERIAL TRUE)

    add_test(NAME Smoke.orderbook_latency COMMAND orderbook_latency --depths=10,100 --events=10000)
    add_test(NAME Smoke.gen_itch COMMAND gen_itch WORKING_DIRECTORY "${MINIHFT_SMOKE_DIR}")
    add_test(NAME Smoke.itch_test COMMAND itch_test market_data.itch WORKING_DIRECTORY "${MINIHFT_SMOKE_DIR}")
    add_test(NAME Smoke.itch_replay COMMAND itch_replay market_data.itch WORKING_DIRECTORY "${MINIHFT_SMOKE_DIR}")
    set_tests_properties(Smoke.gen_itch PROPERTIES FIXTURES_SETUP itch_sample_file)
    set_tests_properties(Smoke.itch_test Smoke.itch_replay PROPERTIES FIXTURES_REQUIRED itch_sample_file)
    add_test(NAME Smoke.MiniHFT COMMAND main)
    add_test(NAME Smoke.hw_bench COMMAND hw_bench)

    set_tests_properties(${MINIHFT_PINNED_SMOKE_TESTS} Smoke.ring_latency.unpinned
        Smoke.orderbook_latency Smoke.gen_itch Smoke.itch_test Smoke.itch_replay Smoke.MiniHFT Smoke.hw_bench
        PROPERTIES LABELS smoke TIMEOUT 60)
endif()
