#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ItchMessages.hpp"
#include "ItchParser.hpp"

namespace {

// Appends one encoded message with its 2-byte length prefix, as in NASDAQ's files.
template <typename Encode>
void append(std::vector<uint8_t>& out, Encode encode) {
    uint8_t message[64] = {};
    const size_t n = encode(message);
    uint8_t prefix[2];
    itch::putBe16(prefix, static_cast<uint16_t>(n));
    out.insert(out.end(), prefix, prefix + 2);
    out.insert(out.end(), message, message + n);
}

// Writes bytes to an anonymous temporary file, rewound for reading.
std::FILE* toFile(const std::vector<uint8_t>& bytes) {
    std::FILE* f = std::tmpfile();
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::rewind(f);
    return f;
}

struct Recorder {
    std::string log;
    void onAdd(uint16_t locate, uint64_t ts, const itch::AddOrder& m) {
        log += "A " + std::to_string(locate) + " " + std::to_string(ts) + " " + std::to_string(m.ref) + " " + m.side + " " +
               std::to_string(m.shares) + " " + std::string(m.stock, 8) + " " + std::to_string(m.price) + "\n";
    }
    void onExecuted(uint16_t, uint64_t, const itch::OrderExecuted& m) {
        log += "E " + std::to_string(m.ref) + " " + std::to_string(m.shares) + " " + std::to_string(m.match) + "\n";
    }
    void onExecutedWithPrice(uint16_t, uint64_t, const itch::OrderExecutedWithPrice& m) {
        log += "C " + std::to_string(m.ref) + " " + std::to_string(m.shares) + " " + std::to_string(m.match) + " " + m.printable +
               " " + std::to_string(m.price) + "\n";
    }
    void onCancel(uint16_t, uint64_t, const itch::OrderCancel& m) {
        log += "X " + std::to_string(m.ref) + " " + std::to_string(m.shares) + "\n";
    }
    void onDelete(uint16_t, uint64_t, const itch::OrderDelete& m) { log += "D " + std::to_string(m.ref) + "\n"; }
    void onReplace(uint16_t, uint64_t, const itch::OrderReplace& m) {
        log += "U " + std::to_string(m.oldRef) + " " + std::to_string(m.newRef) + " " + std::to_string(m.shares) + " " +
               std::to_string(m.price) + "\n";
    }
    void onTrade(uint16_t, uint64_t, const itch::Trade& m) {
        log += "P " + std::to_string(m.ref) + " " + m.side + " " + std::to_string(m.shares) + " " + std::string(m.stock, 8) + " " +
               std::to_string(m.price) + " " + std::to_string(m.match) + "\n";
    }
    void onStockDirectory(uint16_t locate, uint64_t, const itch::StockDirectory& m) {
        log += "R " + std::to_string(locate) + " " + std::string(m.stock, 8) + "\n";
    }
    void onSystemEvent(uint16_t, uint64_t, const itch::SystemEvent& m) { log += std::string("S ") + m.code + "\n"; }
    void onTradingAction(uint16_t locate, uint64_t, const itch::TradingAction& m) {
        log += "H " + std::to_string(locate) + " " + std::string(m.stock, 8) + " " + m.state + " " + std::string(m.reason, 4) + "\n";
    }
};

} // namespace

TEST(Itch, BigEndianHelpersRoundTrip) {
    uint8_t b[8];
    itch::putBe16(b, 0x1234);
    EXPECT_EQ(b[0], 0x12);
    EXPECT_EQ(itch::be16(b), 0x1234);
    itch::putBe48(b, 0x010203040506ULL); // 6-byte timestamp
    EXPECT_EQ(b[0], 0x01);
    EXPECT_EQ(b[5], 0x06);
    EXPECT_EQ(itch::be48(b), 0x010203040506ULL);
    itch::putBe64(b, 0x1122334455667788ULL);
    EXPECT_EQ(itch::be64(b), 0x1122334455667788ULL);
}

TEST(Itch, EveryOrderMessageRoundTripsThroughReaderAndDispatch) {
    const uint64_t ts = 34'200'000'000'123ULL; // 09:30:00.000000123
    std::vector<uint8_t> bytes;
    append(bytes, [&](uint8_t* p) { return itch::encodeSystemEvent(p, ts, 'Q'); });
    append(bytes, [&](uint8_t* p) { return itch::encodeStockDirectory(p, 7, ts, "AAPL    "); });
    append(bytes, [&](uint8_t* p) { return itch::encodeTradingAction(p, 7, ts, "AAPL    ", 'T', "    "); });
    append(bytes, [&](uint8_t* p) { return itch::encodeAddOrder(p, 7, ts, 1001, 'B', 300, "AAPL    ", 2915200); });
    append(bytes, [&](uint8_t* p) { return itch::encodeOrderExecuted(p, 7, ts, 1001, 100, 555); });
    append(bytes, [&](uint8_t* p) { return itch::encodeOrderExecutedWithPrice(p, 7, ts, 1001, 50, 556, 'Y', 2915100); });
    append(bytes, [&](uint8_t* p) { return itch::encodeOrderCancel(p, 7, ts, 1001, 25); });
    append(bytes, [&](uint8_t* p) { return itch::encodeOrderReplace(p, 7, ts, 1001, 1002, 400, 2915300); });
    append(bytes, [&](uint8_t* p) { return itch::encodeOrderDelete(p, 7, ts, 1002); });
    append(bytes, [&](uint8_t* p) { return itch::encodeTrade(p, 7, ts, 0, 'S', 10, "AAPL    ", 2915250, 557); });

    std::FILE* f = toFile(bytes);
    ItchReader reader(f, 16); // tiny buffer: forces refills in the middle of messages
    Recorder r;
    size_t length = 0;
    while (const uint8_t* m = reader.next(length)) {
        EXPECT_EQ(length, itch::messageLength(char(m[0])));
        EXPECT_EQ(itch::decodeHeader(m).timestamp, ts);
        itch::dispatch(m, r);
    }
    std::fclose(f);

    EXPECT_EQ(reader.messages(), 10u);
    EXPECT_EQ(reader.lengthMismatches(), 0u);
    EXPECT_EQ(reader.unknownTypes(), 0u);
    EXPECT_FALSE(reader.truncated());
    EXPECT_EQ(r.log,
              "S Q\n"
              "R 7 AAPL    \n"
              "H 7 AAPL     T     \n"
              "A 7 34200000000123 1001 B 300 AAPL     2915200\n"
              "E 1001 100 555\n"
              "C 1001 50 556 Y 2915100\n"
              "X 1001 25\n"
              "U 1001 1002 400 2915300\n"
              "D 1002\n"
              "P 0 S 10 AAPL     2915250 557\n");
}

TEST(Itch, ReaderFlagsWrongLengthsAndTruncatedInput) {
    std::vector<uint8_t> bytes;
    append(bytes, [](uint8_t* p) { return itch::encodeOrderDelete(p, 1, 0, 5) + 2; }); // 'D' framed as 21 bytes, not 19
    append(bytes, [](uint8_t* p) { return itch::encodeOrderDelete(p, 1, 0, 6); });
    bytes.resize(bytes.size() - 3); // cut the last message short

    std::FILE* f = toFile(bytes);
    ItchReader reader(f);
    size_t length = 0;
    while (reader.next(length)) {}
    std::fclose(f);

    EXPECT_EQ(reader.messages(), 1u);
    EXPECT_EQ(reader.lengthMismatches(), 1u);
    EXPECT_TRUE(reader.truncated());
}
