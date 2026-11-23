// Differential fuzzing: drive OrderBook and ReferenceBook with identical
// message streams and require identical observable behaviour — every
// ExecReport, every Trade (in order), top of book after every message and
// full depth at regular checkpoints.
//
// Two stream sources:
//   * adversarial: uniformly random ops over a narrow price band, all TIFs,
//     duplicate and unknown ids, invalid prices/qtys, capacity pressure;
//   * realistic: the FlowGenerator used by the benchmarks.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "test_framework.hpp"
#include "tickforge/flow_generator.hpp"
#include "tickforge/order_book.hpp"
#include "tickforge/reference_book.hpp"

using namespace tickforge;

namespace {

template <class T>
long long L(T v) {
    return static_cast<long long>(v);
}
template <class T>
unsigned long long U(T v) {
    return static_cast<unsigned long long>(v);
}

struct Recorder {
    std::vector<Trade>* out;
    std::vector<StpCancel>* cancels;
    void operator()(const Trade& t) const { out->push_back(t); }
    void operator()(const StpCancel& c) const { cancels->push_back(c); }
};

Msg random_msg(Xoshiro256& rng, OrderId& next_id, Price lo, Price hi) {
    Msg m{};
    const double u = rng.uniform();
    m.side = rng.uniform() < 0.5 ? Side::Buy : Side::Sell;
    m.tif = static_cast<Tif>(rng.below(10) < 7 ? 0 : 1 + rng.below(2));
    m.qty = static_cast<Qty>(1 + rng.below(20));
    m.price = lo + static_cast<Price>(rng.below(static_cast<std::uint64_t>(hi - lo + 1)));
    // Reference an existing-ish id for cancels/amends (often stale).
    const OrderId recent = next_id > 1 ? 1 + rng.below(next_id - 1) : 1;
    if (u < 0.45) {
        m.type = MsgType::Limit;
        m.id = rng.below(50) == 0 ? recent : next_id++;  // ~2% duplicate ids
    } else if (u < 0.75) {
        m.type = MsgType::Cancel;
        m.id = recent;
    } else if (u < 0.88) {
        m.type = MsgType::Amend;
        m.id = recent;
    } else {
        m.type = MsgType::Market;
        m.id = rng.below(50) == 0 ? recent : next_id++;  // ~2% reuse a possibly-live id
    }
    // Most orders belong to one of three owners so self-trade prevention
    // fires constantly; a quarter are anonymous.
    const std::uint64_t who = rng.below(4);
    m.owner = static_cast<OwnerId>(who);
    m.stp = static_cast<Stp>(rng.below(3));
    if (rng.below(200) == 0) m.qty = -static_cast<Qty>(rng.below(2));   // invalid qty
    if (rng.below(400) == 0) m.qty = kMaxQty + 1;                       // over the cap
    if (rng.below(200) == 0) m.price = rng.below(2) ? lo - 1 : hi + 1;  // invalid price
    if (rng.below(500) == 0) {                                          // malformed enum byte
        switch (rng.below(3)) {
            case 0:
                m.side = static_cast<Side>(2 + rng.below(254));
                break;
            case 1:  // bad time-in-force or bad STP mode
                if (rng.below(2))
                    m.tif = static_cast<Tif>(3 + rng.below(253));
                else
                    m.stp = static_cast<Stp>(3 + rng.below(253));
                break;
            default:
                m.type = static_cast<MsgType>(4 + rng.below(252));
                break;
        }
    }
    return m;
}

bool run_stream(const std::vector<Msg>& msgs, Price lo, Price hi, std::size_t cap) {
    OrderBook fast({lo, hi, cap});
    ReferenceBook ref({lo, hi, cap});
    std::vector<Trade> tf, tr;
    std::vector<StpCancel> cf, cr;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        tf.clear();
        tr.clear();
        cf.clear();
        cr.clear();
        const ExecReport a = apply(fast, msgs[i], Recorder{&tf, &cf});
        const ExecReport b = apply(ref, msgs[i], Recorder{&tr, &cr});
        bool ok = a.status == b.status && a.filled == b.filled && a.resting == b.resting &&
                  a.self_trade_cancelled == b.self_trade_cancelled && tf == tr && cf == cr &&
                  fast.best_bid() == ref.best_bid() && fast.best_ask() == ref.best_ask() &&
                  fast.order_count() == ref.order_count();
        if (ok && (i % 64 == 0 || i + 1 == msgs.size())) {
            ok = fast.depth(Side::Buy, 1000) == ref.depth(Side::Buy, 1000) &&
                 fast.depth(Side::Sell, 1000) == ref.depth(Side::Sell, 1000);
        }
        if (!ok) {
            const Msg& m = msgs[i];
            std::printf("    divergence at msg %zu: type=%d side=%d tif=%d id=%llu px=%lld qty=%lld\n", i,
                        static_cast<int>(m.type), static_cast<int>(m.side), static_cast<int>(m.tif), U(m.id),
                        L(m.price), L(m.qty));
            std::printf(
                "    fast: st=%s f=%lld r=%lld stp=%lld trades=%zu cx=%zu  "
                "ref: st=%s f=%lld r=%lld stp=%lld trades=%zu cx=%zu\n",
                to_string(a.status), L(a.filled), L(a.resting), L(a.self_trade_cancelled), tf.size(),
                cf.size(), to_string(b.status), L(b.filled), L(b.resting), L(b.self_trade_cancelled),
                tr.size(), cr.size());
            return false;
        }
    }
    return true;
}

