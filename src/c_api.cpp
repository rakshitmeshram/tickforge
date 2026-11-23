#include "tickforge/c_api.h"

#include <new>
#include <vector>

#include "tickforge/flow_generator.hpp"
#include "tickforge/order_book.hpp"

using namespace tickforge;

struct tf_book {
    OrderBook book;
    std::vector<tf_trade> trades;
    std::vector<tf_stp_cancel> stp_cancels;
};

struct tf_gen {
    FlowGenerator gen;
};

namespace {

struct Collector {
    tf_book* b;
    void operator()(const Trade& t) const {
        b->trades.push_back(
            tf_trade{t.maker_id, t.taker_id, t.price, t.qty, static_cast<std::uint8_t>(t.taker_side)});
    }
    void operator()(const StpCancel& c) const { b->stp_cancels.push_back(tf_stp_cancel{c.order_id, c.qty}); }
};

tf_report to_c(ExecReport r) noexcept {
    return {static_cast<std::int32_t>(r.status), r.filled, r.resting, r.self_trade_cancelled};
}

bool valid_side(std::uint8_t s) noexcept {
    return s <= static_cast<std::uint8_t>(Side::Sell);
}
bool valid_tif(std::uint8_t t) noexcept {
    return t <= static_cast<std::uint8_t>(Tif::FOK);
}
bool valid_stp(std::uint8_t s) noexcept {
    return s <= static_cast<std::uint8_t>(Stp::CancelBoth);
}
Side side_of(std::uint8_t s) noexcept {
    return static_cast<Side>(s);
}
Tif tif_of(std::uint8_t t) noexcept {
    return static_cast<Tif>(t);
}

constexpr tf_report kNoMemory{static_cast<std::int32_t>(Status::RejectedCapacity), 0, 0, 0};
constexpr tf_report kInvalid{static_cast<std::int32_t>(Status::RejectedInvalidMessage), 0, 0, 0};

}  // namespace

