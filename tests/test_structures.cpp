// Property tests for the building blocks, each checked against a trivially
// correct std:: equivalent over long random operation sequences.
#include <cstdint>
#include <set>
#include <unordered_map>

#include "test_framework.hpp"
#include "tickforge/flow_generator.hpp"
#include "tickforge/hier_bitset.hpp"
#include "tickforge/id_map.hpp"

using namespace tickforge;

TEST(bitset_next_prev_match_std_set) {
    for (std::size_t n : {1u, 63u, 64u, 65u, 4096u, 4097u, 300000u}) {
        HierBitset bs(n);
        std::set<std::int64_t> ref;
        Xoshiro256 rng(n);
        for (int step = 0; step < 20000; ++step) {
            const auto i = static_cast<std::int64_t>(rng.below(n));
            if (rng.uniform() < 0.55) {
                bs.set(static_cast<std::size_t>(i));
                ref.insert(i);
            } else {
                bs.reset(static_cast<std::size_t>(i));
                ref.erase(i);
            }
            const auto q = static_cast<std::int64_t>(rng.below(n));
            auto it = ref.lower_bound(q);
            const std::int64_t want_next = it == ref.end() ? HierBitset::npos : *it;
            auto jt = ref.upper_bound(q);
            const std::int64_t want_prev = jt == ref.begin() ? HierBitset::npos : *std::prev(jt);
            if (bs.next_set(q) != want_next || bs.prev_set(q) != want_prev ||
                bs.test(static_cast<std::size_t>(i)) != (ref.count(i) == 1) || bs.any() != !ref.empty()) {
                CHECK(false);
                std::printf("    n=%zu step=%d q=%lld\n", n, step, static_cast<long long>(q));
                return;
            }
        }
    }
}

TEST(bitset_edges) {
    HierBitset bs(1000);
    CHECK_EQ(bs.next_set(0), HierBitset::npos);
    CHECK_EQ(bs.prev_set(999), HierBitset::npos);
    bs.set(0);
    bs.set(999);
    CHECK_EQ(bs.next_set(1), 999);
    CHECK_EQ(bs.prev_set(998), 0);
    CHECK_EQ(bs.prev_set(-1), HierBitset::npos);
    CHECK_EQ(bs.next_set(1000), HierBitset::npos);
    CHECK_EQ(bs.prev_set(5000), 999);
}

TEST(id_map_matches_unordered_map) {
    IdMap m(4);  // start tiny to force many rehashes
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    Xoshiro256 rng(42);
    for (int step = 0; step < 400000; ++step) {
        // Small key space => many collisions, re-inserts and deletes.
        const std::uint64_t k = rng.below(5000);
        const double u = rng.uniform();
        if (u < 0.5) {
            const auto v = static_cast<std::uint32_t>(step);
            const bool inserted = m.insert(k, v);
            const bool ref_inserted = ref.emplace(k, v).second;
            if (inserted != ref_inserted) {
                CHECK(false);
                return;
            }
        } else if (u < 0.8) {
            if (m.erase(k) != (ref.erase(k) == 1)) {
                CHECK(false);
                return;
            }
        } else {
            auto it = ref.find(k);
            const std::uint32_t want = it == ref.end() ? IdMap::kMissing : it->second;
            if (m.find(k) != want) {
                CHECK(false);
                return;
            }
        }
        if (m.size() != ref.size()) {
            CHECK(false);
            return;
        }
    }
    for (const auto& [k, v] : ref) CHECK_EQ(m.find(k), v);
}

TEST(rng_is_deterministic) {
    Xoshiro256 a(7), b(7), c(8);
    bool same = true, differs = false;
    for (int i = 0; i < 1000; ++i) {
        const auto x = a.next();
        same &= x == b.next();
        differs |= x != c.next();
    }
    CHECK(same);
    CHECK(differs);
}

TF_TEST_MAIN
