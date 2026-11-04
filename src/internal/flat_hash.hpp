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

#include "base/hash.hpp"
#include <cstring>
#include <string_view>
#include <vector>

#include "sql/error.hpp"

namespace sql::internal {

/// Working memory of the running statement, so one query cannot take the whole device. Charged where result rows and
/// key sets grow (RowStore, FlatKeySet, GroupTable); never given back during the statement, so the figure is a
/// conservative upper bound. Thread-local: a statement runs on one thread, and RowStore has no link to its connection.
struct StatementMemory
{
    std::uint64_t limit = 0;  ///< 0 = no limit
    std::uint64_t used = 0;
};
inline thread_local StatementMemory* t_statementMemory = nullptr;

inline void chargeStatement(std::size_t bytes)
{
    StatementMemory* m = t_statementMemory;
    if (m && m->limit && (m->used += bytes) > m->limit)
        throw Error(ErrorCode::Runtime, "statement memory limit exceeded (PRAGMA hard_heap_limit)");
}

/// The outermost statement owns the budget; a statement a trigger runs shares it.
struct StatementMemoryScope
{
    explicit StatementMemoryScope(std::uint64_t limit)
    {
        if (!t_statementMemory) {
            own.limit = limit;
            t_statementMemory = &own;
            owner = true;
        }
    }
    ~StatementMemoryScope()
    {
        if (owner)
            t_statementMemory = nullptr;
    }
    StatementMemoryScope(const StatementMemoryScope&) = delete;
    StatementMemoryScope& operator=(const StatementMemoryScope&) = delete;
    StatementMemory own;
    bool owner = false;
};

}  // namespace sql::internal

namespace sql::internal {

/// A fast 64-bit hash for short byte strings (keys are mostly < 32 bytes).
/// Fixed-size loads only: the tail is read with overlapping loads rather than
/// a variable-length copy, which would be a library call.
inline std::uint64_t hashBytes(const char* p, std::size_t n) noexcept
{
    return base::hashBytes64(p, n);   // moved unchanged to libcore's libbase
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
        chargeStatement(key.size() + sizeof(Entry) + 2 * sizeof(std::uint64_t));
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
