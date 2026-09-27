#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
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

struct FileCloser {
    void operator()(std::FILE* f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>; // closed even when an ASSERT returns early

// Writes bytes to an anonymous temporary file, positioned at the start for reading. Returns
// null, and fails the test, if the file can't be created or written.
File toFile(const std::vector<uint8_t>& bytes) {
    File f(std::tmpfile());
    const bool written = f && std::fwrite(bytes.data(), 1, bytes.size(), f.get()) == bytes.size() &&
                         std::fseek(f.get(), 0, SEEK_SET) == 0;
    if (!written) {
        ADD_FAILURE() << "could not write the test input to a temporary file";
        return nullptr;
    }
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

    const File f = toFile(bytes);
    ASSERT_NE(f, nullptr);
    ItchReader reader(f.get(), 16); // tiny buffer: forces refills in the middle of messages
    Recorder r;
    for (std::span<const uint8_t> m = reader.next(); !m.empty(); m = reader.next()) {
        EXPECT_EQ(m.size(), itch::messageLength(char(m[0])));
        EXPECT_EQ(itch::decodeHeader(m.data()).timestamp, ts);
        EXPECT_TRUE(itch::dispatch(m, r));
    }

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

TEST(Itch, ReaderFlagsInputEndingInsideALengthPrefix) {
    std::vector<uint8_t> bytes;
    append(bytes, [](uint8_t* p) { return itch::encodeOrderDelete(p, 1, 0, 5); });
    bytes.push_back(0x00); // one stray byte: half of a length prefix

    const File f = toFile(bytes);
    ASSERT_NE(f, nullptr);
    ItchReader reader(f.get());
    while (!reader.next().empty()) {}

    EXPECT_EQ(reader.messages(), 1u);
    EXPECT_TRUE(reader.truncated());
}

TEST(Itch, ReaderFlagsWrongLengthsAndTruncatedInput) {
    std::vector<uint8_t> bytes;
    append(bytes, [](uint8_t* p) { return itch::encodeOrderDelete(p, 1, 0, 5) + 2; }); // 'D' framed as 21 bytes, not 19
    append(bytes, [](uint8_t* p) { return itch::encodeOrderDelete(p, 1, 0, 6); });
    bytes.resize(bytes.size() - 3); // cut the last message short

    const File f = toFile(bytes);
    ASSERT_NE(f, nullptr);
    ItchReader reader(f.get());
    while (!reader.next().empty()) {}

    EXPECT_EQ(reader.messages(), 1u);
    EXPECT_EQ(reader.lengthMismatches(), 1u);
    EXPECT_TRUE(reader.truncated());
}

// Decoders read fixed offsets, so a message shorter than its type requires must never reach
// one. The reader skips bad frames and carries on with the next one: the length prefix still
// says where it starts.
TEST(Itch, ReaderSkipsMalformedMessagesAndKeepsGoing) {
    std::vector<uint8_t> bytes;
    append(bytes, [](uint8_t* p) { return itch::encodeOrderDelete(p, 1, 0, 5); });   // good
    append(bytes, [](uint8_t* p) { p[0] = 'A'; return size_t{5}; });                  // add order, 5 bytes instead of 36
    append(bytes, [](uint8_t*) { return size_t{0}; });                                // empty message
    append(bytes, [](uint8_t* p) { p[0] = 'Z'; return size_t{12}; });                 // unknown type
    append(bytes, [](uint8_t* p) { return itch::encodeOrderDelete(p, 1, 0, 6); });   // good

    const File f = toFile(bytes);
    ASSERT_NE(f, nullptr);
    ItchReader reader(f.get(), 16);
    std::vector<uint64_t> deleted;
    for (std::span<const uint8_t> m = reader.next(); !m.empty(); m = reader.next()) {
        ASSERT_EQ(m.size(), itch::messageLength(char(m[0])));
        deleted.push_back(itch::decodeOrderDelete(m.data()).ref);
    }

    EXPECT_EQ(deleted, (std::vector<uint64_t>{5, 6}));
    EXPECT_EQ(reader.messages(), 5u);
    EXPECT_EQ(reader.lengthMismatches(), 2u); // the short add and the empty message
    EXPECT_EQ(reader.unknownTypes(), 1u);
    EXPECT_FALSE(reader.truncated());
}

TEST(Itch, DispatchRefusesMessagesOfTheWrongLength) {
    Recorder r;
    uint8_t message[64] = {};
    const size_t n = itch::encodeAddOrder(message, 7, 0, 1001, 'B', 300, "AAPL    ", 2915200);

    EXPECT_FALSE(itch::dispatch(std::span<const uint8_t>(message, n - 1), r)); // one byte short
    EXPECT_FALSE(itch::dispatch(std::span<const uint8_t>(message, n + 1), r)); // one byte long
    EXPECT_FALSE(itch::dispatch(std::span<const uint8_t>(), r));               // empty
    EXPECT_EQ(r.log, "");                                                      // the handler never saw them

    EXPECT_TRUE(itch::dispatch(std::span<const uint8_t>(message, n), r));
    EXPECT_EQ(r.log, "A 7 0 1001 B 300 AAPL     2915200\n");
}
