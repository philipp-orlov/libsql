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

/// Forces the few functions on the per-row path into their one caller: a row
/// costs a call's worth of saved registers for each of them otherwise.
#if defined(__GNUC__) || defined(__clang__)
#define SQL_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define SQL_ALWAYS_INLINE __forceinline
#else
#define SQL_ALWAYS_INLINE inline
#endif

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
[[noreturn]] void throwMalformed(const char* what);

// ------------------------------------------------------- inline decoding --
//
// What a scan does to every row, kept in the header so that it compiles into
// the loop that reads the row. The formats are described in encoding.cpp.

namespace record {

inline constexpr std::uint8_t kIntWidth[7] = {0, 1, 2, 3, 4, 6, 8};

SQL_ALWAYS_INLINE std::size_t payloadSize(std::uint64_t code)
{
    // Codes 0-11: NULL, six integer widths, REAL, 0, 1, DATETIME, unknown.
    static constexpr std::uint8_t kSize[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 8, 0xff};
    if (code >= 12)
        return std::size_t((code - 12) >> 1);
    if (code == 11)
        throwMalformed("unknown record type code");
    return kSize[code];
}

/// Little-endian, sign-extended from `width` bytes.
SQL_ALWAYS_INLINE std::int64_t readInt(const char* p, unsigned width) noexcept
{
    std::uint64_t u = 0;
    switch (width) {
        case 1: return std::int8_t(p[0]);
        case 2: {
            std::uint16_t x;
            std::memcpy(&x, p, 2);
            return std::int16_t(x);
        }
        case 4: {
            std::uint32_t x;
            std::memcpy(&x, p, 4);
            return std::int32_t(x);
        }
        case 8: std::memcpy(&u, p, 8); return std::int64_t(u);
        default: break;
    }
    for (unsigned i = 0; i < width; ++i)
        u |= std::uint64_t(static_cast<unsigned char>(p[i])) << (8 * i);
    const unsigned shift = 64 - 8 * width;
    return std::int64_t(u << shift) >> shift;
}

/// Decodes the payload of type `code` at `p` (already bounds-checked).
SQL_ALWAYS_INLINE void decodeCell(std::uint64_t code, const char* p, std::size_t size, Value& out, bool borrow)
{
    if (code >= 12) {
        if (borrow) {
            if (code & 1)
                out.setTextRef(std::string_view(p, size));
            else
                out.setBlobRef(p, size);
        } else if (code & 1) {
            out.setText(std::string_view(p, size));
        } else {
            out.setBlob(p, size);
        }
        return;
    }
    switch (code) {
        case 0: out.setNull(); return;
        case 7: {
            double d;
            std::memcpy(&d, p, 8);
            out.setReal(d);
            return;
        }
        case 8: out.setInteger(0); return;
        case 9: out.setInteger(1); return;
        case 10: out.setDatetime(Datetime{readInt(p, 8)}); return;
        default: out.setInteger(readInt(p, kIntWidth[code])); return;
    }
}

/// Reads a varint from [p, end); fast path for one byte.
SQL_ALWAYS_INLINE std::uint64_t headerVarint(const char*& p, const char* end)
{
    if (p < end && !(static_cast<unsigned char>(*p) & 0x80))
        return static_cast<unsigned char>(*p++);
    std::string_view in(p, std::size_t(end - p));
    const std::uint64_t v = getVarint(in);
    p = in.data();
    return v;
}

/// Splits a record into its type codes and its payloads.
struct Cursor
{
    const char* code;      ///< next type code
    const char* codeEnd;
    const char* body;      ///< next payload
    const char* bodyEnd;
    std::size_t count;

    SQL_ALWAYS_INLINE explicit Cursor(std::string_view in)
    {
        const char* p = in.data();
        const char* end = p + in.size();
        const std::uint64_t n = headerVarint(p, end);
        const std::uint64_t hdr = headerVarint(p, end);
        if (hdr > std::size_t(end - p) || n > hdr)
            throwMalformed("implausible record header");
        count = std::size_t(n);
        code = p;
        codeEnd = code + hdr;
        body = codeEnd;
        bodyEnd = end;
    }
    /// The next cell's type code and payload; advances past it.
    SQL_ALWAYS_INLINE std::uint64_t next(const char*& payload, std::size_t& size)
    {
        const std::uint64_t c = headerVarint(code, codeEnd);
        size = payloadSize(c);
        if (size > std::size_t(bodyEnd - body))
            throwMalformed("truncated record");
        payload = body;
        body += size;
        return c;
    }
};

}  // namespace record

/// decodeRowInto with a `used` mask, inline.
SQL_ALWAYS_INLINE std::size_t decodeRowUsed(std::string_view in, Value* out, std::size_t width,
                                            const std::uint8_t* used, bool fillMissing, bool borrow)
{
    record::Cursor r(in);
    const std::size_t have = r.count < width ? r.count : width;
    for (std::size_t i = 0; i < have; ++i) {
        const char* p;
        std::size_t size;
        const std::uint64_t c = r.next(p, size);
        if (used[i])
            record::decodeCell(c, p, size, out[i], borrow);
    }
    if (fillMissing) {
        for (std::size_t i = have; i < width; ++i)
            out[i].setNull();
    }
    return have;
}

/// skipKeys(in, 1), with a number or a NULL stepped over inline.
SQL_ALWAYS_INLINE void skipKeyFast(std::string_view& in)
{
    if (in.size() >= 11 && in[0] == '\x02')
        in.remove_prefix(11);
    else if (!in.empty() && in[0] == '\x01')
        in.remove_prefix(1);
    else
        skipKeys(in, 1);
}

/// decodeKeyInto, with the two forms nearly every key part has decoded
/// inline: a number that is an integer, and text or a blob holding no zero
/// byte. Anything else goes the general way.
SQL_ALWAYS_INLINE void decodeKeyFast(std::string_view& in, Value& out, bool borrow)
{
    const std::size_t n = in.size();
    if (n >= 11 && in[0] == '\x02') {
        // The number tag: a sortable double, then what an integer adds to it.
        std::uint64_t bits = loadBigEndian64(in.data() + 1);
        bits = (bits >> 63) ? (bits & 0x7fffffffffffffffull) : ~bits;
        double d;
        std::memcpy(&d, &bits, sizeof d);
        if (in[9] == 0 && in[10] == 0 && d > -9.0e18 && d < 9.0e18) {
            const std::int64_t i = std::int64_t(d);
            if (double(i) == d) {
                in.remove_prefix(11);
                out.setInteger(i);
                return;
            }
        }
    } else if (n >= 3 && (in[0] == '\x05' || in[0] == '\x06')) {
        // Text or a blob: bytes up to 00 00; an escaped zero is 00 ff.
        const char* begin = in.data() + 1;
        const char* z = static_cast<const char*>(std::memchr(begin, 0, n - 1));
        if (z && z + 1 < in.data() + n && z[1] == '\0') {
            const std::size_t len = std::size_t(z - begin);
            if (!borrow) {
                if (in[0] == '\x05')
                    out.setText(std::string_view(begin, len));
                else
                    out.setBlob(begin, len);
            } else if (in[0] == '\x05') {
                out.setTextRef(std::string_view(begin, len));
            } else {
                out.setBlobRef(begin, len);
            }
            in.remove_prefix(len + 3);
            return;
        }
    }
    decodeKeyInto(in, out, borrow);
}
void putString(std::string& out, std::string_view s);
std::string getString(std::string_view& in);
void putValue(std::string& out, const Value& v);
Value getValue(std::string_view& in);
void getValueInto(std::string_view& in, Value& out);

}  // namespace sql::internal
