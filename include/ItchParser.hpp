#pragma once

// Streams ITCH 5.0 messages in the file format NASDAQ's sample files use: each message is
// preceded by its length as a 2-byte big-endian integer. Reads from any FILE*, so a whole
// trading day can be streamed without unpacking it to disk first:
//
//   gzip -dc 12302019.NASDAQ_ITCH50.gz | itch_replay -
//
// Messages are returned as spans into an internal buffer (no copy). A span stays valid until
// the next call to next().

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "ItchMessages.hpp"

class ItchReader {
public:
    explicit ItchReader(std::FILE* file, size_t bufferBytes = size_t{1} << 20)
        : file_(file), buffer_(bufferBytes) {}

    // The next well-formed message (without its length prefix), or an empty span at the end
    // of input. A message that is empty, has an unknown type, or isn't exactly as long as its
    // type requires is skipped and counted, never returned, so every span this returns is
    // safe to decode.
    [[nodiscard]] std::span<const uint8_t> next() {
        for (;;) {
            if (!ensure(2)) {
                truncated_ = end_ > begin_; // input ended inside a length prefix
                return {};
            }
            const size_t len = itch::be16(buffer_.data() + begin_);
            if (!ensure(2 + len)) {
                truncated_ = true; // input ended inside a message
                return {};
            }
            const uint8_t* message = buffer_.data() + begin_ + 2;
            begin_ += 2 + len;
            ++messages_;
            bytes_ += 2 + len;

            if (len == 0) {
                ++lengthMismatches_;
                continue;
            }
            const size_t expected = itch::messageLength(char(message[0]));
            if (expected == 0) {
                ++unknownTypes_;
                continue;
            }
            if (expected != len) {
                ++lengthMismatches_;
                continue;
            }
            return {message, len};
        }
    }

    [[nodiscard]] uint64_t messages() const { return messages_; } // all framed messages, skipped ones included
    [[nodiscard]] uint64_t bytes() const { return bytes_; }
    [[nodiscard]] uint64_t unknownTypes() const { return unknownTypes_; }         // skipped: type missing from itch::messageLength
    [[nodiscard]] uint64_t lengthMismatches() const { return lengthMismatches_; } // skipped: empty, or wrong length for its type
    [[nodiscard]] bool truncated() const { return truncated_; }                   // input ended part-way through a message

private:
    // Makes at least `need` bytes available from begin_, refilling from the file as needed.
    bool ensure(size_t need) {
        if (end_ - begin_ >= need) return true;
        if (need > buffer_.size()) buffer_.resize(need);
        if (begin_ > 0) {
            std::memmove(buffer_.data(), buffer_.data() + begin_, end_ - begin_);
            end_ -= begin_;
            begin_ = 0;
        }
        while (end_ < need && !eof_) {
            const size_t n = std::fread(buffer_.data() + end_, 1, buffer_.size() - end_, file_);
            if (n == 0) eof_ = true;
            end_ += n;
        }
        return end_ - begin_ >= need;
    }

    std::FILE* file_;
    std::vector<uint8_t> buffer_;
    size_t begin_ = 0;
    size_t end_ = 0;
    bool eof_ = false;
    bool truncated_ = false;
    uint64_t messages_ = 0;
    uint64_t bytes_ = 0;
    uint64_t unknownTypes_ = 0;
    uint64_t lengthMismatches_ = 0;
};
