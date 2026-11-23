// Synthetic order flow with a latent fair value.
//
// Model (all parameters in FlowParams):
//   * A hidden fair value F follows a Gaussian random walk (sigma ticks/event).
//   * Liquidity providers post limit orders at F -/+ a geometric distance,
//     with a small probability of a marketable (crossing) limit.
//   * Cancels hit a uniformly random live order. The cancel probability is
//     n / (n + target_live_orders) for n live orders, so book depth is
//     stationary. Fills remove orders too, so the depth settles below
//     `target_live_orders` (about 55% of it with the default parameters).
//   * The generator tracks which of its orders are really resting: it is told
//     every fill against its orders and the result of every message it sends
//     (see step()). Filled, never-rested and rejected orders are dropped, so
//     cancels and amends always reference live orders and the live count the
//     cancel rate depends on is the book's true depth.
//   * Market orders are partly informed: P(buy) = logistic(k * (F - mid)), so
//     takers lean toward the fair value. This creates real adverse selection
//     for a market maker and makes book imbalance mildly predictive — both
//     properties the research scripts then measure.
//
// Uses its own RNG transforms (not std:: distributions, whose output is
// implementation-defined) so a seed reproduces the identical stream on every
// compiler and platform.
#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "tickforge/message.hpp"
#include "tickforge/types.hpp"

namespace tickforge {

struct FlowParams {
    double start_price = 10'000.0;  // ticks
    double sigma = 0.08;            // fair-value step stdev, ticks per event
    double p_market = 0.06;
    double p_amend = 0.03;
    double target_live_orders = 4000;
    double depth_decay = 0.25;   // geometric parameter for distance from F
    double p_aggressive = 0.03;  // chance a limit order crosses
    double qty_log_mean = 2.3;   // lognormal size (median ~10 lots)
    double qty_log_sd = 0.7;
    double informed_k = 1.5;  // logistic slope of taker direction
    Price min_price = 1;
    Price max_price = 20'000 - 1;
};

class Xoshiro256 {  // xoshiro256**, public domain reference algorithm
public:
    explicit Xoshiro256(std::uint64_t seed) noexcept {
        for (auto& w : s_) {  // seed via splitmix64
            seed += 0x9e3779b97f4a7c15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            w = z ^ (z >> 31);
        }
    }
    std::uint64_t next() noexcept {
        const std::uint64_t r = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return r;
    }
    double uniform() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
    std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }
    double normal() noexcept {  // Box-Muller
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }
        double u = 0.0;
        while (u <= 1e-300) u = uniform();
        const double v = uniform();
        const double r = std::sqrt(-2.0 * std::log(u));
        spare_ = r * std::sin(6.283185307179586 * v);
        has_spare_ = true;
        return r * std::cos(6.283185307179586 * v);
    }
    std::int64_t geometric(double p) noexcept {  // failures before first success
        double u = 0.0;
        while (u <= 1e-300) u = uniform();
        return static_cast<std::int64_t>(std::floor(std::log(u) / std::log1p(-p)));
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4];
    double spare_ = 0.0;
    bool has_spare_ = false;
};

class FlowGenerator {
public:
    explicit FlowGenerator(std::uint64_t seed, FlowParams p = {}) : p_(p), rng_(seed), fair_(p.start_price) {}

    // Produces the next message given the current top of book (kNoPrice if a
    // side is empty). Order ids are allocated from `first_id` upward.
    Msg next(Price best_bid, Price best_ask) {
        fair_ += p_.sigma * rng_.normal();
        const double u = rng_.uniform();

        if (u < p_.p_market) return make_market(best_bid, best_ask);
        if (u < p_.p_market + p_.p_amend && !live_.empty()) return make_amend();

        const double n = static_cast<double>(live_.size());
        const double p_cancel = n / (n + p_.target_live_orders);
        if (!live_.empty() && rng_.uniform() < p_cancel) return make_cancel();
        return make_limit();
    }

    [[nodiscard]] double fair_value() const noexcept { return fair_; }
    void set_first_id(OrderId id) noexcept { next_id_ = id; }
    // Number of the generator's orders it believes are resting.
    [[nodiscard]] std::size_t live_count() const noexcept { return live_.size(); }

    // ----- feedback: callers that apply next()'s messages themselves must
    // forward every trade (and STP cancel) and every report, or the
    // generator's view of its live orders drifts. step() does this for you.
    void on_trade(const Trade& t) {
        auto it = pos_.find(t.maker_id);
        if (it == pos_.end()) return;  // not ours (e.g. a strategy's quote)
        Live& o = live_[it->second];
        o.qty -= t.qty;
        if (o.qty <= 0) forget(t.maker_id);
    }
    void on_stp_cancel(const StpCancel& c) { forget(c.order_id); }

