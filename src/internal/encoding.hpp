// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Turning values into bytes, twice over.
//
//   * key encoding is order preserving: memcmp on two encoded keys gives the
//     same answer as sql::compare on the values they came from, and encoded
//     values concatenate into composite keys without a separator
//   * row encoding is compact and self-describing, and makes no ordering
//     promise at all
//
// Integers and reals share one key tag and interleave numerically, so 1 and
// 1.0 are the same key, as SQLite says they are equal. Text keys can be
// collated (NOCASE folds, RTRIM strips) and any component can be descending,
// which complements its bytes.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "internal/types.hpp"
#include "nosql/slice.hpp"
#include "sql/value.hpp"

namespace sql::internal {

inline std::string_view viewOf(nosql::Slice s) noexcept
{
    return s.view();
}
inline nosql::Slice sliceOf(std::string_view s) noexcept
{
    return nosql::Slice(s.data(), s.size());
}

// ----------------------------------------------------------------- keys ---

void appendKey(std::string& out, const Value& v, Collation c = Collation::Binary,
               bool desc = false);
/// A rowid as the storage key of a rowid table: eight bytes, sign-flipped
/// big-endian, no tag.
void appendRowidKey(std::string& out, std::int64_t rowid);
[[noreturn]] void throwTruncatedKey();

/// The eight bytes at `p` as a big-endian number, so that numbers compare as
/// the bytes do under memcmp.
inline std::uint64_t loadBigEndian64(const char* p) noexcept
{
    std::uint64_t v;
    std::memcpy(&v, p, 8);
#if defined(_MSC_VER)
    v = _byteswap_uint64(v);
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

/// Reads the 8-byte big-endian, sign-flipped rowid at the front of `in`.
inline std::int64_t readRowidKey(std::string_view& in)
{
    if (in.size() < 8)
        throwTruncatedKey();
    std::uint64_t v;
    std::memcpy(&v, in.data(), 8);
#if defined(_MSC_VER)
    v = _byteswap_uint64(v);
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    in.remove_prefix(8);
    return std::int64_t(v ^ 0x8000000000000000ull);
}
std::string encodeKey(const Value& v);
std::string encodeKey(const std::vector<Value>& values);

/// Decodes one value off the front of `in` and consumes it.
Value decodeKey(std::string_view& in);
/// The same, into a cell that keeps its buffer. With `borrow`, text and blobs
/// stored without escapes point into `in` instead of being copied.
void decodeKeyInto(std::string_view& in, Value& out, bool borrow = false);
/// Consumes `count` encoded values without materialising them.
void skipKeys(std::string_view& in, std::size_t count);

/// The smallest byte string that sorts after every key starting with `prefix`.
/// Empty when `prefix` is all 0xff, which means "no upper bound".
std::string successor(std::string_view prefix);
/// The same, into `out`, which keeps its buffer.
void successorInto(std::string& out, std::string_view prefix);

// ----------------------------------------------------------------- rows ---

std::string encodeRow(const std::vector<Value>& values);
/// Appends the encoding of `count` cells to `out`; cells whose `asNull` byte
/// is nonzero are stored as NULL whatever they hold.
void encodeRowInto(std::string& out, const Value* values, std::size_t count,
                   const std::uint8_t* asNull = nullptr);
std::vector<Value> decodeRow(std::string_view in);
/// Decodes into `width` cells that keep their buffers; columns the record
/// does not carry become NULL. Returns how many the record carried (at most
/// `width`), so a caller can supply defaults for columns added later. With
/// `borrow`, text and blobs point into `in` instead of being copied.
std::size_t decodeRowInto(std::string_view in, Value* out, std::size_t width, bool borrow = false);
/// The same, decoding only the cells whose `used` byte is nonzero; the others
/// are stepped over and left as they were.
std::size_t decodeRowInto(std::string_view in, Value* out, std::size_t width,
                          const std::uint8_t* used, bool fillMissing = true, bool borrow = false);
/// How many cells a record holds.
std::size_t rowColumnCount(std::string_view in);
/// Cell `i` of a record without decoding the rest: 1 an integer (in `n`), 2 a
/// real (in `r`), 0 NULL, -1 anything else or a record that ends before it.
int storedNumber(std::string_view in, std::size_t i, std::int64_t& n, double& r);

// ------------------------------------------------------------ primitives --

void putVarint(std::string& out, std::uint64_t v);
std::uint64_t getVarint(std::string_view& in);
void putString(std::string& out, std::string_view s);
std::string getString(std::string_view& in);
void putValue(std::string& out, const Value& v);
Value getValue(std::string_view& in);
void getValueInto(std::string_view& in, Value& out);

}  // namespace sql::internal
