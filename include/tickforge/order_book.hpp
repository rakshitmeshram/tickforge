// tickforge::OrderBook — price-time priority limit order book + matching engine.
//
// Design (see docs/DESIGN.md for the full rationale):
//   * Price ladder: one Level slot per tick in [min_price, max_price], so a
//     price maps to its level with a subtraction — no tree, no hashing.
//   * Occupied levels are tracked in a HierBitset per side; the best bid/ask
//     are cached and, when a best level empties, the next one is found in
//     O(log64 N) word operations.
//   * Orders live in a pre-allocated pool and form an intrusive doubly linked
//     FIFO per level (32-bit indices, not pointers). Nothing is allocated on
//     the hot path after construction.
//   * OrderId -> pool index via an open-addressing IdMap: O(1) cancel/amend.
//   * Trades are delivered to a caller-supplied callable (template parameter),
//     so the per-fill callback is inlined rather than a virtual call.
//   * Self-trade prevention: each order carries an owner id and STP mode.
//     Because quantities are capped at 2^30, a node stores its quantity in
//     32 bits, which makes room for both without growing the node past 32
//     bytes (two nodes per cache line). The owner is only compared when the
//     incoming order has a non-zero owner.
//
// Not thread-safe by design: a matching engine is a single-writer state
// machine; parallelism belongs at the instrument-sharding level.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "tickforge/hier_bitset.hpp"
#include "tickforge/id_map.hpp"
#include "tickforge/types.hpp"

namespace tickforge {

class OrderBook {
public:
    using Config = BookConfig;

    struct OrderInfo {
        Side side;
        Price price;
        Qty qty;
        OwnerId owner;
    };

    explicit OrderBook(Config cfg = {})
        : cfg_(validated(cfg)),
          nlevels_(static_cast<std::size_t>(cfg_.max_price - cfg_.min_price + 1)),
          levels_{std::vector<Level>(nlevels_), std::vector<Level>(nlevels_)},
          occupied_{HierBitset(nlevels_), HierBitset(nlevels_)},
          nodes_(cfg_.max_orders),
          ids_(std::min(cfg_.expected_orders, cfg_.max_orders)) {
        for (std::uint32_t i = 0; i < nodes_.size(); ++i)
            nodes_[i].next = (i + 1 < nodes_.size()) ? i + 1 : kNil;
        free_head_ = nodes_.empty() ? kNil : 0;
    }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    // ---------------------------------------------------------------- orders

    template <class Sink = NullSink>
    ExecReport limit(OrderId id, Side side, Price price, Qty qty, Tif tif = Tif::GTC,
                     Sink&& on_trade = Sink{}, OrderOwner owner = {}) {
        if (qty <= 0 || qty > kMaxQty) return {Status::RejectedInvalidQty, 0, 0};
        if (price < cfg_.min_price || price > cfg_.max_price) return {Status::RejectedInvalidPrice, 0, 0};
        if (id_in_use(id)) return {Status::RejectedDuplicateId, 0, 0};

        const auto px = index_of(price);
        if (tif == Tif::FOK && available_through(side, px, qty, owner) < qty)
            return {Status::RejectedFokUnfillable, 0, 0};

        const MatchResult m = match(id, side, static_cast<std::int64_t>(px), qty, on_trade, owner);
        const Qty filled = m.filled;
        const Qty remaining = qty - filled;
        if (m.stp_stopped) return {Status::Ok, filled, 0, remaining};
        if (remaining == 0 || tif != Tif::GTC) return {Status::Ok, filled, 0};

        // A remainder exists only if every crossing maker was fully consumed,
        // and each consumed maker frees a pool slot. So a capacity rejection
        // implies nothing traded: no fills are ever hidden behind it.
        if (free_head_ == kNil) {
            assert(filled == 0);
            return {Status::RejectedCapacity, 0, 0};
        }
        rest(id, side, px, remaining, owner);
        return {Status::Ok, filled, remaining};
    }

    // Market order: sweeps the opposite side with no price limit; any
    // unfilled remainder is cancelled (never rests). The id must not belong
    // to a resting order, so every trade's taker id is unambiguous.
    template <class Sink = NullSink>
    ExecReport market(OrderId id, Side side, Qty qty, Sink&& on_trade = Sink{}, OrderOwner owner = {}) {
        if (qty <= 0 || qty > kMaxQty) return {Status::RejectedInvalidQty, 0, 0};
        if (id_in_use(id)) return {Status::RejectedDuplicateId, 0, 0};
        const std::int64_t bound = side == Side::Buy ? static_cast<std::int64_t>(nlevels_) - 1 : 0;
        const MatchResult m = match(id, side, bound, qty, on_trade, owner);
        return {Status::Ok, m.filled, 0, m.stp_stopped ? qty - m.filled : 0};
    }

    ExecReport cancel(OrderId id) {
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kMissing) return {Status::RejectedUnknownOrder, 0, 0};
        remove(n);
        return {Status::Ok, 0, 0};
    }

