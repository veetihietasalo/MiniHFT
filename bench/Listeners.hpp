#pragma once

// Top-of-book listeners for the W05 comparison. Both do the same work: count updates and fold
// each quote into a checksum, so the optimizer can't delete it. One is called through the
// builder's template parameter; the other through a virtual interface, chosen at run time so
// the compiler can't see which implementation it is.

#include <cstdint>

#include "ItchBook.hpp"

inline uint64_t foldQuote(uint64_t checksum, const TopOfBook& t) {
    return checksum * 31 + (uint64_t{t.bidPrice} << 32 | t.askPrice) + t.bidShares + t.askShares;
}

// Called directly: onTopOfBook can be inlined into the book builder.
struct QuoteCounter {
    uint64_t updates = 0;
    uint64_t checksum = 0;
    void onTopOfBook(const TopOfBook& t) {
        ++updates;
        checksum = foldQuote(checksum, t);
    }
};

// The same work behind a virtual interface.
struct TopOfBookHandler {
    virtual ~TopOfBookHandler() = default;
    virtual void onTopOfBook(const TopOfBook& t) = 0;
    uint64_t updates = 0;
    uint64_t checksum = 0;
};

struct CountingHandler final : TopOfBookHandler {
    void onTopOfBook(const TopOfBook& t) override {
        ++updates;
        checksum = foldQuote(checksum, t);
    }
};

// A second implementation, so that a TopOfBookHandler* could point at either and the call
// site can't be devirtualized.
struct IgnoringHandler final : TopOfBookHandler {
    void onTopOfBook(const TopOfBook&) override { ++updates; }
};

inline volatile bool g_useIgnoringHandler = false;

// Satisfies TopOfBookListener by forwarding through the virtual interface.
struct VirtualListener {
    TopOfBookHandler* target = nullptr;
    void onTopOfBook(const TopOfBook& t) { target->onTopOfBook(t); }
};