std::size_t fuzz_iterations() {
    const char* env = std::getenv("TF_FUZZ_SEEDS");
    return env ? static_cast<std::size_t>(std::atoll(env)) : 40;
}

}  // namespace

TEST(differential_adversarial_streams) {
    const std::size_t seeds = fuzz_iterations();
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        Xoshiro256 rng(seed);
        OrderId next_id = 1;
        const Price lo = 100, hi = 100 + static_cast<Price>(5 + rng.below(60));
        const std::size_t cap = 50 + rng.below(400);  // small: exercises RejectedCapacity
        std::vector<Msg> msgs;
        for (int i = 0; i < 20000; ++i) msgs.push_back(random_msg(rng, next_id, lo, hi));
        if (!run_stream(msgs, lo, hi, cap)) {
            CHECK(false);
            std::printf("    seed=%llu\n", U(seed));
            return;
        }
    }
}

TEST(differential_realistic_flow) {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        FlowParams p;
        ReferenceBook driver({p.min_price, p.max_price, 1 << 20});
        FlowGenerator gen(seed, p);
        const auto msgs = gen.generate(driver, 200000);
        if (!run_stream(msgs, p.min_price, p.max_price, 1 << 20)) {
            CHECK(false);
            std::printf("    seed=%llu\n", U(seed));
            return;
        }
    }
}

// The generator must only cancel or amend orders that are really resting,
// and its live set must equal the book's contents (it is the only trader).
TEST(generator_tracks_live_orders) {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        FlowParams p;
        OrderBook book({p.min_price, p.max_price, 1 << 20});
        FlowGenerator gen(seed, p);
        std::size_t stale = 0, cancels = 0, amends = 0;
        for (int i = 0; i < 300000; ++i) {
            const Msg m = gen.next(book.best_bid(), book.best_ask());
            struct Fwd {
                FlowGenerator* g;
                void operator()(const Trade& t) const { g->on_trade(t); }
                void operator()(const StpCancel& c) const { g->on_stp_cancel(c); }
            };
            const ExecReport r = apply(book, m, Fwd{&gen});
            gen.on_report(m, r);
            if (m.type == MsgType::Cancel) ++cancels;
            if (m.type == MsgType::Amend) ++amends;
            if (r.status == Status::RejectedUnknownOrder) ++stale;
            if (i % 1000 == 0 && gen.live_count() != book.order_count()) {
                std::printf("    seed %llu msg %d: generator thinks %zu live, book has %zu\n", U(seed), i,
                            gen.live_count(), book.order_count());
                CHECK(false);
                return;
            }
        }
        CHECK_EQ(stale, 0u);
        CHECK(cancels > 50000 && amends > 5000);
        CHECK_EQ(gen.live_count(), book.order_count());
        // Fills remove orders as well as cancels, so depth settles below the
        // target (about 55% of it with default parameters), not at it.
        const double live = static_cast<double>(book.order_count());
        CHECK(live > 0.4 * p.target_live_orders && live < 0.75 * p.target_live_orders);
    }
}

// step() must produce exactly the stream next()+manual feedback produces.
TEST(generator_step_matches_manual_feedback) {
    FlowParams p;
    OrderBook a({p.min_price, p.max_price, 1 << 20}), b({p.min_price, p.max_price, 1 << 20});
    FlowGenerator ga(5, p), gb(5, p);
    const auto msgs = ga.generate(a, 50000);
    for (const auto& m : msgs) {
        const Msg n = gb.step(b);
        if (n.type != m.type || n.id != m.id || n.price != m.price || n.qty != m.qty) {
            CHECK(false);
            return;
        }
    }
    CHECK_EQ(a.order_count(), b.order_count());
}

TF_TEST_MAIN
