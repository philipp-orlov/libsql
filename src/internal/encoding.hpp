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
// The one gap between the two is that key encoding sorts every INTEGER before
// every REAL, where sql::compare interleaves them numerically. Column values
// are cast to the column's declared type on the way in, so a stored column
// never mixes the two and the gap never shows.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

void appendKey(std::string& out, const Value& v);
std::string encodeKey(const Value& v);
std::string encodeKey(const std::vector<Value>& values);

/// Decodes one value off the front of `in` and consumes it.
Value decodeKey(std::string_view& in);
/// The same, into a cell that keeps its buffer.
void decodeKeyInto(std::string_view& in, Value& out);
/// Consumes `count` encoded values without materialising them.
void skipKeys(std::string_view& in, std::size_t count);

/// The smallest byte string that sorts after every key starting with `prefix`.
/// Empty when `prefix` is all 0xff, which means "no upper bound".
std::string successor(std::string_view prefix);
/// The same, into `out`, which keeps its buffer.
void successorInto(std::string& out, std::string_view prefix);

// ----------------------------------------------------------------- rows ---

std::string encodeRow(const std::vector<Value>& values);
/// Appends the encoding of `count` cells to `out`.
void encodeRowInto(std::string& out, const Value* values, std::size_t count);
std::vector<Value> decodeRow(std::string_view in);
/// Decodes into `width` cells that keep their buffers; columns the record
/// does not carry become NULL.
void decodeRowInto(std::string_view in, Value* out, std::size_t width);

// ------------------------------------------------------------ primitives --

void putVarint(std::string& out, std::uint64_t v);
std::uint64_t getVarint(std::string_view& in);
void putString(std::string& out, std::string_view s);
std::string getString(std::string_view& in);
void putValue(std::string& out, const Value& v);
Value getValue(std::string_view& in);
void getValueInto(std::string_view& in, Value& out);

}  // namespace sql::internal
