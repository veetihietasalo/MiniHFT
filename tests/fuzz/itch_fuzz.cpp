// libFuzzer target: arbitrary bytes through ItchReader and itch::dispatch into a book builder.
//
// Build with the clang-fuzz preset (-fsanitize=fuzzer,address,undefined), seed a corpus with
// the saved inputs and gen_itch's sample feed, then run:
//   mkdir corpus && cp tests/fuzz/corpus/* corpus/ && (cd corpus && ../build/clang-fuzz/gen_itch)
//   ./build/clang-fuzz/itch_fuzz -max_total_time=60 corpus/
// A crash means some input made the reader or a decoder read outside the message, or broke
// the book. Once it's fixed, add the input to tests/fuzz/corpus/ so every run replays it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdio.h> // fmemopen: POSIX, so <cstdio> doesn't promise it

#include "ItchBook.hpp"
#include "ItchMessages.hpp"
#include "ItchParser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // One builder for the whole run: allocating 65,536 books for every input would be too slow.
    static auto builder = std::make_unique<ItchBookBuilder>(1024);
    if (size == 0) return 0;

    std::FILE* file = fmemopen(const_cast<uint8_t*>(data), size, "rb");
    if (!file) return 0;
    ItchReader reader(file, 16); // tiny buffer: an overrun leaves the allocation, where ASan sees it
    for (std::span<const uint8_t> m = reader.next(); !m.empty(); m = reader.next()) {
        if (m.size() != itch::messageLength(char(m[0]))) __builtin_trap(); // the reader must never return this
        (void)itch::dispatch(m, *builder);
    }
    std::fclose(file);

    // dispatch() must also refuse raw bytes on its own, whatever they are.
    (void)itch::dispatch(std::span<const uint8_t>(data, size), *builder);
    return 0;
}