extern "C" {

tf_book* tf_book_new(int64_t min_price, int64_t max_price, uint64_t max_orders) {
    try {
        return new tf_book{OrderBook({min_price, max_price, static_cast<std::size_t>(max_orders)}), {}, {}};
    } catch (...) {
        return nullptr;
    }
}

void tf_book_free(tf_book* b) {
    delete b;
}

tf_report tf_book_limit(tf_book* b, uint64_t id, uint8_t side, int64_t price, int64_t qty, uint8_t tif) {
    if (!b || !valid_side(side) || !valid_tif(tif)) return kInvalid;
    try {
        return to_c(b->book.limit(id, side_of(side), price, qty, tif_of(tif), Collector{b}));
    } catch (const std::bad_alloc&) {
        return kNoMemory;
    }
}

tf_report tf_book_limit_ex(tf_book* b, uint64_t id, uint8_t side, int64_t price, int64_t qty, uint8_t tif,
                           uint32_t owner, uint8_t stp) {
    if (!b || !valid_side(side) || !valid_tif(tif) || !valid_stp(stp)) return kInvalid;
    try {
        return to_c(b->book.limit(id, side_of(side), price, qty, tif_of(tif), Collector{b},
                                  OrderOwner{owner, static_cast<Stp>(stp)}));
    } catch (const std::bad_alloc&) {
        return kNoMemory;
    }
}

tf_report tf_book_market_ex(tf_book* b, uint64_t id, uint8_t side, int64_t qty, uint32_t owner, uint8_t stp) {
    if (!b || !valid_side(side) || !valid_stp(stp)) return kInvalid;
    try {
        return to_c(
            b->book.market(id, side_of(side), qty, Collector{b}, OrderOwner{owner, static_cast<Stp>(stp)}));
    } catch (const std::bad_alloc&) {
        return kNoMemory;
    }
}

tf_report tf_book_market(tf_book* b, uint64_t id, uint8_t side, int64_t qty) {
    if (!b || !valid_side(side)) return kInvalid;
    try {
        return to_c(b->book.market(id, side_of(side), qty, Collector{b}));
    } catch (const std::bad_alloc&) {
        return kNoMemory;
    }
}

tf_report tf_book_cancel(tf_book* b, uint64_t id) {
    if (!b) return kInvalid;
    return to_c(b->book.cancel(id));
}

tf_report tf_book_amend(tf_book* b, uint64_t id, int64_t price, int64_t qty) {
    if (!b) return kInvalid;
    try {
        return to_c(b->book.amend(id, price, qty, Collector{b}));
    } catch (const std::bad_alloc&) {
        return kNoMemory;
    }
}

tf_report tf_book_apply(tf_book* b, const tf_msg* m) {
    if (!b || !m) return kInvalid;
    // Raw bytes are copied as-is; apply() rejects out-of-range enum values.
    const Msg msg{static_cast<MsgType>(m->type),
                  static_cast<Side>(m->side),
                  static_cast<Tif>(m->tif),
                  static_cast<Stp>(m->stp),
                  m->owner,
                  m->id,
                  m->price,
                  m->qty};
    try {
        return to_c(apply(b->book, msg, Collector{b}));
    } catch (const std::bad_alloc&) {
        return kNoMemory;
    }
}

int64_t tf_book_best_bid(const tf_book* b) {
    return b ? b->book.best_bid() : kNoPrice;
}
int64_t tf_book_best_ask(const tf_book* b) {
    return b ? b->book.best_ask() : kNoPrice;
}
uint64_t tf_book_order_count(const tf_book* b) {
    return b ? b->book.order_count() : 0;
}

uint64_t tf_book_depth(const tf_book* b, uint8_t side, uint64_t n, int64_t* prices, int64_t* qtys) {
    if (!b || !valid_side(side) || !prices || !qtys) return 0;
    LevelView buf[64];
    std::uint64_t written = 0;
    // n is capped at 64 levels per call.
    const std::size_t k = b->book.depth(side_of(side), n < 64 ? static_cast<std::size_t>(n) : 64, buf);
    for (std::size_t i = 0; i < k; ++i, ++written) {
        prices[i] = buf[i].price;
        qtys[i] = buf[i].qty;
    }
    return written;
}

uint64_t tf_book_trades(const tf_book* b, const tf_trade** out) {
    if (!out) return 0;
    if (!b) {
        *out = nullptr;
        return 0;
    }
    *out = b->trades.data();
    return b->trades.size();
}

void tf_book_clear_trades(tf_book* b) {
    if (b) b->trades.clear();
}

uint64_t tf_book_self_trade_cancels(const tf_book* b, const tf_stp_cancel** out) {
    if (!out) return 0;
    if (!b) {
        *out = nullptr;
        return 0;
    }
    *out = b->stp_cancels.data();
    return b->stp_cancels.size();
}

void tf_book_clear_self_trade_cancels(tf_book* b) {
    if (b) b->stp_cancels.clear();
}

tf_gen* tf_gen_new(uint64_t seed, double sigma, double p_market, double informed_k, uint64_t first_id) {
    try {
        FlowParams p;
        p.sigma = sigma;
        p.p_market = p_market;
        p.informed_k = informed_k;
        auto* g = new tf_gen{FlowGenerator(seed, p)};
        g->gen.set_first_id(first_id);
        return g;
    } catch (...) {
        return nullptr;
    }
}

void tf_gen_free(tf_gen* g) {
    delete g;
}

double tf_gen_fair_value(const tf_gen* g) {
    return g ? g->gen.fair_value() : 0.0;
}

uint64_t tf_gen_step(tf_gen* g, tf_book* b, uint64_t n) {
    if (!g || !b) return 0;
    try {
        // step() feeds fills and reports back so the generator only ever
        // cancels or amends orders that are really resting.
        for (uint64_t i = 0; i < n; ++i) g->gen.step(b->book, Collector{b});
        return n;
    } catch (const std::bad_alloc&) {
        return 0;
    }
}

const char* tf_status_string(int32_t status) {
    return to_string(static_cast<Status>(status));
}

}  // extern "C"
