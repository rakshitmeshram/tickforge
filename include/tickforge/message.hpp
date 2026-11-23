// Fixed-size binary order-entry message (32 bytes, trivially copyable).
//
// Message streams are written to disk as raw arrays of Msg preceded by a small
// header, so a replay is a single read() into memory followed by a tight loop
// — I/O and parsing never pollute the latency measurements.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "tickforge/types.hpp"

namespace tickforge {

enum class MsgType : std::uint8_t { Limit = 0, Market = 1, Cancel = 2, Amend = 3 };

// Layout: the owner/STP fields occupy what used to be padding, so files
// written before STP existed (zeroed padding) read back as owner 0 = no STP.
struct Msg {
    MsgType type;
    Side side;
    Tif tif;
    Stp stp;        // self-trade prevention mode (used only if owner != 0)
    OwnerId owner;  // 0 = anonymous, never self-matches
    OrderId id;
    Price price;
    Qty qty;
};
static_assert(sizeof(Msg) == 32);
static_assert(std::is_trivially_copyable_v<Msg>);

// Convenience constructor for anonymous (owner 0) messages.
[[nodiscard]] constexpr Msg make_msg(MsgType type, Side side, Tif tif, OrderId id, Price price, Qty qty,
                                     OrderOwner owner = {}) noexcept {
    return Msg{type, side, tif, owner.stp, owner.id, id, price, qty};
}

// True if the enum fields hold values the engine understands. Messages read
// from files or received over an FFI are raw bytes: a side byte of 2 would
// otherwise index past the end of the book's per-side arrays.
[[nodiscard]] constexpr bool is_valid(const Msg& m) noexcept {
    return static_cast<std::uint8_t>(m.type) <= static_cast<std::uint8_t>(MsgType::Amend) &&
           static_cast<std::uint8_t>(m.side) <= static_cast<std::uint8_t>(Side::Sell) &&
           static_cast<std::uint8_t>(m.tif) <= static_cast<std::uint8_t>(Tif::FOK) &&
           static_cast<std::uint8_t>(m.stp) <= static_cast<std::uint8_t>(Stp::CancelBoth);
}

// Applies one message to any book type exposing the OrderBook interface.
// Malformed messages are rejected with RejectedInvalidMessage and never
// touch the book.
template <class Book, class Sink>
ExecReport apply(Book& book, const Msg& m, Sink&& on_trade) {
    if (!is_valid(m)) return {Status::RejectedInvalidMessage, 0, 0};
    switch (m.type) {
        case MsgType::Limit:
            return book.limit(m.id, m.side, m.price, m.qty, m.tif, on_trade, OrderOwner{m.owner, m.stp});
        case MsgType::Market:
            return book.market(m.id, m.side, m.qty, on_trade, OrderOwner{m.owner, m.stp});
        case MsgType::Cancel:
            return book.cancel(m.id);
        case MsgType::Amend:
            return book.amend(m.id, m.price, m.qty, on_trade);
    }
    return {Status::RejectedInvalidMessage, 0, 0};
}

// ------------------------------------------------------------------ file I/O

inline constexpr std::uint64_t kMsgFileMagic = 0x3147534d46544bULL;  // "KTFMSG1"

inline void write_messages(const std::string& path, const std::vector<Msg>& msgs) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open " + path + " for writing");
    const std::uint64_t header[2] = {kMsgFileMagic, msgs.size()};
    bool ok = std::fwrite(header, sizeof header, 1, f) == 1;
    if (ok && !msgs.empty()) ok = std::fwrite(msgs.data(), sizeof(Msg), msgs.size(), f) == msgs.size();
    std::fclose(f);
    if (!ok) throw std::runtime_error("short write to " + path);
}

// Reads a message file written by write_messages. The header's record count
// is checked against the actual file size before anything is allocated, and
// every record is validated, so a corrupt or hostile file fails with an
// exception instead of a huge allocation or out-of-bounds book access.
inline std::vector<Msg> read_messages(const std::string& path) {
    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot open " + path + ": " + ec.message());

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::uint64_t header[2] = {0, 0};
    if (file_size < sizeof header || std::fread(header, sizeof header, 1, f) != 1 || header[0] != kMsgFileMagic) {
        std::fclose(f);
        throw std::runtime_error(path + ": not a tickforge message file");
    }
    const std::uintmax_t records = (file_size - sizeof header) / sizeof(Msg);
    if (header[1] != records || (file_size - sizeof header) % sizeof(Msg) != 0) {
        std::fclose(f);
        throw std::runtime_error(path + ": header says " + std::to_string(header[1]) + " messages but the file holds " +
                                 std::to_string(file_size - sizeof header) + " bytes of records (truncated or corrupt)");
    }
    std::vector<Msg> msgs(static_cast<std::size_t>(records));
    const bool ok = msgs.empty() || std::fread(msgs.data(), sizeof(Msg), msgs.size(), f) == msgs.size();
    std::fclose(f);
    if (!ok) throw std::runtime_error(path + ": truncated");
    for (std::size_t i = 0; i < msgs.size(); ++i)
        if (!is_valid(msgs[i]))
            throw std::runtime_error(path + ": invalid message at index " + std::to_string(i) +
                                     " (bad type, side or time-in-force byte)");
    return msgs;
}

}  // namespace tickforge
