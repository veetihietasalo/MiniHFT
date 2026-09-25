#pragma once

#include <thread>
#include <iostream>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX // keep windows.h from defining min/max macros that break std::min
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
    #include <cstring>
#endif

namespace ThreadUtils {

#if defined(_WIN32)

    // Pin current thread to a specific core ID (0, 1, 2...)
    inline bool pinThread(int coreId) {
        HANDLE threadHandle = GetCurrentThread();
        DWORD_PTR mask = (1ULL << coreId);

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
    inline int currentCore() {
        return static_cast<int>(GetCurrentProcessorNumber());
    }

#else

    // Pin current thread to a specific core ID (0, 1, 2...)
    inline bool pinThread(int coreId) {
        // CPU_SET outside [0, CPU_SETSIZE) writes past the bitmap, so reject those ids up front.
        // Ids inside that range but not present on this machine are rejected by the kernel (EINVAL).
        if (coreId < 0 || coreId >= CPU_SETSIZE) {
            std::cerr << "Failed to pin thread to core " << coreId << ". Error: core id out of range\n";
            return false;
        }

        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(coreId, &set);

        // Returns the error number directly; it does not set errno.
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set);
        if (rc != 0) {
            std::cerr << "Failed to pin thread to core " << coreId << ". Error: " << std::strerror(rc) << "\n";
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
            std::cerr << "SCHED_FIFO unavailable: " << std::strerror(rc) << "\n";
        }
    }

    // Logical CPU the calling thread is running on right now
    inline int currentCore() {
        return sched_getcpu();
    }

#endif
}
