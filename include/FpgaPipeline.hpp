#pragma once

#include <vector>
#include <cstdint>
#include <iostream>

// Simulating hardware types
using Wire = bool;
using Bus8 = uint8_t;
using Bus32 = uint32_t;

class FpgaPipeline {
private:
    struct PipelineStage {
        Bus32 inputReg;
        Bus32 outputReg;
        bool valid = false;
    };

    PipelineStage stage1_fetch;
    PipelineStage stage2_decode;
    PipelineStage stage3_execute;

    uint64_t clockCycles = 0;

public:
    // Simulating a clock edge
    void tick() {
        clockCycles++;

        // Stage 3: Execute (e.g., Filter orders)
        if (stage2_decode.valid) {
            stage3_execute.inputReg = stage2_decode.outputReg;
            // Hardware Logic: Check if price > 100 (Simulated Comparator)
            bool priceFilter = (stage3_execute.inputReg > 100);
            stage3_execute.outputReg = priceFilter ? stage3_execute.inputReg : 0;
            stage3_execute.valid = true;
        }

        // Stage 2: Decode (e.g., Extract Price field)
        if (stage1_fetch.valid) {
            stage2_decode.inputReg = stage1_fetch.outputReg;
            // Hardware Logic: Masking bits (Simulated AND gate)
            // Assuming price is lower 16 bits for demo
            stage2_decode.outputReg = stage2_decode.inputReg & 0xFFFF;
            stage2_decode.valid = true;
        }

        // Stage 1: Fetch (Load data from bus)
        // (In real HW, this would latch data from an input pin)
    }

    void input(Bus32 data) {
        stage1_fetch.outputReg = data;
        stage1_fetch.valid = true;
    }

    bool output(Bus32& data) {
        if (stage3_execute.valid) {
            data = stage3_execute.outputReg;
            return true;
        }
        return false;
    }

    uint64_t getCycles() const { return clockCycles; }
};
