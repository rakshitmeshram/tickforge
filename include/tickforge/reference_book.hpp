// ReferenceBook — a deliberately simple order book built from std::map and
// std::list. It implements exactly the same semantics as OrderBook and exists
// for two reasons:
//   1. As a test oracle: the differential fuzz test drives both books with the
//      same random message stream and requires identical trades and depth.
//   2. As the performance baseline the benchmark compares against.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "tickforge/types.hpp"

namespace tickforge {

class ReferenceBook {
public:
    using Config = BookConfig;

    explicit ReferenceBook(Config cfg = {}) : cfg_(cfg) {}

    template <class Sink = NullSink>
    ExecReport limit(OrderId id, Side side, Price price, Qty qty, Tif tif = Tif::GTC,
                     Sink&& on_trade = Sink{}, OrderOwner owner = {}) {
        if (qty <= 0 || qty > kMaxQty) return {Status::RejectedInvalidQty, 0, 0};
        if (price < cfg_.min_price || price > cfg_.max_price) return {Status::RejectedInvalidPrice, 0, 0};
        if (id == ~OrderId{0} || index_.count(id)) return {Status::RejectedDuplicateId, 0, 0};
        if (tif == Tif::FOK && available(side, price, owner) < qty)
            return {Status::RejectedFokUnfillable, 0, 0};

        bool stopped = false;
        const Qty filled = match(id, side, price, qty, on_trade, owner, stopped);
        const Qty remaining = qty - filled;
        if (stopped) return {Status::Ok, filled, 0, remaining};
        if (remaining == 0 || tif != Tif::GTC) return {Status::Ok, filled, 0};
        if (index_.size() >= cfg_.max_orders) return {Status::RejectedCapacity, filled, 0};

        auto& q = side == Side::Buy ? bids_[price] : asks_[price];
        q.push_back(Order{id, remaining, owner});
        index_[id] = Loc{side, price, std::prev(q.end())};
        return {Status::Ok, filled, remaining};
    }

    template <class Sink = NullSink>
    ExecReport market(OrderId id, Side side, Qty qty, Sink&& on_trade = Sink{}, OrderOwner owner = {}) {
        if (qty <= 0 || qty > kMaxQty) return {Status::RejectedInvalidQty, 0, 0};
        if (id == ~OrderId{0} || index_.count(id)) return {Status::RejectedDuplicateId, 0, 0};
        const Price bound = side == Side::Buy ? cfg_.max_price : cfg_.min_price;
        bool stopped = false;
        const Qty filled = match(id, side, bound, qty, on_trade, owner, stopped);
        return {Status::Ok, filled, 0, stopped ? qty - filled : 0};
    }

    ExecReport cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return {Status::RejectedUnknownOrder, 0, 0};
        erase(it);
        return {Status::Ok, 0, 0};
    }

    template <class Sink = NullSink>
    ExecReport amend(OrderId id, Price new_price, Qty new_qty, Sink&& on_trade = Sink{}) {
        auto it = index_.find(id);
        if (it == index_.end()) return {Status::RejectedUnknownOrder, 0, 0};
        if (new_qty <= 0 || new_qty > kMaxQty) return {Status::RejectedInvalidQty, 0, 0};
        if (new_price < cfg_.min_price || new_price > cfg_.max_price)
            return {Status::RejectedInvalidPrice, 0, 0};
        Loc& loc = it->second;
        if (loc.price == new_price && new_qty <= loc.it->qty) {
            loc.it->qty = new_qty;
            return {Status::Ok, 0, new_qty};
        }
        const Side side = loc.side;
        const OrderOwner owner = loc.it->owner;
        erase(it);
        return limit(id, side, new_price, new_qty, Tif::GTC, on_trade, owner);
    }

    [[nodiscard]] Price best_bid() const { return bids_.empty() ? kNoPrice : bids_.begin()->first; }
    [[nodiscard]] Price best_ask() const { return asks_.empty() ? kNoPrice : asks_.begin()->first; }

