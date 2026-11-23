// Scenario tests for matching semantics. Every scenario runs against BOTH the
// optimized OrderBook and the ReferenceBook, so the oracle used by the fuzz
// test is itself pinned to hand-checked behaviour.
#include <vector>

#include "test_framework.hpp"
#include "tickforge/order_book.hpp"
#include "tickforge/reference_book.hpp"

using namespace tickforge;

namespace {

struct Recorder {
    std::vector<Trade>* out;
    void operator()(const Trade& t) const { out->push_back(t); }
};

// Records trades and self-trade-prevention cancels.
struct StpRecorder {
    std::vector<Trade>* trades;
    std::vector<StpCancel>* cancels;
    void operator()(const Trade& t) const { trades->push_back(t); }
    void operator()(const StpCancel& c) const { cancels->push_back(c); }
};

constexpr OrderOwner kAlice{7, Stp::CancelNewest};
constexpr OrderOwner own(OwnerId id, Stp stp) {
    return OrderOwner{id, stp};
}

template <class Book>
Book make_book() {
    return Book(typename Book::Config{0, 1000, 1024});
}

template <class Book>
void price_time_priority() {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    Recorder rec{&tr};
    CHECK(b.limit(1, Side::Sell, 101, 5).ok());
    CHECK(b.limit(2, Side::Sell, 100, 5).ok());  // better price
    CHECK(b.limit(3, Side::Sell, 100, 5).ok());  // same price, later
    CHECK_EQ(b.best_ask(), 100);

    auto r = b.limit(10, Side::Buy, 101, 12, Tif::GTC, rec);
    CHECK(r.ok());
    CHECK_EQ(r.filled, 12);
    CHECK_EQ(r.resting, 0);
    REQUIRE(tr.size() == 3);
    CHECK(tr[0] == (Trade{2, 10, 100, 5, Side::Buy}));  // best price first
    CHECK(tr[1] == (Trade{3, 10, 100, 5, Side::Buy}));  // then FIFO
    CHECK(tr[2] == (Trade{1, 10, 101, 2, Side::Buy}));  // executes at maker price
    CHECK_EQ(b.best_ask(), 101);
    CHECK_EQ(b.order_count(), 1u);
}

template <class Book>
void partial_fill_rests_remainder() {
    auto b = make_book<Book>();
    CHECK(b.limit(1, Side::Sell, 100, 3).ok());
    auto r = b.limit(2, Side::Buy, 100, 10);
    CHECK_EQ(r.filled, 3);
    CHECK_EQ(r.resting, 7);
    CHECK_EQ(b.best_bid(), 100);
    CHECK_EQ(b.best_ask(), kNoPrice);
    auto d = b.depth(Side::Buy, 5);
    REQUIRE(d.size() == 1);
    CHECK(d[0] == (LevelView{100, 7, 1}));
}

template <class Book>
void non_crossing_limit_does_not_trade() {
    auto b = make_book<Book>();
    CHECK(b.limit(1, Side::Sell, 105, 3).ok());
    auto r = b.limit(2, Side::Buy, 104, 3);
    CHECK_EQ(r.filled, 0);
    CHECK_EQ(r.resting, 3);
    CHECK_EQ(b.best_bid(), 104);
    CHECK_EQ(b.best_ask(), 105);
}

template <class Book>
void ioc_cancels_remainder() {
    auto b = make_book<Book>();
    CHECK(b.limit(1, Side::Buy, 100, 4).ok());
    auto r = b.limit(2, Side::Sell, 99, 10, Tif::IOC);
    CHECK(r.ok());
    CHECK_EQ(r.filled, 4);
    CHECK_EQ(r.resting, 0);
    CHECK_EQ(b.order_count(), 0u);
    CHECK_EQ(b.best_ask(), kNoPrice);
}

template <class Book>
void fok_all_or_nothing() {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    Recorder rec{&tr};
    CHECK(b.limit(1, Side::Sell, 100, 4).ok());
    CHECK(b.limit(2, Side::Sell, 102, 4).ok());
    // Only 4 available at <= 101: must reject and leave the book untouched.
    auto r = b.limit(3, Side::Buy, 101, 5, Tif::FOK, rec);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(Status::RejectedFokUnfillable));
    CHECK(tr.empty());
    CHECK_EQ(b.order_count(), 2u);
    // 8 available at <= 102: fills completely.
    r = b.limit(4, Side::Buy, 102, 8, Tif::FOK, rec);
    CHECK(r.ok());
    CHECK_EQ(r.filled, 8);
    CHECK_EQ(tr.size(), 2u);
    CHECK_EQ(b.order_count(), 0u);
}

