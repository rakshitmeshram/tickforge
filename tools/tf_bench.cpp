// tf_bench — latency and throughput of OrderBook vs the std::map ReferenceBook
// on the same reproducible message stream.
//
//   tf_bench --n 2000000 --seed 1 --runs 5 --json results/bench.json --samples results/latency_samples.csv
//
// Options --live N and --decay_pct P set the generator's target book size and
// how far orders spread from the fair value (smaller P = wider book).
//
// Method
//   * The stream is generated up front; replay touches only memory.
//   * Throughput: untimed replay of the whole stream, best of --runs.
//   * Latency: every message is bracketed by steady_clock reads; the median
//     cost of an empty bracket is measured and subtracted. Percentiles are
//     reported per message type and overall.
//   * Each run starts from a fresh book; one warm-up replay precedes timing.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#include "cli.hpp"
#include "tickforge/flow_generator.hpp"
#include "tickforge/order_book.hpp"
#include "tickforge/reference_book.hpp"

using namespace tickforge;
using Clock = std::chrono::steady_clock;

namespace {

struct Counter {
    std::size_t* trades;
    void operator()(const Trade&) const noexcept { ++*trades; }
};

// Keeps the compiler from discarding a result it can prove unused.
#if defined(_MSC_VER) && !defined(__clang__)
template <class T>
inline void do_not_optimize(const T& v) {
    // MSVC has no GNU inline asm: force a read of the object through a
    // volatile pointer and block compiler reordering around it.
    static_cast<void>(*reinterpret_cast<const volatile char*>(&v));
    _ReadWriteBarrier();
}
#else
template <class T>
inline void do_not_optimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}
#endif

double clock_overhead_ns() {
    std::vector<std::int64_t> d(100000);
    for (auto& x : d) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        x = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    }
    std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
    return static_cast<double>(d[d.size() / 2]);
}

struct Result {
    std::string name;
    double mmsgs_per_sec = 0;
    double pct[4][5] = {};  // [type][p50,p90,p99,p99.9,mean]
    double all[5] = {};
    std::vector<std::int32_t> lat;  // per message, ns
};

void percentiles(std::vector<std::int32_t> v, double out[5]) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        return static_cast<double>(v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))]);
    };
    out[0] = at(0.50);
    out[1] = at(0.90);
    out[2] = at(0.99);
    out[3] = at(0.999);
    double s = 0;
    for (auto x : v) s += x;
    out[4] = s / static_cast<double>(v.size());
}

template <class Book>
Result bench(const char* name, const std::vector<Msg>& msgs, int runs, double overhead) {
    const FlowParams p;
    const typename Book::Config cfg{p.min_price, p.max_price, 1 << 21};
    Result r;
    r.name = name;
    std::size_t trades = 0;

    {  // warm-up
        Book b(cfg);
        for (const auto& m : msgs) do_not_optimize(apply(b, m, Counter{&trades}));
    }
    double best = 1e300;
    for (int i = 0; i < runs; ++i) {
        Book b(cfg);
        const auto t0 = Clock::now();
        for (const auto& m : msgs) do_not_optimize(apply(b, m, Counter{&trades}));
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        best = std::min(best, s);
    }
    r.mmsgs_per_sec = static_cast<double>(msgs.size()) / best / 1e6;

    Book b(cfg);
    r.lat.resize(msgs.size());
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        const auto t0 = Clock::now();
        do_not_optimize(apply(b, msgs[i], Counter{&trades}));
        const auto t1 = Clock::now();
        const double ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) -
            overhead;
        r.lat[i] = static_cast<std::int32_t>(std::max(0.0, ns));
    }
    std::vector<std::int32_t> by_type[4];
    for (std::size_t i = 0; i < msgs.size(); ++i) by_type[static_cast<int>(msgs[i].type)].push_back(r.lat[i]);
    for (int t = 0; t < 4; ++t) percentiles(by_type[t], r.pct[t]);
    percentiles(r.lat, r.all);
    do_not_optimize(trades);
    return r;
}

const char* kTypeNames[4] = {"limit", "market", "cancel", "amend"};