    void on_report(const Msg& m, const ExecReport& r) {
        switch (m.type) {
            case MsgType::Limit:
                if (r.ok() && r.resting > 0) remember(m.id, m.side, r.resting);
                break;
            case MsgType::Amend: {
                auto it = pos_.find(m.id);
                if (it == pos_.end()) break;
                if (r.ok() && r.resting > 0)
                    live_[it->second].qty = r.resting;
                else
                    forget(m.id);  // fully filled on re-entry, or rejected
                break;
            }
            case MsgType::Cancel:  // already dropped when generated
            case MsgType::Market:  // never rests
                break;
        }
    }

    // Generates the next message, applies it to `book`, and feeds the trades
    // and report back. Trades and STP cancels are also passed to `sink`.
    template <class Book, class Sink = NullSink>
    Msg step(Book& book, Sink&& sink = Sink{}) {
        const Msg m = next(book.best_bid(), book.best_ask());
        Tee<std::remove_reference_t<Sink>> tee{this, &sink};
        on_report(m, apply(book, m, tee));
        return m;
    }

    // Convenience: generate a whole stream by driving a book of type Book.
    template <class Book>
    std::vector<Msg> generate(Book& book, std::size_t n) {
        std::vector<Msg> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(step(book));
        return out;
    }

private:
    struct Live {
        OrderId id;
        Side side;
        Qty qty;  // remaining quantity resting on the book
    };

    template <class Sink>
    struct Tee {
        FlowGenerator* gen;
        Sink* user;
        void operator()(const Trade& t) const {
            gen->on_trade(t);
            (*user)(t);
        }
        void operator()(const StpCancel& c) const {
            gen->on_stp_cancel(c);
            notify_stp(*user, c);
        }
    };

    void remember(OrderId id, Side side, Qty qty) {
        pos_[id] = live_.size();
        live_.push_back({id, side, qty});
    }

    void forget(OrderId id) {
        auto it = pos_.find(id);
        if (it == pos_.end()) return;
        const std::size_t i = it->second;
        pos_.erase(it);
        if (i + 1 != live_.size()) {
            live_[i] = live_.back();
            pos_[live_[i].id] = i;
        }
        live_.pop_back();
    }

    Price clamp(Price px) const noexcept {
        return px < p_.min_price ? p_.min_price : (px > p_.max_price ? p_.max_price : px);
    }

    Qty draw_qty() noexcept {
        const double q = std::exp(p_.qty_log_mean + p_.qty_log_sd * rng_.normal());
        return q < 1.0 ? 1 : static_cast<Qty>(std::llround(q));
    }

    Price quote_price(Side side) noexcept {
        std::int64_t d = rng_.geometric(p_.depth_decay);
        if (rng_.uniform() < p_.p_aggressive) d = -1 - rng_.geometric(0.6);
        // Buys rest at or below fair value, sells at or above.
        return side == Side::Buy ? clamp(static_cast<Price>(std::floor(fair_)) - d)
                                 : clamp(static_cast<Price>(std::ceil(fair_)) + d);
    }

    // The order is added to live_ only once its report shows it resting.
    Msg make_limit() noexcept {
        const Side side = rng_.uniform() < 0.5 ? Side::Buy : Side::Sell;
        const OrderId id = next_id_++;
        // Price and quantity both draw random numbers. Draw them in separate
        // statements: as two arguments of one call, their evaluation order is
        // unspecified (Clang goes left to right, GCC and MSVC right to left),
        // and the same seed gave different streams per compiler.
        const Price px = quote_price(side);
        const Qty qty = draw_qty();
        return make_msg(MsgType::Limit, side, Tif::GTC, id, px, qty);
    }

    Msg make_cancel() {
        const OrderId id = live_[rng_.below(live_.size())].id;
        forget(id);
        return make_msg(MsgType::Cancel, Side::Buy, Tif::GTC, id, 0, 0);
    }

    Msg make_amend() noexcept {
        const Live& o = live_[rng_.below(live_.size())];
        const Price px = quote_price(o.side);  // separate statements: see make_limit
        const Qty qty = draw_qty();
        return make_msg(MsgType::Amend, o.side, Tif::GTC, o.id, px, qty);
    }

    Msg make_market(Price bid, Price ask) noexcept {
        double p_buy = 0.5;
        if (bid != kNoPrice && ask != kNoPrice) {
            const double mid = 0.5 * static_cast<double>(bid + ask);
            p_buy = 1.0 / (1.0 + std::exp(-p_.informed_k * (fair_ - mid)));
        }
        const Side side = rng_.uniform() < p_buy ? Side::Buy : Side::Sell;
        return make_msg(MsgType::Market, side, Tif::GTC, next_id_++, 0, draw_qty());
    }

    FlowParams p_;
    Xoshiro256 rng_;
    double fair_;
    OrderId next_id_ = 1;
    std::vector<Live> live_;
    std::unordered_map<OrderId, std::size_t> pos_;  // id -> index in live_
};

}  // namespace tickforge
