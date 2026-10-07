#include <cstdint>
#include <iostream>
#include <thread>

#include "RingBuffer.hpp"
#include "ThreadUtils.hpp"
#include "Tsc.hpp"

namespace {

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
    if (!ThreadUtils::pinThread(1)) std::cerr << "producer not pinned: the latency below is less meaningful\n";

    for (int i = 0; i < MSG_COUNT; ++i) {
        Message* msg = nullptr;
        // Spin until slot is available
        while ((msg = ringBuffer.claim()) == nullptr) {
            // Tsc::cpuRelax(); // CPU hint to relax the loop (PAUSE on x86, ISB on AArch64)
        }

        msg->id = static_cast<uint64_t>(i);
        msg->timestamp = Tsc::read(); // RDTSC on x86, CNTVCT_EL0 on AArch64
        ringBuffer.publish();
    }
}

void consumer() {
    // Pin to Core 2
    if (!ThreadUtils::pinThread(2)) std::cerr << "consumer not pinned: the latency below is less meaningful\n";

    uint64_t totalLatency = 0;

    for (int i = 0; i < MSG_COUNT; ++i) {
        Message* msg = nullptr;
        // Spin until data is available
        while ((msg = ringBuffer.peek()) == nullptr) {}

        uint64_t now = Tsc::read();
        totalLatency += (now - msg->timestamp);

        ringBuffer.consume();
    }

    std::cout << "Average Latency: " << (totalLatency / MSG_COUNT) << " counter ticks\n";
}

} // namespace

int main() {
    std::cout << "Starting RingBuffer Benchmark...\n";

    std::thread t1(producer);
    std::thread t2(consumer);

    t1.join();
    t2.join();

    return 0;
}