template <class Book>
void market_order_sweeps_levels() {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    Recorder rec{&tr};
    CHECK(b.limit(1, Side::Buy, 100, 2).ok());
    CHECK(b.limit(2, Side::Buy, 99, 2).ok());
    CHECK(b.limit(3, Side::Buy, 50, 2).ok());
    auto r = b.market(9, Side::Sell, 5, rec);
    CHECK_EQ(r.filled, 5);
    REQUIRE(tr.size() == 3);
    CHECK_EQ(tr[2].price, 50);
    CHECK_EQ(tr[2].qty, 1);
    CHECK_EQ(b.best_bid(), 50);
    // Market order larger than the book: fills what exists, never rests.
    r = b.market(10, Side::Sell, 100, rec);
    CHECK_EQ(r.filled, 1);
    CHECK_EQ(b.order_count(), 0u);
    CHECK_EQ(b.best_bid(), kNoPrice);
}

template <class Book>
void cancel_semantics() {
    auto b = make_book<Book>();
    CHECK(b.limit(1, Side::Buy, 100, 5).ok());
    CHECK(b.limit(2, Side::Buy, 100, 5).ok());
    CHECK(b.limit(3, Side::Buy, 98, 5).ok());
    CHECK(b.cancel(1).ok());
    CHECK_EQ(static_cast<int>(b.cancel(1).status), static_cast<int>(Status::RejectedUnknownOrder));
    CHECK_EQ(b.best_bid(), 100);
    CHECK(b.cancel(2).ok());
    CHECK_EQ(b.best_bid(), 98);  // best level emptied -> next level found
    CHECK(b.cancel(3).ok());
    CHECK_EQ(b.best_bid(), kNoPrice);
}

template <class Book>
void amend_priority_rules() {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    Recorder rec{&tr};
    CHECK(b.limit(1, Side::Sell, 100, 5).ok());
    CHECK(b.limit(2, Side::Sell, 100, 5).ok());
    // Reduce qty at same price: keeps the front of the queue.
    CHECK(b.amend(1, 100, 3).ok());
    b.market(9, Side::Buy, 1, rec);
    REQUIRE(tr.size() == 1);
    CHECK_EQ(tr[0].maker_id, 1u);
    // Increase qty: cancel/replace, goes to the back.
    tr.clear();
    CHECK(b.amend(1, 100, 10).ok());
    b.market(10, Side::Buy, 1, rec);
    REQUIRE(tr.size() == 1);
    CHECK_EQ(tr[0].maker_id, 2u);
    // Re-price through the market: trades immediately as a taker.
    tr.clear();
    CHECK(b.limit(3, Side::Buy, 95, 4).ok());
    auto r = b.amend(3, 100, 4, rec);
    CHECK_EQ(r.filled, 4);
    REQUIRE(!tr.empty());
    CHECK_EQ(tr[0].taker_id, 3u);
}

template <class Book>
void rejections() {
    auto b = make_book<Book>();
    CHECK_EQ(static_cast<int>(b.limit(1, Side::Buy, 100, 0).status),
             static_cast<int>(Status::RejectedInvalidQty));
    CHECK_EQ(static_cast<int>(b.limit(1, Side::Buy, -1, 5).status),
             static_cast<int>(Status::RejectedInvalidPrice));
    CHECK_EQ(static_cast<int>(b.limit(1, Side::Buy, 1001, 5).status),
             static_cast<int>(Status::RejectedInvalidPrice));
    CHECK(b.limit(1, Side::Buy, 100, 5).ok());
    CHECK_EQ(static_cast<int>(b.limit(1, Side::Sell, 200, 5).status),
             static_cast<int>(Status::RejectedDuplicateId));
    CHECK_EQ(static_cast<int>(b.amend(77, 100, 5).status), static_cast<int>(Status::RejectedUnknownOrder));
    CHECK_EQ(b.order_count(), 1u);
}

template <class Book>
void capacity_limit() {
    Book b(typename Book::Config{0, 100, 2});
    CHECK(b.limit(1, Side::Buy, 10, 1).ok());
    CHECK(b.limit(2, Side::Buy, 11, 1).ok());
    CHECK_EQ(static_cast<int>(b.limit(3, Side::Buy, 12, 1).status),
             static_cast<int>(Status::RejectedCapacity));
    CHECK(b.cancel(1).ok());
    CHECK(b.limit(3, Side::Buy, 12, 1).ok());  // slot reused
}