    // Amend semantics follow common exchange practice:
    //   same price and qty reduced  -> modified in place, keeps time priority
    //   anything else               -> cancel/replace: loses priority and may
    //                                  trade immediately if it now crosses.
    template <class Sink = NullSink>
    ExecReport amend(OrderId id, Price new_price, Qty new_qty, Sink&& on_trade = Sink{}) {
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kMissing) return {Status::RejectedUnknownOrder, 0, 0};
        if (new_qty <= 0 || new_qty > kMaxQty) return {Status::RejectedInvalidQty, 0, 0};
        if (new_price < cfg_.min_price || new_price > cfg_.max_price)
            return {Status::RejectedInvalidPrice, 0, 0};

        Node& node = nodes_[n];
        if (price_of(node.px) == new_price && new_qty <= node.qty) {
            levels_[side_index(node.side)][node.px].qty -= node.qty - new_qty;
            node.qty = static_cast<NodeQty>(new_qty);
            return {Status::Ok, 0, new_qty};
        }
        const Side side = node.side;
        const OrderOwner owner{node.owner, node.stp};
        remove(n);
        return limit(id, side, new_price, new_qty, Tif::GTC, on_trade, owner);
    }

    // ----------------------------------------------------------- market data

    [[nodiscard]] Price best_bid() const noexcept {
        return best_[0] == HierBitset::npos ? kNoPrice : price_of(best_[0]);
    }
    [[nodiscard]] Price best_ask() const noexcept {
        return best_[1] == HierBitset::npos ? kNoPrice : price_of(best_[1]);
    }

    [[nodiscard]] Qty volume_at(Side side, Price price) const noexcept {
        if (price < cfg_.min_price || price > cfg_.max_price) return 0;
        return levels_[side_index(side)][index_of(price)].qty;
    }

    // Writes up to `n` levels from the top of `side` into `out`, best first.
    std::size_t depth(Side side, std::size_t n, LevelView* out) const noexcept {
        const int s = side_index(side);
        std::size_t k = 0;
        std::int64_t i = best_[s];
        while (k < n && i != HierBitset::npos) {
            const Level& lv = levels_[s][static_cast<std::size_t>(i)];
            out[k++] = LevelView{price_of(i), lv.qty, lv.count};
            i = side == Side::Buy ? occupied_[s].prev_set(i - 1) : occupied_[s].next_set(i + 1);
        }
        return k;
    }

    [[nodiscard]] std::vector<LevelView> depth(Side side, std::size_t n) const {
        std::vector<LevelView> v(n);
        v.resize(depth(side, n, v.data()));
        return v;
    }

    [[nodiscard]] std::optional<OrderInfo> find(OrderId id) const noexcept {
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kMissing) return std::nullopt;
        const Node& node = nodes_[n];
        return OrderInfo{node.side, price_of(node.px), node.qty, node.owner};
    }

    [[nodiscard]] std::size_t order_count() const noexcept { return ids_.size(); }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

    // Per-order quantity fits in 32 bits because of kMaxQty; level totals
    // stay 64-bit.
    using NodeQty = std::int32_t;
    static_assert(kMaxQty <= std::numeric_limits<NodeQty>::max());

    struct Node {
        OrderId id = 0;
        NodeQty qty = 0;
        std::uint32_t px = 0;  // level index
        std::uint32_t prev = kNil;
        std::uint32_t next = kNil;
        OwnerId owner = kNoOwner;
        Side side = Side::Buy;
        Stp stp = Stp::CancelNewest;
    };
    static_assert(sizeof(Node) == 32, "two nodes per cache line");

    struct MatchResult {
        Qty filled;
        bool stp_stopped;  // matching stopped by CancelNewest/CancelBoth
    };

    struct Level {
        Qty qty = 0;
        std::uint32_t head = kNil;
        std::uint32_t tail = kNil;
        std::uint32_t count = 0;
    };

    static Config validated(Config c) {
        if (c.max_price < c.min_price) throw std::invalid_argument("max_price < min_price");
        if (c.max_price - c.min_price + 1 > (Price{1} << 28))
            throw std::invalid_argument("price range too large for a dense ladder");
        if (c.max_orders == 0 || c.max_orders >= kNil) throw std::invalid_argument("max_orders out of range");
        return c;
    }

    bool id_in_use(OrderId id) const noexcept {
        return id == IdMap::kEmpty || ids_.find(id) != IdMap::kMissing;
    }

    static constexpr int side_index(Side s) noexcept { return static_cast<int>(s); }
    std::size_t index_of(Price p) const noexcept { return static_cast<std::size_t>(p - cfg_.min_price); }
    Price price_of(std::int64_t idx) const noexcept { return cfg_.min_price + idx; }

    // Quantity an incoming order could actually execute at prices acceptable
    // to `side` within `limit_px`, stopping early once `needed` is reached
    // (for FOK). With an owner, it mirrors matching exactly: own orders are
    // skipped under CancelOldest and end the scan under the other modes.
    Qty available_through(Side side, std::size_t limit_px, Qty needed, OrderOwner owner) const noexcept {
        const int o = side_index(opposite(side));
        Qty total = 0;
        std::int64_t i = best_[o];
        const auto lim = static_cast<std::int64_t>(limit_px);
        while (i != HierBitset::npos && total < needed) {
            if (side == Side::Buy ? i > lim : i < lim) break;
            const Level& lv = levels_[o][static_cast<std::size_t>(i)];
            if (owner.id == kNoOwner) {
                total += lv.qty;
            } else {
                for (std::uint32_t n = lv.head; n != kNil && total < needed; n = nodes_[n].next) {
                    if (nodes_[n].owner == owner.id) {
                        if (owner.stp == Stp::CancelOldest) continue;
                        return total;
                    }
                    total += nodes_[n].qty;
                }
            }
            i = side == Side::Buy ? occupied_[o].next_set(i + 1) : occupied_[o].prev_set(i - 1);
        }
        return total;
    }

    template <class Sink>
    MatchResult match(OrderId taker, Side side, std::int64_t limit_px, Qty qty, Sink& on_trade,
                      OrderOwner owner) {
        const int o = side_index(opposite(side));
        auto& levels = levels_[o];
        Qty filled = 0;
        while (qty > 0) {
            const std::int64_t best = best_[o];
            if (best == HierBitset::npos) break;
            if (side == Side::Buy ? best > limit_px : best < limit_px) break;

            Level& lv = levels[static_cast<std::size_t>(best)];
            const Price px = price_of(best);
            while (qty > 0 && lv.head != kNil) {
                const std::uint32_t n = lv.head;
                Node& maker = nodes_[n];
                if (owner.id != kNoOwner && maker.owner == owner.id) [[unlikely]] {
                    if (owner.stp == Stp::CancelNewest) return {filled, true};
                    notify_stp(on_trade, StpCancel{maker.id, maker.qty});
                    remove(n);  // clears the level (and moves best_) if it empties
                    if (owner.stp == Stp::CancelBoth) return {filled, true};
                    continue;
                }
                const Qty fill = std::min(qty, Qty{maker.qty});
                on_trade(Trade{maker.id, taker, px, fill, side});
                qty -= fill;
                filled += fill;
                maker.qty = static_cast<NodeQty>(maker.qty - fill);
                lv.qty -= fill;
                if (maker.qty == 0) {
                    lv.head = maker.next;
                    if (lv.head != kNil)
                        nodes_[lv.head].prev = kNil;
                    else
                        lv.tail = kNil;
                    --lv.count;
                    ids_.erase(maker.id);
                    release(n);
                }
            }
            // Idempotent if an STP cancel already emptied and cleared it.
            if (lv.head == kNil) clear_level(o, static_cast<std::size_t>(best));
        }
        return {filled, false};
    }

    void rest(OrderId id, Side side, std::size_t px, Qty qty, OrderOwner owner) {
        const int s = side_index(side);
        const std::uint32_t n = free_head_;
        free_head_ = nodes_[n].next;

        Node& node = nodes_[n];
        node.id = id;
        node.qty = static_cast<NodeQty>(qty);
        node.px = static_cast<std::uint32_t>(px);
        node.side = side;
        node.next = kNil;
        node.owner = owner.id;
        node.stp = owner.stp;

        Level& lv = levels_[s][px];
        node.prev = lv.tail;
        if (lv.tail != kNil)
            nodes_[lv.tail].next = n;
        else
            lv.head = n;
        lv.tail = n;
        lv.qty += qty;
        if (lv.count++ == 0) {
            occupied_[s].set(px);
            const auto ipx = static_cast<std::int64_t>(px);
            if (best_[s] == HierBitset::npos || (side == Side::Buy ? ipx > best_[s] : ipx < best_[s]))
                best_[s] = ipx;
        }
        ids_.insert(id, n);
    }

    void remove(std::uint32_t n) {
        Node& node = nodes_[n];
        const int s = side_index(node.side);
        Level& lv = levels_[s][node.px];
        if (node.prev != kNil)
            nodes_[node.prev].next = node.next;
        else
            lv.head = node.next;
        if (node.next != kNil)
            nodes_[node.next].prev = node.prev;
        else
            lv.tail = node.prev;
        lv.qty -= node.qty;
        ids_.erase(node.id);
        if (--lv.count == 0) clear_level(s, node.px);
        release(n);
    }

    void clear_level(int s, std::size_t px) noexcept {
        occupied_[s].reset(px);
        const auto ipx = static_cast<std::int64_t>(px);
        if (best_[s] == ipx)
            best_[s] = s == 0 ? occupied_[s].prev_set(ipx - 1) : occupied_[s].next_set(ipx + 1);
    }

    void release(std::uint32_t n) noexcept {
        nodes_[n].next = free_head_;
        free_head_ = n;
    }

    Config cfg_;
    std::size_t nlevels_;
    std::vector<Level> levels_[2];
    HierBitset occupied_[2];
    std::int64_t best_[2] = {HierBitset::npos, HierBitset::npos};
    std::vector<Node> nodes_;
    std::uint32_t free_head_ = kNil;
    IdMap ids_;
};

}  // namespace tickforge
