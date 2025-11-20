#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include "RingBuffer.hpp"
#include "ThreadUtils.hpp"

// A simple message to pass around
struct Message {
    uint64_t id;
    uint64_t timestamp;
};

// 1 Million messages
constexpr int MSG_COUNT = 1'000'000;
RingBuffer<Message, 1024> ringBuffer;

void producer() {
    // Pin to Core 1
    ThreadUtils::pinThread(1);

    for (int i = 0; i < MSG_COUNT; ++i) {
        Message* msg = nullptr;
        // Spin until slot is available
        while ((msg = ringBuffer.claim()) == nullptr) {
            // _mm_pause(); // CPU hint to relax the loop (SSE2 intrinsic)
        }

        msg->id = i;
        msg->timestamp = __rdtsc(); // Read Time-Stamp Counter (CPU cycles)
        ringBuffer.publish();
    }
}

void consumer() {
    // Pin to Core 2
    ThreadUtils::pinThread(2);

    uint64_t totalLatency = 0;

    for (int i = 0; i < MSG_COUNT; ++i) {
        Message* msg = nullptr;
        // Spin until data is available
        while ((msg = ringBuffer.peek()) == nullptr) {}

        uint64_t now = __rdtsc();
        totalLatency += (now - msg->timestamp);

        ringBuffer.consume();
    }

    std::cout << "Average Latency: " << (totalLatency / MSG_COUNT) << " cycles\n";
}

int main() {
    std::cout << "Starting RingBuffer Benchmark...\n";

    std::thread t1(producer);
    std::thread t2(consumer);

    t1.join();
    t2.join();

    return 0;
}