template <class Book>
void boundary_prices() {
    auto b = make_book<Book>();
    CHECK(b.limit(1, Side::Buy, 0, 1).ok());
    CHECK(b.limit(2, Side::Sell, 1000, 1).ok());
    CHECK_EQ(b.best_bid(), 0);
    CHECK_EQ(b.best_ask(), 1000);
    CHECK_EQ(b.market(3, Side::Buy, 1).filled, 1);
    CHECK_EQ(b.market(4, Side::Sell, 1).filled, 1);
    CHECK_EQ(b.order_count(), 0u);
}

// ------------------------------------------------- self-trade prevention

// Book for the STP scenarios: asks, best first,
//   id 1 @100 qty 3 owner 9 (someone else)
//   id 2 @100 qty 4 owner 7 (alice)
//   id 3 @101 qty 5 owner 9
template <class Book>
Book stp_book() {
    auto b = make_book<Book>();
    CHECK(b.limit(1, Side::Sell, 100, 3, Tif::GTC, NullSink{}, own(9, Stp::CancelNewest)).ok());
    CHECK(b.limit(2, Side::Sell, 100, 4, Tif::GTC, NullSink{}, own(7, Stp::CancelNewest)).ok());
    CHECK(b.limit(3, Side::Sell, 101, 5, Tif::GTC, NullSink{}, own(9, Stp::CancelNewest)).ok());
    return b;
}

template <class Book>
void stp_cancel_newest() {
    auto b = stp_book<Book>();
    std::vector<Trade> tr;
    std::vector<StpCancel> cx;
    // Alice buys 10 @101: fills 3 from id 1, then meets her own id 2 and stops.
    auto r = b.limit(10, Side::Buy, 101, 10, Tif::GTC, StpRecorder{&tr, &cx}, own(7, Stp::CancelNewest));
    CHECK(r.ok());
    CHECK_EQ(r.filled, 3);
    CHECK_EQ(r.resting, 0);  // GTC remainder does NOT rest
    CHECK_EQ(r.self_trade_cancelled, 7);
    REQUIRE(tr.size() == 1);
    CHECK_EQ(tr[0].maker_id, 1u);
    CHECK(cx.empty());           // resting order untouched
    CHECK_EQ(b.order_count(), 2u);  // ids 2 and 3
    CHECK_EQ(b.best_ask(), 100);
    CHECK_EQ(b.best_bid(), kNoPrice);
}

template <class Book>
void stp_cancel_oldest() {
    auto b = stp_book<Book>();
    std::vector<Trade> tr;
    std::vector<StpCancel> cx;
    // Cancels her resting id 2 and keeps matching into id 3.
    auto r = b.limit(10, Side::Buy, 101, 10, Tif::GTC, StpRecorder{&tr, &cx}, own(7, Stp::CancelOldest));
    CHECK_EQ(r.filled, 8);  // 3 from id 1 + 5 from id 3
    CHECK_EQ(r.resting, 2);
    CHECK_EQ(r.self_trade_cancelled, 0);
    REQUIRE(tr.size() == 2);
    CHECK_EQ(tr[0].maker_id, 1u);
    CHECK_EQ(tr[1].maker_id, 3u);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0] == (StpCancel{2, 4}));
    CHECK_EQ(b.order_count(), 1u);  // only the new bid
    CHECK_EQ(b.best_bid(), 101);
    CHECK_EQ(b.best_ask(), kNoPrice);
}

template <class Book>
void stp_cancel_both() {
    auto b = stp_book<Book>();
    std::vector<Trade> tr;
    std::vector<StpCancel> cx;
    auto r = b.limit(10, Side::Buy, 101, 10, Tif::GTC, StpRecorder{&tr, &cx}, own(7, Stp::CancelBoth));
    CHECK_EQ(r.filled, 3);
    CHECK_EQ(r.resting, 0);
    CHECK_EQ(r.self_trade_cancelled, 7);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0] == (StpCancel{2, 4}));
    CHECK_EQ(b.order_count(), 1u);  // only id 3
    CHECK_EQ(b.best_ask(), 101);
}

template <class Book>
void stp_only_same_owner() {
    auto b = stp_book<Book>();
    std::vector<Trade> tr;
    std::vector<StpCancel> cx;
    // Anonymous (owner 0) and another owner trade with everyone.
    auto r = b.market(10, Side::Buy, 4, StpRecorder{&tr, &cx});
    CHECK_EQ(r.filled, 4);
    r = b.market(11, Side::Buy, 4, StpRecorder{&tr, &cx}, own(3, Stp::CancelBoth));
    CHECK_EQ(r.filled, 4);
    CHECK_EQ(r.self_trade_cancelled, 0);
    CHECK(cx.empty());
    REQUIRE(tr.size() == 4);       // id1, id2 | id2, id3
    CHECK_EQ(tr[2].maker_id, 2u);  // alice's order traded with owner 3
}

