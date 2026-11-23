// tickforge — core vocabulary types.
//
// Prices are integer ticks (never floating point): this removes rounding
// ambiguity at price levels and lets the book index levels directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace tickforge {

using OrderId = std::uint64_t;
using Price = std::int64_t;     // in ticks
using Qty = std::int64_t;       // in lots, always > 0 for live orders
using OwnerId = std::uint32_t;  // participant / account; 0 = anonymous

inline constexpr Price kNoPrice = std::numeric_limits<Price>::min();

// Largest quantity accepted for a single order. With fewer than 2^32 live
// orders, any sum of resting quantities stays below 2^62, so per-level and
// FOK liquidity totals can never overflow.
inline constexpr Qty kMaxQty = Qty{1} << 30;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Time in force.
//   GTC: rest any unfilled remainder on the book.
//   IOC: fill what is immediately available, cancel the remainder.
//   FOK: fill the entire quantity immediately or do nothing at all.
enum class Tif : std::uint8_t { GTC = 0, IOC = 1, FOK = 2 };

// Self-trade prevention (STP). When an incoming order would match a resting
// order with the same non-zero owner, no trade happens; instead:
//   CancelNewest: the incoming order's remaining quantity is cancelled
//                 (it does not rest, even if GTC). The resting order stays.
//   CancelOldest: the resting order is cancelled and matching continues.
//   CancelBoth:   the resting order is cancelled and the incoming order's
//                 remaining quantity is cancelled.
// Fills against other owners that happen before the self-match stand. The
// mode is taken from the incoming order; an amend that re-enters the book
// reuses the mode the order was originally sent with.
enum class Stp : std::uint8_t { CancelNewest = 0, CancelOldest = 1, CancelBoth = 2 };

inline constexpr OwnerId kNoOwner = 0;  // orders with owner 0 never self-match

struct OrderOwner {
    OwnerId id = kNoOwner;
    Stp stp = Stp::CancelNewest;
};

enum class Status : std::uint8_t {
    Ok = 0,
    RejectedDuplicateId,
    RejectedInvalidPrice,
    RejectedInvalidQty,
    RejectedUnknownOrder,
    RejectedFokUnfillable,
    RejectedCapacity,
    // A message whose type, side or time-in-force field is out of range.
    RejectedInvalidMessage,
};

[[nodiscard]] constexpr const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok:
            return "Ok";
        case Status::RejectedDuplicateId:
            return "RejectedDuplicateId";
        case Status::RejectedInvalidPrice:
            return "RejectedInvalidPrice";
        case Status::RejectedInvalidQty:
            return "RejectedInvalidQty";
        case Status::RejectedUnknownOrder:
            return "RejectedUnknownOrder";
        case Status::RejectedFokUnfillable:
            return "RejectedFokUnfillable";
        case Status::RejectedCapacity:
            return "RejectedCapacity";
        case Status::RejectedInvalidMessage:
            return "RejectedInvalidMessage";
    }
    return "Unknown";
}

// One fill between a resting (maker) order and an incoming (taker) order.
// Executes at the maker's price (price-time priority, no price improvement
// for the maker).
struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Qty qty;
    Side taker_side;

    friend bool operator==(const Trade&, const Trade&) = default;
};

// A resting order cancelled by self-trade prevention. Delivered to the trade
// sink if it accepts this type (see notify_stp); sinks that only take Trade
// simply don't see these events.
struct StpCancel {
    OrderId order_id;  // the resting order that was cancelled
    Qty qty;           // its remaining quantity at cancellation

    friend bool operator==(const StpCancel&, const StpCancel&) = default;
};

template <class Sink>
inline void notify_stp(Sink& sink, const StpCancel& c) {
    if constexpr (std::is_invocable_v<Sink&, const StpCancel&>) sink(c);
}

// Result of a single request.
struct ExecReport {
    Status status = Status::Ok;
    Qty filled = 0;   // quantity executed by this request
    Qty resting = 0;  // quantity left resting on the book after the request
    // Incoming quantity cancelled by self-trade prevention (CancelNewest /
    // CancelBoth). filled + resting + self_trade_cancelled <= order qty.
    Qty self_trade_cancelled = 0;

    [[nodiscard]] bool ok() const noexcept { return status == Status::Ok; }
};

// Book construction parameters shared by OrderBook and ReferenceBook.
struct BookConfig {
    Price min_price = 0;
    Price max_price = 65535;
    std::size_t max_orders = 1 << 20;
    // Sizing hint for the id index. Sized to the *typical* live order count,
    // not max_orders, so the hash table stays cache-resident; it grows if
    // the hint is exceeded.
    std::size_t expected_orders = 1 << 14;
};

// Default trade sink: discards fills.
struct NullSink {
    void operator()(const Trade&) const noexcept {}
};

struct LevelView {
    Price price;
    Qty qty;
    std::uint32_t orders;

    friend bool operator==(const LevelView&, const LevelView&) = default;
};

}  // namespace tickforge
