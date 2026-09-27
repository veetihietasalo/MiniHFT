#pragma once

// Applies ITCH 5.0 order events to an L3OrderBook per instrument. Use it as the handler for
// itch::dispatch().
//
// Orders come from an ObjectPool and are found by reference number through an OrderIndex, so
// execute, cancel, delete and replace never search a book. It also counts anything that would
// mean the feed or the book is wrong: events for unknown orders, executions larger than the
// order, crossed books during market hours.
//
// A strategy plugs in as the Listener template parameter and hears about every change to an
// instrument's best bid or ask. The call is resolved at compile time and can be inlined, and
// with the default NoListener the notification code isn't generated at all.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ItchMessages.hpp"
#include "L3OrderBook.hpp"
#include "ObjectPool.hpp"
#include "OrderIndex.hpp"

// Best bid and ask of one instrument, as reported to a TopOfBookListener. A price of 0 means
// that side of the book is empty.
struct TopOfBook {
    uint16_t locate = 0;
    uint64_t timestamp = 0; // ns since midnight, from the message that changed it
    uint32_t bidPrice = 0;
    uint64_t bidShares = 0;
    uint32_t askPrice = 0;
    uint64_t askShares = 0;
};

template <typename L>
concept TopOfBookListener = requires(L& listener, const TopOfBook& top) {
    { listener.onTopOfBook(top) } -> std::same_as<void>;
};

// The default listener: nobody is listening, so no notification code is generated.
struct NoListener {
    void onTopOfBook(const TopOfBook&) {}
};

// A book that crossed while its symbol was marked as trading, kept for inspection.
struct CrossedEvent {
    uint16_t locate;
    uint64_t timestamp;         // ns since midnight
    uint64_t lastTradingAction; // when the symbol's trading state last changed (0 = never)
};

struct ItchBookStats {
    uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0, trades = 0;
    uint64_t unknownRefs = 0;         // execute/cancel/delete/replace for an order we don't have
    uint64_t overExecutions = 0;      // executed or cancelled more shares than the order had left
    uint64_t crossedDuringMarket = 0; // an add or replace left the book crossed during market hours
    uint64_t crossedWhileTrading = 0; // ... and the symbol's trading state was 'T' (not halted or paused)
    uint64_t tradingActions = 0;
    uint64_t liveOrders = 0;
    uint64_t peakLiveOrders = 0;
    // How far from the top of its side each book change landed, in price levels:
    // [0]=top, [1]=second level, [2]=3rd-5th, [3]=6th-10th, [4]=11th-50th, [5]=51st+
    std::array<uint64_t, 6> depthHistogram{};
};

template <TopOfBookListener Listener = NoListener>
class BasicItchBookBuilder {
    static constexpr bool kNotify = !std::is_same_v<Listener, NoListener>;

public:
    static constexpr size_t kMaxLocates = 65536;
    using Stats = ItchBookStats;

    explicit BasicItchBookBuilder(size_t expectedLiveOrders = size_t{1} << 20, Listener listener = Listener{})
        : books_(std::make_unique<L3OrderBook[]>(kMaxLocates)),
          symbols_(kMaxLocates),
          lastPrice_(kMaxLocates, 0),
          tradingState_(kMaxLocates, 'T'),
          crossedByLocate_(kMaxLocates, 0),
          lastTradingAction_(kMaxLocates, 0),
          index_(expectedLiveOrders),
          listener_(std::move(listener)) {}

    // ---- itch::dispatch handlers ----
    void onAdd(uint16_t locate, uint64_t ts, const itch::AddOrder& m) {
        ++stats_.adds;
        changeBook(locate, ts, [&] {
            addOrder(locate, ts, m.ref, m.side == 'B' ? BookSide::Bid : BookSide::Ask, m.shares, m.price);
        });
    }

    void onExecuted(uint16_t, uint64_t ts, const itch::OrderExecuted& m) {
        ++stats_.executions;
        if (BookOrder* o = lookup(m.ref)) {
            lastPrice_[o->locate] = o->price;
            changeBook(o->locate, ts, [&] { take(o, m.shares); });
        }
    }

    void onExecutedWithPrice(uint16_t, uint64_t ts, const itch::OrderExecutedWithPrice& m) {
        ++stats_.executions;
        if (BookOrder* o = lookup(m.ref)) {
            if (m.printable == 'Y') lastPrice_[o->locate] = m.price;
            changeBook(o->locate, ts, [&] { take(o, m.shares); });
        }
    }

    void onCancel(uint16_t, uint64_t ts, const itch::OrderCancel& m) {
        ++stats_.cancels;
        if (BookOrder* o = lookup(m.ref)) changeBook(o->locate, ts, [&] { take(o, m.shares); });
    }

    void onDelete(uint16_t, uint64_t ts, const itch::OrderDelete& m) {
        ++stats_.deletes;
        if (BookOrder* o = lookup(m.ref)) changeBook(o->locate, ts, [&] { removeOrder(o); });
    }

    // The replacement is a new order: new reference number, new price and size, and it goes to
    // the back of the queue at its price. Side and instrument stay the same. The listener hears
    // about the result once, not about the moment in between.
    void onReplace(uint16_t, uint64_t ts, const itch::OrderReplace& m) {
        ++stats_.replaces;
        BookOrder* o = lookup(m.oldRef);
        if (!o) return;
        const uint16_t locate = o->locate;
        const BookSide side = o->side;
        changeBook(locate, ts, [&] {
            removeOrder(o);
            addOrder(locate, ts, m.newRef, side, m.shares, m.price);
        });
    }

    // Executions against non-displayed orders: they never were in the book.
    void onTrade(uint16_t locate, uint64_t, const itch::Trade& m) {
        ++stats_.trades;
        lastPrice_[locate] = m.price;
    }

