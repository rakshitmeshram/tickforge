// tf_bench — latency and throughput of OrderBook vs the std::map ReferenceBook
// on the same reproducible message stream.
//
//   tf_bench --n 2000000 --seed 1 --runs 5 --json results/bench.json --samples results/latency_samples.csv
//
// Options --live N and --decay_pct P set the generator's target book size and
// how far orders spread from the fair value (smaller P = wider book).
// --expected_orders N sets the fast book's id-index sizing hint.
//
// Method
//   * The stream is generated up front; replay touches only memory.
//   * Throughput: untimed replay of the whole stream, best of --runs.
//   * Latency: every message is bracketed by timer reads; the median cost of
//     an empty bracket is measured and subtracted. Percentiles are reported
//     per message type and overall. On x86-64 with an invariant TSC the timer
//     is lfence-fenced rdtsc, calibrated against steady_clock, which resolves
//     well under a nanosecond. Elsewhere it is steady_clock, whose resolution
//     is reported (100 ns on Windows), so coarse percentiles are visible.
//   * Each run starts from a fresh book; one warm-up replay precedes timing.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define TF_HAVE_TSC 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#elif defined(_MSC_VER) && !defined(__clang__)
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

// Per-message timer: raw ticks plus a calibrated tick length.
struct Timer {
    bool tsc = false;
    double ns_per_tick = 1.0;
    double resolution_ns = 1.0;  // smallest non-zero difference two reads can show
    double overhead_ns = 0.0;    // median cost of an empty bracket, subtracted

    std::uint64_t now() const noexcept {
#ifdef TF_HAVE_TSC
        if (tsc) {
            _mm_lfence();
            const std::uint64_t t = __rdtsc();
            _mm_lfence();
            return t;
        }
#endif
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    }
    double ns(std::uint64_t a, std::uint64_t b) const noexcept {
        return static_cast<double>(b - a) * ns_per_tick;
    }
    const char* name() const noexcept { return tsc ? "rdtsc" : "steady_clock"; }
};

#ifdef TF_HAVE_TSC
bool invariant_tsc() {
    unsigned d = 0;
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4];
    __cpuid(r, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(r[0]) < 0x80000007u) return false;
    __cpuid(r, static_cast<int>(0x80000007u));
    d = static_cast<unsigned>(r[3]);
#else
    unsigned a = 0, b = 0, c = 0;
    if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000007u) return false;
    __get_cpuid(0x80000007u, &a, &b, &c, &d);
#endif
    return (d & (1u << 8)) != 0;  // CPUID.80000007H:EDX[8], invariant TSC
}
#endif

Timer make_timer() {
    Timer t;
#ifdef TF_HAVE_TSC
    if (invariant_tsc()) {
        // Calibrate TSC ticks against steady_clock over ~250 ms.
        t.tsc = true;
        const auto c0 = Clock::now();
        const std::uint64_t k0 = t.now();
        while (Clock::now() - c0 < std::chrono::milliseconds(250)) {
        }
        const auto c1 = Clock::now();
        const std::uint64_t k1 = t.now();
        t.ns_per_tick =
            std::chrono::duration<double, std::nano>(c1 - c0).count() / static_cast<double>(k1 - k0);
        t.resolution_ns = t.ns_per_tick;
    }
#endif
    if (!t.tsc) {
        // Smallest non-zero step steady_clock shows.
        std::int64_t step = INT64_MAX;
        for (int i = 0; i < 1000; ++i) {
            const auto a = Clock::now();
            auto b = Clock::now();
            while (b == a) b = Clock::now();
            step = std::min<std::int64_t>(
                step, std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
        t.resolution_ns = static_cast<double>(step);
    }
    std::vector<double> d(100000);
    for (auto& x : d) {
        const std::uint64_t a = t.now();
        const std::uint64_t b = t.now();
        x = t.ns(a, b);
    }
    std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
    t.overhead_ns = d[d.size() / 2];
    return t;
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
Result bench(const char* name, const std::vector<Msg>& msgs, int runs, const Timer& timer,
             std::size_t expected_orders) {
    const FlowParams p;
    const typename Book::Config cfg{p.min_price, p.max_price, 1 << 21, expected_orders};
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
        const std::uint64_t t0 = timer.now();
        do_not_optimize(apply(b, msgs[i], Counter{&trades}));
        const std::uint64_t t1 = timer.now();
        const double ns = timer.ns(t0, t1) - timer.overhead_ns;
        r.lat[i] = static_cast<std::int32_t>(std::max(0.0, ns + 0.5));
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
                const Timer& timer) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fprintf(
        f,
        "{\n  \"messages\": %zu,\n  \"seed\": %llu,\n  \"timer\": \"%s\",\n  \"timer_resolution_ns\": "
        "%.3f,\n  \"clock_overhead_ns\": %.1f,\n  \"books\": [\n",
        n, static_cast<unsigned long long>(seed), timer.name(), timer.resolution_ns, timer.overhead_ns);
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
    // Id-index sizing hint for the fast book. Setting it to max_orders (2097152)
    // reproduces the oversized-hash-table regression described in DESIGN.md.
    const auto expected = static_cast<std::size_t>(
        args.num("expected_orders", static_cast<long long>(BookConfig{}.expected_orders), 1, 1 << 21));

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
    const Timer timer = make_timer();
    std::printf(
        "tickforge benchmark: %zu messages, seed %llu, timer %s (resolution %.2f ns), overhead %.1f ns "
        "(subtracted)\n",
        msgs.size(), static_cast<unsigned long long>(seed), timer.name(), timer.resolution_ns,
        timer.overhead_ns);

    std::vector<Result> rs;
    rs.push_back(bench<OrderBook>("tickforge::OrderBook", msgs, runs, timer, expected));
    rs.push_back(bench<ReferenceBook>("std::map baseline", msgs, runs, timer, expected));
    for (const auto& r : rs) print(r);
    std::printf("\nspeedup: throughput %.1fx, p50 %.1fx, p99 %.1fx\n",
                rs[0].mmsgs_per_sec / rs[1].mmsgs_per_sec, rs[1].all[0] / std::max(1.0, rs[0].all[0]),
                rs[1].all[2] / std::max(1.0, rs[0].all[2]));

    if (args.has("json")) write_json(args.str("json", ""), rs, msgs.size(), seed, timer);
    if (args.has("samples"))
        write_samples(args.str("samples", ""), rs, msgs, std::max<std::size_t>(1, msgs.size() / 200000));
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "tf_bench: %s\n", e.what());
    return 1;
}