template <class Book>
void stp_market_order() {
    auto b = stp_book<Book>();
    std::vector<Trade> tr;
    std::vector<StpCancel> cx;
    auto r = b.market(10, Side::Buy, 20, StpRecorder{&tr, &cx}, own(7, Stp::CancelNewest));
    CHECK_EQ(r.filled, 3);
    CHECK_EQ(r.self_trade_cancelled, 17);
    r = b.market(11, Side::Buy, 20, StpRecorder{&tr, &cx}, own(7, Stp::CancelOldest));
    CHECK_EQ(r.filled, 5);              // id 2 cancelled, id 3 consumed
    CHECK_EQ(r.self_trade_cancelled, 0);  // a market remainder is not an STP cancel
    CHECK_EQ(cx.size(), 1u);
    CHECK_EQ(b.order_count(), 0u);
}

template <class Book>
void stp_fok_counts_only_executable_liquidity() {
    // CancelNewest stops at alice's order: only 3 lots are reachable.
    auto b = stp_book<Book>();
    std::vector<Trade> tr;
    std::vector<StpCancel> cx;
    auto r = b.limit(10, Side::Buy, 101, 4, Tif::FOK, StpRecorder{&tr, &cx}, own(7, Stp::CancelNewest));
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(Status::RejectedFokUnfillable));
    CHECK(tr.empty());
    CHECK(cx.empty());
    CHECK_EQ(b.order_count(), 3u);  // book untouched
    // CancelOldest skips her order: 3 + 5 = 8 reachable.
    r = b.limit(11, Side::Buy, 101, 8, Tif::FOK, StpRecorder{&tr, &cx}, own(7, Stp::CancelOldest));
    CHECK(r.ok());
    CHECK_EQ(r.filled, 8);
    CHECK_EQ(cx.size(), 1u);
    r = b.limit(12, Side::Buy, 101, 1, Tif::FOK);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(Status::RejectedFokUnfillable));
}

template <class Book>
void stp_amend_keeps_owner() {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    std::vector<StpCancel> cx;
    CHECK(b.limit(1, Side::Sell, 100, 5, Tif::GTC, NullSink{}, kAlice).ok());
    CHECK(b.limit(2, Side::Buy, 95, 5, Tif::GTC, NullSink{}, kAlice).ok());
    // Re-pricing alice's bid through her own ask must not self-trade.
    auto r = b.amend(2, 100, 5, StpRecorder{&tr, &cx});
    CHECK(r.ok());
    CHECK_EQ(r.filled, 0);
    CHECK_EQ(r.self_trade_cancelled, 5);
    CHECK(tr.empty());
    CHECK_EQ(b.order_count(), 1u);  // bid gone (CancelNewest), ask stays
    CHECK_EQ(b.best_ask(), 100);
}

}  // namespace

#define BOTH(fn)             \
    TEST(fn##_fast) {        \
        fn<OrderBook>();     \
    }                        \
    TEST(fn##_reference) {   \
        fn<ReferenceBook>(); \
    }

BOTH(price_time_priority)
BOTH(partial_fill_rests_remainder)
BOTH(non_crossing_limit_does_not_trade)
BOTH(ioc_cancels_remainder)
BOTH(fok_all_or_nothing)
BOTH(market_order_sweeps_levels)
BOTH(cancel_semantics)
BOTH(amend_priority_rules)
BOTH(rejections)
BOTH(capacity_limit)
BOTH(boundary_prices)
BOTH(stp_cancel_newest)
BOTH(stp_cancel_oldest)
BOTH(stp_cancel_both)
BOTH(stp_only_same_owner)
BOTH(stp_market_order)
BOTH(stp_fok_counts_only_executable_liquidity)
BOTH(stp_amend_keeps_owner)

TEST(volume_at_and_find) {
    OrderBook b({0, 1000, 64});
    CHECK(b.limit(1, Side::Buy, 100, 5).ok());
    CHECK(b.limit(2, Side::Buy, 100, 7).ok());
    CHECK_EQ(b.volume_at(Side::Buy, 100), 12);
    CHECK_EQ(b.volume_at(Side::Sell, 100), 0);
    CHECK_EQ(b.volume_at(Side::Buy, 5000), 0);
    auto info = b.find(2);
    REQUIRE(info.has_value());
    CHECK_EQ(info->qty, 7);
    CHECK_EQ(info->price, 100);
    CHECK(!b.find(3).has_value());
}

TF_TEST_MAIN