    void onStockDirectory(uint16_t locate, uint64_t, const itch::StockDirectory& m) {
        std::string symbol(m.stock, 8);
        symbol.erase(symbol.find_last_not_of(' ') + 1);
        symbols_[locate] = symbol;
    }

    void onSystemEvent(uint16_t, uint64_t, const itch::SystemEvent& m) {
        if (m.code == 'Q') marketOpen_ = true;  // start of market hours
        if (m.code == 'M') marketOpen_ = false; // end of market hours
    }

    // Halts and pauses: orders keep arriving, nothing executes until the reopening auction,
    // so the book can legitimately cross.
    void onTradingAction(uint16_t locate, uint64_t ts, const itch::TradingAction& m) {
        ++stats_.tradingActions;
        tradingState_[locate] = m.state;
        lastTradingAction_[locate] = ts;
    }

    // ---- queries ----
    const L3OrderBook& book(uint16_t locate) const { return books_[locate]; }
    const std::string& symbol(uint16_t locate) const { return symbols_[locate]; }
    uint32_t lastPrice(uint16_t locate) const { return lastPrice_[locate]; } // last execution, 0 if none
    char tradingState(uint16_t locate) const { return tradingState_[locate]; }
    uint32_t crossedCount(uint16_t locate) const { return crossedByLocate_[locate]; } // during market hours
    const std::vector<CrossedEvent>& crossedWhileTrading() const { return crossedWhileTrading_; } // first 100
    Listener& listener() { return listener_; }

    // Locate code for a symbol from the stock directory, or -1 if not seen.
    int locateOf(std::string_view symbol) const {
        for (size_t i = 0; i < kMaxLocates; ++i) {
            if (symbols_[i] == symbol) return static_cast<int>(i);
        }
        return -1;
    }

    const Stats& stats() const { return stats_; }

private:
    // Applies `change` to the book for `locate`, then tells the listener if the best bid or ask
    // moved (price or size). With NoListener this is exactly `change()`.
    template <typename Change>
    void changeBook(uint16_t locate, uint64_t ts, Change&& change) {
        if constexpr (kNotify) {
            const TopOfBook before = topOf(locate, ts);
            change();
            const TopOfBook after = topOf(locate, ts);
            if (after.bidPrice != before.bidPrice || after.bidShares != before.bidShares ||
                after.askPrice != before.askPrice || after.askShares != before.askShares) {
                listener_.onTopOfBook(after);
            }
        } else {
            change();
        }
    }

    TopOfBook topOf(uint16_t locate, uint64_t ts) const {
        const L3OrderBook& b = books_[locate];
        TopOfBook top;
        top.locate = locate;
        top.timestamp = ts;
        if (const PriceLevel* bid = b.bestBid()) {
            top.bidPrice = bid->price;
            top.bidShares = bid->shares;
        }
        if (const PriceLevel* ask = b.bestAsk()) {
            top.askPrice = ask->price;
            top.askShares = ask->shares;
        }
        return top;
    }

    BookOrder* lookup(uint64_t ref) {
        BookOrder** found = index_.find(ref);
        if (!found) {
            ++stats_.unknownRefs;
            return nullptr;
        }
        return *found;
    }

    void addOrder(uint16_t locate, uint64_t ts, uint64_t ref, BookSide side, uint32_t shares, uint32_t price) {
        BookOrder* o = pool_.acquire();
        o->ref = ref;
        o->price = price;
        o->shares = shares;
        o->locate = locate;
        o->side = side;
        index_.insert(ref, o);
        recordDepth(books_[locate].add(o));
        if (++stats_.liveOrders > stats_.peakLiveOrders) stats_.peakLiveOrders = stats_.liveOrders;
        if (marketOpen_ && books_[locate].crossed()) {
            ++stats_.crossedDuringMarket;
            ++crossedByLocate_[locate];
            if (tradingState_[locate] == 'T') {
                ++stats_.crossedWhileTrading;
                if (crossedWhileTrading_.size() < 100) crossedWhileTrading_.push_back({locate, ts, lastTradingAction_[locate]});
            }
        }
    }

    // Execution or partial cancel of `shares`; the order leaves the book when nothing is left.
    void take(BookOrder* o, uint32_t shares) {
        if (shares > o->shares) {
            ++stats_.overExecutions;
            shares = o->shares;
        }
        if (shares == o->shares) removeOrder(o);
        else recordDepth(books_[o->locate].reduce(o, shares));
    }

    void removeOrder(BookOrder* o) {
        recordDepth(books_[o->locate].remove(o));
        index_.erase(o->ref);
        pool_.release(o);
        --stats_.liveOrders;
    }

    void recordDepth(size_t depth) {
        const size_t bucket = depth == 0 ? 0 : depth == 1 ? 1 : depth < 5 ? 2 : depth < 10 ? 3 : depth < 50 ? 4 : 5;
        ++stats_.depthHistogram[bucket];
    }

    std::unique_ptr<L3OrderBook[]> books_; // indexed by stock locate
    std::vector<std::string> symbols_;     // indexed by stock locate
    std::vector<uint32_t> lastPrice_;      // indexed by stock locate
    std::vector<char> tradingState_;       // indexed by stock locate
    std::vector<uint32_t> crossedByLocate_; // indexed by stock locate
    std::vector<uint64_t> lastTradingAction_; // indexed by stock locate
    std::vector<CrossedEvent> crossedWhileTrading_;
    ObjectPool<BookOrder, 65536> pool_;
    OrderIndex<BookOrder*> index_;
    Stats stats_;
    bool marketOpen_ = false;
    Listener listener_;
};

// The builder with no listener: what itch_replay and the tests use.
using ItchBookBuilder = BasicItchBookBuilder<>;
