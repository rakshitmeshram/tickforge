// tf_replay — replay a message file through the engine and export market data
// for research: a top-of-book / depth snapshot after each message and the
// full trade tape.
//
//   tf_replay --in data/flow.bin --book data/book.csv --trades data/trades.csv
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include "cli.hpp"
#include "tickforge/flow_generator.hpp"
#include "tickforge/order_book.hpp"

using namespace tickforge;

namespace {
struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f) std::fclose(f);
    }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

File open_or_throw(const std::string& path) {
    File f(std::fopen(path.c_str(), "w"));
    if (!f) throw std::runtime_error("cannot open " + path);
    return f;
}
}  // namespace

int main(int argc, char** argv) try {
    const cli::Args args(argc, argv);
    const auto msgs = read_messages(args.str("in", "flow.bin"));
    const auto every = static_cast<std::size_t>(args.num("every", 1, 1));
    constexpr std::size_t kLevels = 5;

    File book_csv = open_or_throw(args.str("book", "book.csv"));
    File trade_csv = open_or_throw(args.str("trades", "trades.csv"));
    std::fprintf(book_csv.get(), "seq,bid,ask,bid_qty1,ask_qty1,bid_qty5,ask_qty5\n");
    std::fprintf(trade_csv.get(), "seq,price,qty,taker_side\n");

    FlowParams p;
    OrderBook book({p.min_price, p.max_price, 1 << 21});
    std::size_t seq = 0, ntrades = 0;
    auto on_trade = [&](const Trade& t) {
        ++ntrades;
        std::fprintf(trade_csv.get(), "%zu,%lld,%lld,%c\n", seq, static_cast<long long>(t.price),
                     static_cast<long long>(t.qty), t.taker_side == Side::Buy ? 'B' : 'S');
    };

    LevelView bids[kLevels], asks[kLevels];
    for (; seq < msgs.size(); ++seq) {
        apply(book, msgs[seq], on_trade);
        if (seq % every != 0) continue;
        const std::size_t nb = book.depth(Side::Buy, kLevels, bids);
        const std::size_t na = book.depth(Side::Sell, kLevels, asks);
        if (nb == 0 || na == 0) continue;
        Qty b5 = 0, a5 = 0;
        for (std::size_t i = 0; i < nb; ++i) b5 += bids[i].qty;
        for (std::size_t i = 0; i < na; ++i) a5 += asks[i].qty;
        std::fprintf(book_csv.get(), "%zu,%lld,%lld,%lld,%lld,%lld,%lld\n", seq,
                     static_cast<long long>(bids[0].price), static_cast<long long>(asks[0].price),
                     static_cast<long long>(bids[0].qty), static_cast<long long>(asks[0].qty),
                     static_cast<long long>(b5), static_cast<long long>(a5));
    }
    std::printf("replayed %zu messages, %zu trades, %zu orders resting at end\n", msgs.size(), ntrades,
                book.order_count());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "tf_replay: %s\n", e.what());
    return 1;
}
