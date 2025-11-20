#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include "FpgaPipeline.hpp"
#include "KernelBypass.hpp"

// Software approach: Function call
uint32_t softwareProcess(uint32_t data) {
    // Decode
    uint32_t decoded = data & 0xFFFF;
    // Execute
    if (decoded > 100) return decoded;
    return 0;
}

void benchmarkSoftware() {
    auto start = std::chrono::high_resolution_clock::now();
    uint32_t sum = 0;
    for (int i = 0; i < 1000000; ++i) {
        sum += softwareProcess(i);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Software Time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
              << " us\n";
}

void benchmarkFpga() {
    FpgaPipeline fpga;
    auto start = std::chrono::high_resolution_clock::now();

    // Simulate streaming data
    for (int i = 0; i < 1000000; ++i) {
        fpga.input(i);
        fpga.tick(); // Clock cycle 1
        fpga.tick(); // Clock cycle 2
        fpga.tick(); // Clock cycle 3

        Bus32 out;
        fpga.output(out);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "FPGA Sim Time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
              << " us (Simulated Cycles: " << fpga.getCycles() << ")\n";
}

int main() {
    std::cout << "--- Hardware Simulation Benchmark ---\n";
    benchmarkSoftware();
    benchmarkFpga();
    return 0;
}