void print(const Result& r) {
    std::printf("\n%s  —  throughput %.2f M msgs/s\n", r.name.c_str(), r.mmsgs_per_sec);
    std::printf("  %-8s %8s %8s %8s %8s %8s   (ns)\n", "type", "p50", "p90", "p99", "p99.9", "mean");
    for (int t = 0; t < 4; ++t)
        std::printf("  %-8s %8.0f %8.0f %8.0f %8.0f %8.1f\n", kTypeNames[t], r.pct[t][0], r.pct[t][1],
                    r.pct[t][2], r.pct[t][3], r.pct[t][4]);
    std::printf("  %-8s %8.0f %8.0f %8.0f %8.0f %8.1f\n", "all", r.all[0], r.all[1], r.all[2], r.all[3],
                r.all[4]);
}

void write_json(const std::string& path, const std::vector<Result>& rs, std::size_t n, std::uint64_t seed,
                double overhead) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fprintf(
        f, "{\n  \"messages\": %zu,\n  \"seed\": %llu,\n  \"clock_overhead_ns\": %.1f,\n  \"books\": [\n", n,
        static_cast<unsigned long long>(seed), overhead);
    for (std::size_t k = 0; k < rs.size(); ++k) {
        const auto& r = rs[k];
        std::fprintf(f, "    {\"name\": \"%s\", \"mmsgs_per_sec\": %.3f, \"latency_ns\": {", r.name.c_str(),
                     r.mmsgs_per_sec);
        for (int t = 0; t < 5; ++t) {
            const double* v = t < 4 ? r.pct[t] : r.all;
            std::fprintf(
                f, "%s\"%s\": {\"p50\": %.0f, \"p90\": %.0f, \"p99\": %.0f, \"p999\": %.0f, \"mean\": %.1f}",
                t ? ", " : "", t < 4 ? kTypeNames[t] : "all", v[0], v[1], v[2], v[3], v[4]);
        }
        std::fprintf(f, "}}%s\n", k + 1 < rs.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
}

void write_samples(const std::string& path, const std::vector<Result>& rs, const std::vector<Msg>& msgs,
                   std::size_t stride) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fprintf(f, "book,type,ns\n");
    for (const auto& r : rs)
        for (std::size_t i = 0; i < msgs.size(); i += stride)
            std::fprintf(f, "%s,%s,%d\n", r.name.c_str(), kTypeNames[static_cast<int>(msgs[i].type)],
                         r.lat[i]);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) try {
    const cli::Args args(argc, argv);
    const auto n = static_cast<std::size_t>(args.num("n", 2'000'000, 1));
    const auto seed = static_cast<std::uint64_t>(args.num("seed", 1, 0));
    const int runs = static_cast<int>(args.num("runs", 5, 1, 1000));

    std::vector<Msg> msgs;
    if (args.has("in")) {
        msgs = read_messages(args.str("in", ""));
        if (msgs.empty()) throw std::runtime_error("input file contains no messages");
    } else {
        FlowParams p;
        p.target_live_orders = static_cast<double>(args.num("live", 4000, 1));
        // Geometric parameter must lie strictly inside (0, 1).
        p.depth_decay = static_cast<double>(args.num("decay_pct", 25, 1, 99)) / 100.0;
        OrderBook driver({p.min_price, p.max_price, 1 << 21});
        FlowGenerator gen(seed, p);
        msgs = gen.generate(driver, n);
    }
    const double overhead = clock_overhead_ns();
    std::printf("tickforge benchmark: %zu messages, seed %llu, clock overhead %.0f ns (subtracted)\n",
                msgs.size(), static_cast<unsigned long long>(seed), overhead);

    std::vector<Result> rs;
    rs.push_back(bench<OrderBook>("tickforge::OrderBook", msgs, runs, overhead));
    rs.push_back(bench<ReferenceBook>("std::map baseline", msgs, runs, overhead));
    for (const auto& r : rs) print(r);
    std::printf("\nspeedup: throughput %.1fx, p50 %.1fx, p99 %.1fx\n",
                rs[0].mmsgs_per_sec / rs[1].mmsgs_per_sec, rs[1].all[0] / std::max(1.0, rs[0].all[0]),
                rs[1].all[2] / std::max(1.0, rs[0].all[2]));

    if (args.has("json")) write_json(args.str("json", ""), rs, msgs.size(), seed, overhead);
    if (args.has("samples"))
        write_samples(args.str("samples", ""), rs, msgs, std::max<std::size_t>(1, msgs.size() / 200000));
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "tf_bench: %s\n", e.what());
    return 1;
}
