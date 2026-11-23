// Cross-platform determinism: fixed seeds must produce the same message
// stream on every compiler, standard library and OS. The hashes below were
// pinned on Windows / Clang 22 (zig c++); CI checks them on Linux with GCC and
// Clang (including sanitizer builds) and on Windows with MSVC.
//
// If this test fails on one platform only, its math library rounds a
// transcendental function differently (std::log / std::cos in the Gaussian
// and lognormal transforms), and research results are then not portable to
// that platform. If it fails everywhere, the generator itself changed: check
// the change was intended, then re-pin with the values the test prints.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "test_framework.hpp"
#include "tickforge/flow_generator.hpp"
#include "tickforge/order_book.hpp"

using namespace tickforge;

namespace {

// FNV-1a over explicit fixed-width fields, so the hash does not depend on
// struct layout or padding.
struct Fnv {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    void add(std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    }
};

struct Digest {
    std::uint64_t stream;  // every field of every message
    std::uint64_t fair;    // bit pattern of the final hidden fair value
};

Digest digest(std::uint64_t seed, const FlowParams& p, std::size_t n) {
    OrderBook book({p.min_price, p.max_price, 1 << 21});
    FlowGenerator gen(seed, p);
    Fnv f;
    for (const Msg& m : gen.generate(book, n)) {
        f.add(static_cast<std::uint64_t>(m.type), 1);
        f.add(static_cast<std::uint64_t>(m.side), 1);
        f.add(static_cast<std::uint64_t>(m.tif), 1);
        f.add(static_cast<std::uint64_t>(m.stp), 1);
        f.add(m.owner, 4);
        f.add(m.id, 8);
        f.add(static_cast<std::uint64_t>(m.price), 8);
        f.add(static_cast<std::uint64_t>(m.qty), 8);
    }
    const double fv = gen.fair_value();
    std::uint64_t bits = 0;
    std::memcpy(&bits, &fv, sizeof bits);
    return {f.h, bits};
}

void check(const char* name, Digest got, Digest want) {
    std::printf("    %-8s stream 0x%016llx  fair 0x%016llx\n", name,
                static_cast<unsigned long long>(got.stream), static_cast<unsigned long long>(got.fair));
    CHECK_EQ(got.stream, want.stream);
    CHECK_EQ(got.fair, want.fair);
}

}  // namespace

// Default flow, as in tf_bench's typical book and the imbalance study (seed 7).
TEST(stream_hash_default_flow) {
    check("seed1", digest(1, FlowParams{}, 200000), {0x88fb670e61e67a8eULL, 0x40c3a2c6a6cf1219ULL});
    check("seed7", digest(7, FlowParams{}, 200000), {0x3647baa5c3653d3bULL, 0x40c3a37a4b0c0c1bULL});
}

// Deep, wide book (tf_bench --live 200000 --decay_pct 2).
TEST(stream_hash_deep_book) {
    FlowParams p;
    p.target_live_orders = 200000;
    p.depth_decay = 0.02;
    check("deep", digest(1, p, 200000), {0xd18894bbd607b680ULL, 0x40c38f84043eac4dULL});
}

// Non-default volatility, market-order share and informed flow, so the
// transforms are also checked away from the default parameters.
TEST(stream_hash_custom_params) {
    FlowParams p;
    p.sigma = 0.35;
    p.p_market = 0.10;
    p.informed_k = 2.5;
    check("custom", digest(101, p, 200000), {0x738ece7ba7b94951ULL, 0x40c3683f577a3fdcULL});
}

TF_TEST_MAIN
