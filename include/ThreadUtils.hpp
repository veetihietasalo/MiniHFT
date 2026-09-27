#pragma once

#include <iostream>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX // keep windows.h from defining min/max macros that break std::min
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
    #include <system_error>
#endif

namespace ThreadUtils {

#if defined(_WIN32)

    // Pin current thread to a specific core ID (0, 1, 2...)
    [[nodiscard]] inline bool pinThread(int coreId) {
        // An affinity mask has one bit per processor in the thread's processor group, 64 at
        // most. Shifting by 64 or more, or by a negative amount, is undefined behaviour (in
        // practice x86 masks the shift and `1 << 64` pins to core 0), so check the range first.
        constexpr int kMaskBits = static_cast<int>(sizeof(DWORD_PTR) * 8);
        if (coreId < 0 || coreId >= kMaskBits) {
            std::cerr << "Failed to pin thread to core " << coreId << ". Error: core id out of range\n";
            return false;
        }
        HANDLE threadHandle = GetCurrentThread();
        DWORD_PTR mask = DWORD_PTR{1} << coreId;

        DWORD_PTR result = SetThreadAffinityMask(threadHandle, mask);
        if (result == 0) {
            std::cerr << "Failed to pin thread to core " << coreId << ". Error: " << GetLastError() << "\n";
            return false;
        }
        return true;
    }

    // Set thread priority to highest
    inline void setHighPriority() {
        HANDLE threadHandle = GetCurrentThread();
        SetThreadPriority(threadHandle, THREAD_PRIORITY_TIME_CRITICAL);
    }

    // Logical CPU the calling thread is running on right now
    [[nodiscard]] inline int currentCore() {
        return static_cast<int>(GetCurrentProcessorNumber());
    }

#else

    // Pin current thread to a specific core ID (0, 1, 2...)
    [[nodiscard]] inline bool pinThread(int coreId) {
        // CPU_SET outside [0, CPU_SETSIZE) writes past the bitmap, so reject those ids up front.
        // Ids inside that range but not present on this machine are rejected by the kernel (EINVAL).
        if (coreId < 0 || coreId >= CPU_SETSIZE) {
            std::cerr << "Failed to pin thread to core " << coreId << ". Error: core id out of range\n";
            return false;
        }

        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<size_t>(coreId), &set);

        // Returns the error number directly; it does not set errno. The message comes from
        // generic_category(), not strerror(): the producer and consumer threads both pin
        // themselves at startup, and strerror() may return a buffer another call overwrites.
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set);
        if (rc != 0) {
            std::cerr << "Failed to pin thread to core " << coreId << ". Error: " << std::generic_category().message(rc) << "\n";
            return false;
        }
        return true;
    }

    // Best effort: SCHED_FIFO needs root or CAP_SYS_NICE, so this usually fails for normal users.
    // A SCHED_FIFO thread that busy-spins can starve everything else on its core, so only
    // combine it with pinning to an isolated core.
    inline void setHighPriority() {
        sched_param param{};
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
        if (rc != 0) {
            std::cerr << "SCHED_FIFO unavailable: " << std::generic_category().message(rc) << "\n";
        }
    }

    // Logical CPU the calling thread is running on right now
    [[nodiscard]] inline int currentCore() {
        return sched_getcpu();
    }

#endif
}
