#pragma once

#include <windows.h>
#include <thread>
#include <iostream>
#include <vector>

namespace ThreadUtils {

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
}
