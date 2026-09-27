#pragma once

// Order book for one instrument, built from order-level (L3) events: add, execute, cancel,
// delete, replace. It doesn't match orders itself; the exchange's feed says what traded.
//
// Layout (docs/order_book.md):
// - Each side is a std::vector<PriceLevel>, sorted so the best price is always at the back.
//   Most activity happens at or near the top, so inserting or erasing a level usually shifts
//   only a few elements, and the best price is back().
// - Each level keeps its orders in time priority as an intrusive doubly linked list, so an order
//   can be unlinked in O(1) once found. Orders are found by reference number through an index
//   outside the book (ItchBookBuilder), never by searching the book.
// - Levels store no pointers into the vector and orders store no pointers to levels, so the
//   vector is free to move levels around when it inserts, erases or grows.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class BookSide : uint8_t { Bid, Ask };

struct BookOrder {
    uint64_t ref = 0;
    uint32_t price = 0;  // ITCH price: 4 implied decimals
    uint32_t shares = 0; // remaining
    uint16_t locate = 0;
    BookSide side = BookSide::Bid;
    BookOrder* prev = nullptr; // toward the front of the level's queue
    BookOrder* next = nullptr; // toward the back
};

struct PriceLevel {
    uint32_t price = 0;
    uint32_t orderCount = 0;
    uint64_t shares = 0;
    BookOrder* head = nullptr; // first in line
    BookOrder* tail = nullptr; // last in line
};

class L3OrderBook {
public:
    // Appends `order` to the back of its price level's queue, creating the level if needed.
    // Returns the level's distance from the top of its side (0 = best price).
    size_t add(BookOrder* order) {
        std::vector<PriceLevel>& levels = sideOf(order->side);
        const size_t i = lowerBound(levels, order->side, order->price);
        if (i == levels.size() || levels[i].price != order->price) {
            levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(i), PriceLevel{order->price, 0, 0, nullptr, nullptr});
        }
        PriceLevel& level = levels[i];
        order->prev = level.tail;
        order->next = nullptr;
        if (level.tail) level.tail->next = order;
        else level.head = order;
        level.tail = order;
        ++level.orderCount;
        level.shares += order->shares;
        return levels.size() - 1 - i;
    }

    // Takes `shares` (at most order->shares) off the order, which keeps its place in line.
    size_t reduce(BookOrder* order, uint32_t shares) {
        assert(shares <= order->shares);
        std::vector<PriceLevel>& levels = sideOf(order->side);
        const size_t i = find(levels, order);
        levels[i].shares -= shares;
        order->shares -= shares;
        return levels.size() - 1 - i;
    }

    // Unlinks `order` from its level, and drops the level if it is now empty.
    size_t remove(BookOrder* order) {
        std::vector<PriceLevel>& levels = sideOf(order->side);
        const size_t i = find(levels, order);
        const size_t depth = levels.size() - 1 - i;
        PriceLevel& level = levels[i];
        if (order->prev) order->prev->next = order->next;
        else level.head = order->next;
        if (order->next) order->next->prev = order->prev;
        else level.tail = order->prev;
        --level.orderCount;
        level.shares -= order->shares;
        if (level.orderCount == 0) levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(i));
        order->prev = order->next = nullptr;
        return depth;
    }

    [[nodiscard]] const PriceLevel* bestBid() const { return bids_.empty() ? nullptr : &bids_.back(); }
    [[nodiscard]] const PriceLevel* bestAsk() const { return asks_.empty() ? nullptr : &asks_.back(); }

    [[nodiscard]] bool crossed() const { return !bids_.empty() && !asks_.empty() && bids_.back().price >= asks_.back().price; }

    // All levels of one side, best price last.
    [[nodiscard]] const std::vector<PriceLevel>& levels(BookSide side) const { return side == BookSide::Bid ? bids_ : asks_; }

private:
    std::vector<PriceLevel>& sideOf(BookSide side) { return side == BookSide::Bid ? bids_ : asks_; }

    // Bids ascend and asks descend, so "better" always means "closer to the back".
    // Returns the index of the level at `price`, or where it would be inserted.
    static size_t lowerBound(const std::vector<PriceLevel>& levels, BookSide side, uint32_t price) {
        const auto it = side == BookSide::Bid
            ? std::lower_bound(levels.begin(), levels.end(), price, [](const PriceLevel& l, uint32_t p) { return l.price < p; })
            : std::lower_bound(levels.begin(), levels.end(), price, [](const PriceLevel& l, uint32_t p) { return l.price > p; });
        return static_cast<size_t>(it - levels.begin());
    }

    static size_t find(const std::vector<PriceLevel>& levels, const BookOrder* order) {
        const size_t i = lowerBound(levels, order->side, order->price);
        assert(i < levels.size() && levels[i].price == order->price);
        return i;
    }

    std::vector<PriceLevel> bids_; // ascending price: best (highest) bid at the back
    std::vector<PriceLevel> asks_; // descending price: best (lowest) ask at the back
};