    [[nodiscard]] std::vector<LevelView> depth(Side side, std::size_t n) const {
        std::vector<LevelView> out;
        auto emit = [&](const auto& book) {
            for (const auto& [px, q] : book) {
                if (out.size() == n) break;
                Qty tot = 0;
                for (const auto& o : q) tot += o.qty;
                out.push_back(LevelView{px, tot, static_cast<std::uint32_t>(q.size())});
            }
        };
        if (side == Side::Buy)
            emit(bids_);
        else
            emit(asks_);
        return out;
    }

    [[nodiscard]] std::size_t order_count() const { return index_.size(); }

private:
    struct Order {
        OrderId id;
        Qty qty;
        OrderOwner owner;
    };
    using Queue = std::list<Order>;
    struct Loc {
        Side side;
        Price price;
        Queue::iterator it;
    };

    static bool self_match(const OrderOwner& taker, const Order& maker) {
        return taker.id != kNoOwner && maker.owner.id == taker.id;
    }

    // Executable quantity for a FOK check: walks the same orders matching
    // would, skipping own orders under CancelOldest and stopping at the first
    // own order otherwise.
    template <class Book>
    static Qty available_in(const Book& book, Side side, Price limit, const OrderOwner& owner) {
        Qty t = 0;
        for (const auto& [px, q] : book) {
            if (side == Side::Buy ? px > limit : px < limit) break;
            for (const auto& o : q) {
                if (self_match(owner, o)) {
                    if (owner.stp == Stp::CancelOldest) continue;
                    return t;
                }
                t += o.qty;
            }
        }
        return t;
    }

    Qty available(Side side, Price limit, const OrderOwner& owner) const {
        return side == Side::Buy ? available_in(asks_, side, limit, owner) : available_in(bids_, side, limit, owner);
    }

    template <class Book, class Sink>
    Qty sweep(Book& book, OrderId taker, Side side, Price limit, Qty qty, Sink& on_trade, const OrderOwner& owner,
              bool& stopped) {
        Qty filled = 0;
        while (qty > 0 && !book.empty() && !stopped) {
            auto lvl = book.begin();
            if (side == Side::Buy ? lvl->first > limit : lvl->first < limit) break;
            auto& q = lvl->second;
            while (qty > 0 && !q.empty()) {
                Order& m = q.front();
                if (self_match(owner, m)) {
                    if (owner.stp == Stp::CancelNewest) {
                        stopped = true;
                        break;
                    }
                    notify_stp(on_trade, StpCancel{m.id, m.qty});
                    index_.erase(m.id);
                    q.pop_front();
                    if (owner.stp == Stp::CancelBoth) {
                        stopped = true;
                        break;
                    }
                    continue;
                }
                const Qty f = std::min(qty, m.qty);
                on_trade(Trade{m.id, taker, lvl->first, f, side});
                qty -= f;
                filled += f;
                m.qty -= f;
                if (m.qty == 0) {
                    index_.erase(m.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(lvl);
        }
        return filled;
    }

    template <class Sink>
    Qty match(OrderId taker, Side side, Price limit, Qty qty, Sink& on_trade, const OrderOwner& owner,
              bool& stopped) {
        return side == Side::Buy ? sweep(asks_, taker, side, limit, qty, on_trade, owner, stopped)
                                 : sweep(bids_, taker, side, limit, qty, on_trade, owner, stopped);
    }

    void erase(std::unordered_map<OrderId, Loc>::iterator it) {
        const Loc loc = it->second;
        index_.erase(it);
        if (loc.side == Side::Buy) {
            auto lvl = bids_.find(loc.price);
            lvl->second.erase(loc.it);
            if (lvl->second.empty()) bids_.erase(lvl);
        } else {
            auto lvl = asks_.find(loc.price);
            lvl->second.erase(loc.it);
            if (lvl->second.empty()) asks_.erase(lvl);
        }
    }

    Config cfg_;
    std::map<Price, Queue, std::greater<>> bids_;
    std::map<Price, Queue> asks_;
    std::unordered_map<OrderId, Loc> index_;
};

}  // namespace tickforge
