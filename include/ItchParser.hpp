#pragma once

#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>
#include "ItchMessages.hpp"

class ItchParser {
private:
    std::vector<char> buffer;
    size_t offset = 0;

public:
    // Load entire file into memory (Memory Mapped File would be better for huge files)
    bool loadFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file) return false;

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        buffer.resize(size);
        if (!file.read(buffer.data(), size)) return false;

        offset = 0;
        return true;
    }

    // Parse next message (Zero-Copy)
    // Returns pointer to header, or nullptr if done
    const ItchHeader* next() {
        if (offset + sizeof(ItchHeader) > buffer.size()) return nullptr;

        // Zero-Copy: Point directly to buffer
        const ItchHeader* header = reinterpret_cast<const ItchHeader*>(&buffer[offset]);

        // Determine message length based on type (Simplified for demo)
        size_t msgLen = 0;
        switch (header->messageType) {
            case 'A': msgLen = sizeof(AddOrderMsg); break;
            // Add other cases...
            default:
                // Unknown message, skip 1 byte to try to resync (naive)
                // In reality, ITCH messages are framed or we know lengths of all types
                offset++;
                return next();
        }

        if (offset + msgLen > buffer.size()) return nullptr;

        offset += msgLen;
        return header;
    }

    // Helper to cast generic header to specific message
    static const AddOrderMsg* asAddOrder(const ItchHeader* header) {
        if (header->messageType != 'A') return nullptr;
        return reinterpret_cast<const AddOrderMsg*>(header);
    }
};
