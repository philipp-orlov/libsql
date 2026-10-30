// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Open-addressing hash sets for the encoded keys the executor builds per row
// (GROUP BY, DISTINCT, aggregate DISTINCT, IN (SELECT ...)).
//
// Keys are packed back to back in one byte buffer and the table holds only
// 32-bit references into it, so a lookup touches two arrays instead of
// chasing list nodes, and clearing for the next run keeps both arrays: a
// statement that runs again allocates nothing once its keys have been seen.
// `release` gives the memory back when a run made it unusually large.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "sql/error.hpp"

namespace sql::internal {

/// A fast 64-bit hash for short byte strings (keys are mostly < 32 bytes).
/// Fixed-size loads only: the tail is read with overlapping loads rather than
/// a variable-length copy, which would be a library call.
inline std::uint64_t hashBytes(const char* p, std::size_t n) noexcept
{
    constexpr std::uint64_t k1 = 0x9E3779B97F4A7C15ull, k2 = 0xC2B2AE3D27D4EB4Full;
    auto load64 = [](const char* q) {
        std::uint64_t w;
        std::memcpy(&w, q, 8);
        return w;
    };
    auto load32 = [](const char* q) {
        std::uint32_t w;
        std::memcpy(&w, q, 4);
        return std::uint64_t(w);
    };
    std::uint64_t h = k2 ^ (n * k1);
    std::uint64_t tail;
    if (n >= 8) {
        const char* const last = p + n - 8;
        while (p < last) {
            h = (h ^ (load64(p) * k1)) * k2;
            h ^= h >> 29;
            p += 8;
        }
        tail = load64(last);
    } else if (n >= 4) {
        tail = load32(p) | (load32(p + n - 4) << 32);
    } else if (n > 0) {
        tail = std::uint64_t(static_cast<unsigned char>(p[0])) |
               std::uint64_t(static_cast<unsigned char>(p[n / 2])) << 8 |
               std::uint64_t(static_cast<unsigned char>(p[n - 1])) << 16;
    } else {
        tail = 0;
    }
    h = (h ^ (tail * k1)) * k2;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

/// A set of byte strings that numbers its members in insertion order
/// (0, 1, 2, ...), so it doubles as a map from key to a dense index.
///
/// Each table slot holds the member's index and the high half of its hash,
/// so a probe compares fingerprints without touching the member itself.
class FlatKeySet
{
public:
    static constexpr std::uint32_t npos = 0xFFFFFFFFu;

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    /// The member's index, or npos.
    std::uint32_t find(std::string_view key) const noexcept
    {
        if (entries_.empty())
            return npos;
        const std::uint64_t h = hashBytes(key.data(), key.size());
        const std::uint64_t tag = h & kTagMask;
        for (std::size_t i = std::size_t(h) & mask_;; i = (i + 1) & mask_) {
            const std::uint64_t slot = table_[i];
            if (slot == kEmpty)
                return npos;
            if ((slot & kTagMask) == tag && equal(std::uint32_t(slot), key))
                return std::uint32_t(slot);
        }
    }
    bool contains(std::string_view key) const noexcept { return find(key) != npos; }

    /// Adds `key` if absent. Returns its index; `added` says whether it is new.
    std::uint32_t insert(std::string_view key, bool& added)
    {
        if (table_.empty())
            grow();
        const std::uint64_t h = hashBytes(key.data(), key.size());
        const std::uint64_t tag = h & kTagMask;
        std::size_t i = std::size_t(h) & mask_;
        for (;; i = (i + 1) & mask_) {
            const std::uint64_t slot = table_[i];
            if (slot == kEmpty)
                break;
            if ((slot & kTagMask) == tag && equal(std::uint32_t(slot), key)) {
                added = false;
                return std::uint32_t(slot);
            }
        }
        if (key.size() > 0xFFFFFFFFu - bytes_.size() || entries_.size() >= npos - 1)
            throw Error(ErrorCode::Runtime, "too many distinct keys in one statement");
        if (entries_.size() >= table_.size() - table_.size() / 4) {
            grow();
            i = std::size_t(h) & mask_;
            while (table_[i] != kEmpty)
                i = (i + 1) & mask_;
        }
        // Finish allocations before publishing a member or its byte range.
        if (entries_.size() == entries_.capacity())
            entries_.reserve(entries_.empty() ? 1 : entries_.capacity() * 2);
        const std::uint32_t at = std::uint32_t(entries_.size());
        const auto offset = std::uint32_t(bytes_.size());
        if (!key.empty())
            bytes_.insert(bytes_.end(), key.data(), key.data() + key.size());
        entries_.push_back(Entry{h, offset, std::uint32_t(key.size())});
        table_[i] = tag | at;
        added = true;
        return at;
    }
    /// True when `key` was not there before.
    bool insert(std::string_view key)
    {
        bool added = false;
        insert(key, added);
        return added;
    }

    /// Member `i`, valid until the next insert.
    std::string_view key(std::uint32_t i) const noexcept
    {
        const Entry& e = entries_[i];
        return e.size ? std::string_view(bytes_.data() + e.offset, e.size) : std::string_view{};
    }

    /// Empties the set, keeping its memory for the next run.
    void clear() noexcept
    {
        if (!entries_.empty())
            std::fill(table_.begin(), table_.end(), kEmpty);
        entries_.clear();
        bytes_.clear();
    }
    /// Frees everything once the set holds more than `bytes` of memory.
    void releaseAbove(std::size_t bytes)
    {
        if (memory() > bytes)
            *this = FlatKeySet();
    }
    std::size_t memory() const noexcept
    {
        return bytes_.capacity() + entries_.capacity() * sizeof(Entry) + table_.capacity() * sizeof(std::uint64_t);
    }

private:
    static constexpr std::uint64_t kTagMask = 0xFFFFFFFF00000000ull;
    static constexpr std::uint64_t kEmpty = ~std::uint64_t(0);

    struct Entry
    {
        std::uint64_t hash;
        std::uint32_t offset;
        std::uint32_t size;
    };

    bool equal(std::uint32_t at, std::string_view key) const noexcept
    {
        const Entry& e = entries_[at];
        if (e.size != key.size())
            return false;
        if (key.empty())
            return true;
        const char* a = bytes_.data() + e.offset;
        const char* b = key.data();
        std::size_t n = key.size();
        // Keys are short: compare a word at a time, inline.
        while (n >= 8) {
            std::uint64_t x, y;
            std::memcpy(&x, a, 8);
            std::memcpy(&y, b, 8);
            if (x != y)
                return false;
            a += 8;
            b += 8;
            n -= 8;
        }
        while (n--) {
            if (*a++ != *b++)
                return false;
        }
        return true;
    }

    void grow()
    {
        const std::size_t cap = table_.empty() ? 16 : table_.size() * 2;
        table_.assign(cap, kEmpty);
        mask_ = cap - 1;
        for (std::uint32_t at = 0; at < entries_.size(); ++at) {
            std::size_t i = std::size_t(entries_[at].hash) & mask_;
            while (table_[i] != kEmpty)
                i = (i + 1) & mask_;
            table_[i] = (entries_[at].hash & kTagMask) | at;
        }
    }

    std::vector<char> bytes_;
    std::vector<Entry> entries_;
    std::vector<std::uint64_t> table_;
    std::size_t mask_ = 0;
};

}  // namespace sql::internal
