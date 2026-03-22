#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// Robin Hood open-addressing hash map: int64 -> size_t
// Zero pointer chasing — all data in one flat array
class RobinHoodIndex {
public:
    static constexpr std::size_t kEmpty = ~std::size_t(0);

    explicit RobinHoodIndex(std::size_t capacity = 65536) {
        capacity_ = next_pow2(capacity);
        mask_      = capacity_ - 1;
        slots_.resize(capacity_);
        for (auto& s : slots_) s.dist = -1;
    }

    void reserve(std::size_t n) {
        std::size_t target = next_pow2(static_cast<std::size_t>(n / 0.75) + 1);
        if (target > capacity_) rehash(target);
    }

    bool insert(std::int64_t key, std::size_t val) {
        if ((count_ + 1) * 4 >= capacity_ * 3) {
            rehash(capacity_ * 2);
        }
        Slot cur{key, val, 0};
        std::size_t pos = hash(key) & mask_;
        for (;;) {
            Slot& s = slots_[pos];
            if (s.dist < 0) {           // empty slot
                s = cur;
                ++count_;
                return true;
            }
            if (s.key == key) {         // duplicate
                s.val = val;            // update
                return false;
            }
            if (s.dist < cur.dist) {    // robin hood: steal from rich
                std::swap(s, cur);
            }
            pos = (pos + 1) & mask_;
            ++cur.dist;
        }
    }

    std::size_t lookup(std::int64_t key) const {
        std::size_t pos = hash(key) & mask_;
        int dist = 0;
        for (;;) {
            const Slot& s = slots_[pos];
            if (s.dist < 0 || s.dist < dist) return kEmpty;
            if (s.key == key) return s.val;
            pos = (pos + 1) & mask_;
            ++dist;
        }
    }

    std::size_t size() const { return count_; }

private:
    struct Slot {
        std::int64_t key  = 0;
        std::size_t  val  = 0;
        int          dist = -1;  // -1 = empty
    };

    static std::uint64_t hash(std::int64_t k) {
        std::uint64_t x = static_cast<std::uint64_t>(k);
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }

    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old = std::move(slots_);
        capacity_ = new_cap;
        mask_     = new_cap - 1;
        slots_.assign(new_cap, Slot{});
        for (auto& s : slots_) s.dist = -1;
        count_ = 0;
        for (auto& s : old) {
            if (s.dist >= 0) insert(s.key, s.val);
        }
    }

    std::vector<Slot> slots_;
    std::size_t capacity_ = 0;
    std::size_t mask_     = 0;
    std::size_t count_    = 0;
};