#pragma once

#include <cstdint>

// NASDAQ ITCH 5.0 uses Big Endian (Network Byte Order).
// We need to swap bytes on x86 (Little Endian).

#pragma pack(push, 1) // Force 1-byte alignment (no padding between fields)

struct ItchHeader {
    char messageType;   // 'A' for Add Order, 'E' for Execute, etc.
    uint16_t stockLocate;
    uint16_t trackingNumber;
    uint64_t timestamp; // Nanoseconds since midnight
};

// "Add Order" Message (Type 'A')
// Total Length: 36 bytes
struct AddOrderMsg {
    ItchHeader header;
    uint64_t orderReferenceNumber;
    char buySellIndicator; // 'B' or 'S'
    uint32_t shares;
    char stock[8];
    uint32_t price; // 4-byte integer, implied 4 decimal places
};

#pragma pack(pop)

// Utility to swap bytes (Big Endian -> Little Endian)
inline uint32_t swap32(uint32_t val) {
    return (val << 24) | ((val << 8) & 0x00FF0000) |
           ((val >> 8) & 0x0000FF00) | (val >> 24);
}

inline uint64_t swap64(uint64_t val) {
    val = ((val << 8) & 0xFF00FF00FF00FF00ULL ) | ((val >> 8) & 0x00FF00FF00FF00FFULL );
    val = ((val << 16) & 0xFFFF0000FFFF0000ULL ) | ((val >> 16) & 0x0000FFFF0000FFFFULL );
    return (val << 32) | (val >> 32);
}
