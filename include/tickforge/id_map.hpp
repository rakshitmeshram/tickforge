// Open-addressing hash map: OrderId -> uint32 pool index.
//
// Linear probing over a flat power-of-two array (one cache line holds four
// slots), splitmix64 hashing, and backward-shift deletion so lookups never
// have to step over tombstones. Replaces std::unordered_map, whose
// node-per-entry allocation and pointer chasing dominate cancel latency.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace tickforge {

class IdMap {
public:
    static constexpr std::uint64_t kEmpty = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint32_t kMissing = std::numeric_limits<std::uint32_t>::max();

    explicit IdMap(std::size_t expected = 1024) { rehash(capacity_for(expected)); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Returns kMissing if absent.
    [[nodiscard]] std::uint32_t find(std::uint64_t key) const noexcept {
        assert(key != kEmpty);
        std::size_t i = slot_of(key);
        for (;;) {
            const Slot& s = slots_[i];
            if (s.key == key) return s.value;
            if (s.key == kEmpty) return kMissing;
            i = (i + 1) & mask_;
        }
    }

    // Inserts key -> value. Returns false if key already present.
    bool insert(std::uint64_t key, std::uint32_t value) {
        assert(key != kEmpty);
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        std::size_t i = slot_of(key);
        for (;;) {
            Slot& s = slots_[i];
            if (s.key == key) return false;
            if (s.key == kEmpty) {
                s.key = key;
                s.value = value;
                ++size_;
                return true;
            }
            i = (i + 1) & mask_;
        }
    }

    // Removes key. Returns false if absent.
    bool erase(std::uint64_t key) noexcept {
        std::size_t i = slot_of(key);
        for (;;) {
            if (slots_[i].key == key) break;
            if (slots_[i].key == kEmpty) return false;
            i = (i + 1) & mask_;
        }
        // Backward-shift: pull later entries of the probe chain into the hole
        // whenever doing so does not move them before their home slot.
        std::size_t hole = i;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmpty) break;
            const std::size_t home = slot_of(slots_[j].key);
            // Is `home` cyclically outside (hole, j]? Then the entry may move.
            const bool movable = (hole <= j) ? (home <= hole || home > j) : (home <= hole && home > j);
            if (movable) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmpty;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (auto& s : slots_) s.key = kEmpty;
        size_ = 0;
    }

private:
    struct Slot {
        std::uint64_t key = kEmpty;
        std::uint32_t value = 0;
    };

    static std::uint64_t mix(std::uint64_t x) noexcept {  // splitmix64 finalizer
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    static std::size_t capacity_for(std::size_t n) noexcept {
        std::size_t c = 16;
        while (c < n * 2) c <<= 1;
        return c;
    }

    std::size_t slot_of(std::uint64_t key) const noexcept { return mix(key) & mask_; }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        size_ = 0;
        for (const auto& s : old)
            if (s.key != kEmpty) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

}  // namespace tickforge
