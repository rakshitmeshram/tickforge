// tf_gen — write a reproducible synthetic order-flow file.
//
//   tf_gen --n 2000000 --seed 1 --out data/flow.bin
#include <cstdio>
#include <exception>

#include "cli.hpp"
#include "tickforge/flow_generator.hpp"
#include "tickforge/order_book.hpp"

using namespace tickforge;

int main(int argc, char** argv) try {
    const cli::Args args(argc, argv);
    const auto n = static_cast<std::size_t>(args.num("n", 1'000'000, 1));
    const auto seed = static_cast<std::uint64_t>(args.num("seed", 1, 0));
    const std::string out = args.str("out", "flow.bin");

    FlowParams p;
    OrderBook driver({p.min_price, p.max_price, 1 << 21});
    FlowGenerator gen(seed, p);
    const auto msgs = gen.generate(driver, n);
    write_messages(out, msgs);

    std::size_t counts[4] = {};
    for (const auto& m : msgs) ++counts[static_cast<int>(m.type)];
    std::printf(
        "wrote %zu messages to %s (limit %zu, market %zu, cancel %zu, amend %zu); "
        "final book: %zu orders, bid %lld / ask %lld\n",
        msgs.size(), out.c_str(), counts[0], counts[1], counts[2], counts[3], driver.order_count(),
        static_cast<long long>(driver.best_bid()), static_cast<long long>(driver.best_ask()));
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "tf_gen: %s\n", e.what());
    return 1;
}
