// Hierarchical (64-ary) bitset.
//
// Layer 0 holds one bit per price level. Bit w of layer k+1 is set iff word w
// of layer k is non-zero. Finding the next occupied price level above/below a
// given index therefore costs O(log64 N) word operations — 3 layers cover
// 262,144 levels — instead of a linear scan over empty levels.
#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tickforge {

class HierBitset {
public:
    static constexpr std::int64_t npos = -1;

    explicit HierBitset(std::size_t nbits) : nbits_(nbits) {
        assert(nbits > 0);
        std::size_t bits = nbits;
        for (;;) {
            const std::size_t words = (bits + 63) / 64;
            layers_.emplace_back(words, 0ULL);
            sizes_.push_back(bits);
            if (words == 1) break;
            bits = words;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return nbits_; }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        assert(i < nbits_);
        return (layers_[0][i >> 6] >> (i & 63)) & 1ULL;
    }

    void set(std::size_t i) noexcept {
        assert(i < nbits_);
        for (auto& layer : layers_) {
            const std::uint64_t before = layer[i >> 6];
            layer[i >> 6] = before | (1ULL << (i & 63));
            if (before != 0) return;  // upper layers already mark this word
            i >>= 6;
        }
    }

    void reset(std::size_t i) noexcept {
        assert(i < nbits_);
        for (auto& layer : layers_) {
            layer[i >> 6] &= ~(1ULL << (i & 63));
            if (layer[i >> 6] != 0) return;  // word still non-empty
            i >>= 6;
        }
    }

    // Smallest set index >= i, or npos.
    [[nodiscard]] std::int64_t next_set(std::int64_t i) const noexcept {
        if (i < 0) i = 0;
        return next_in(0, static_cast<std::size_t>(i));
    }

    // Largest set index <= i, or npos.
    [[nodiscard]] std::int64_t prev_set(std::int64_t i) const noexcept {
        if (i < 0) return npos;
        if (static_cast<std::size_t>(i) >= nbits_) i = static_cast<std::int64_t>(nbits_) - 1;
        return prev_in(0, i);
    }

    [[nodiscard]] bool any() const noexcept { return layers_.back()[0] != 0; }

private:
    std::int64_t next_in(std::size_t layer, std::size_t i) const noexcept {
        if (i >= sizes_[layer]) return npos;
        const auto& words = layers_[layer];
        const std::size_t w = i >> 6;
        const std::uint64_t bits = words[w] & (~0ULL << (i & 63));
        if (bits)
            return static_cast<std::int64_t>((w << 6) + static_cast<std::size_t>(std::countr_zero(bits)));
        if (layer + 1 == layers_.size()) return npos;
        const std::int64_t nw = next_in(layer + 1, w + 1);
        if (nw == npos) return npos;
        const auto uw = static_cast<std::size_t>(nw);
        return static_cast<std::int64_t>((uw << 6) + static_cast<std::size_t>(std::countr_zero(words[uw])));
    }

    std::int64_t prev_in(std::size_t layer, std::int64_t i) const noexcept {
        if (i < 0) return npos;
        const auto& words = layers_[layer];
        const auto w = static_cast<std::size_t>(i >> 6);
        const unsigned b = static_cast<unsigned>(i & 63);
        const std::uint64_t mask = (b == 63) ? ~0ULL : ((1ULL << (b + 1)) - 1);
        const std::uint64_t bits = words[w] & mask;
        if (bits)
            return static_cast<std::int64_t>((w << 6) + 63 -
                                             static_cast<std::size_t>(std::countl_zero(bits)));
        if (layer + 1 == layers_.size()) return npos;
        const std::int64_t pw = prev_in(layer + 1, static_cast<std::int64_t>(w) - 1);
        if (pw == npos) return npos;
        const auto uw = static_cast<std::size_t>(pw);
        return static_cast<std::int64_t>((uw << 6) + 63 -
                                         static_cast<std::size_t>(std::countl_zero(words[uw])));
    }

    std::size_t nbits_;
    std::vector<std::vector<std::uint64_t>> layers_;
    std::vector<std::size_t> sizes_;
};

}  // namespace tickforge
