// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Value serialisation. See encoding.hpp for what each form promises.

#include "internal/encoding.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace sql::internal {

namespace {

/// Key tags, ordered to match the cross-class order of compareValues. One tag
/// serves integers and reals alike, so that 1 and 1.0 encode identically, as
/// SQLite says they are equal. A descending component is the bitwise
/// complement of its ascending form, so its tag lies in 0xf9..0xfe.
enum : unsigned char
{
    kKeyNull = 0x01,
    kKeyNumber = 0x02,
    kKeyDatetime = 0x04,
    kKeyText = 0x05,
    kKeyBlob = 0x06,
};

[[noreturn]] void corrupt(const char* what)
{
    throw Error(ErrorCode::Internal, std::string("malformed record: ") + what);
}

void appendBigEndian(std::string& out, std::uint64_t v)
{
    char b[8];
    for (int i = 7; i >= 0; --i) {
        b[i] = char(std::uint8_t(v));
        v >>= 8;
    }
    out.append(b, 8);
}

std::uint64_t readBigEndian(std::string_view& in)
{
    if (in.size() < 8)
        corrupt("truncated fixed-width key field");
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | std::uint8_t(in[std::size_t(i)]);
    in.remove_prefix(8);
    return v;
}

/// Flipping the sign bit makes two's-complement integers sort as unsigned.
constexpr std::uint64_t kSignBit = 0x8000000000000000ull;

std::uint64_t sortableFromDouble(double d) noexcept
{
    if (d == 0.0)
        d = 0.0;  // -0.0 and +0.0 are the same key
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof bits);
    // Negatives run backwards in IEEE-754 order, so invert them whole.
    return (bits & kSignBit) ? ~bits : (bits | kSignBit);
}

double doubleFromSortable(std::uint64_t bits) noexcept
{
    bits = (bits & kSignBit) ? (bits & ~kSignBit) : ~bits;
    double d = 0;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

/// An integer as (d, tail): d the largest double <= i, tail = i - d, which is
/// below 2048 because doubles near 2^63 are 2048 apart. A real is (r, 0).
/// Comparing (d, tail) pairs orders integers and reals together exactly.
void appendNumber(std::string& out, std::int64_t i)
{
    double d = double(i);
    if (compareIntReal(i, d) < 0)
        d = std::nextafter(d, -1e300);
    const std::uint64_t tail = std::uint64_t(i) - std::uint64_t(std::int64_t(d));
    out.push_back(char(kKeyNumber));
    appendBigEndian(out, sortableFromDouble(d));
    out.push_back(char(std::uint8_t(tail >> 8)));
    out.push_back(char(std::uint8_t(tail)));
}

void appendNumber(std::string& out, double r)
{
    out.push_back(char(kKeyNumber));
    appendBigEndian(out, sortableFromDouble(r));
    out.push_back('\0');
    out.push_back('\0');
}

/// 0x00 is the terminator, so a literal zero byte is escaped as 00 ff and the
/// run ends with 00 00. Order survives because ff outranks every 00-prefixed
/// continuation and 00 is below every ordinary byte.
void appendBytes(std::string& out, const char* data, std::size_t size)
{
    std::size_t run = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\0') {
            out.append(data + run, i + 1 - run);
            out.push_back('\xff');
            run = i + 1;
        }
    }
    out.append(data + run, size - run);
    out.push_back('\0');
    out.push_back('\0');
}

void appendText(std::string& out, std::string_view s, Collation c)
{
    switch (c) {
        case Collation::Binary: appendBytes(out, s.data(), s.size()); return;
        case Collation::RTrim:
            while (!s.empty() && s.back() == ' ')
                s.remove_suffix(1);
            appendBytes(out, s.data(), s.size());
            return;
        case Collation::NoCase: {
            const std::size_t at = out.size();
            appendBytes(out, s.data(), s.size());
            for (std::size_t i = at; i < out.size(); ++i)
                out[i] = foldAscii(out[i]);
            return;
        }
    }
}

/// Unescapes a terminated run into `out`, which keeps its buffer.
/// A run with no escaped zero (the usual case) read in place: true and
/// `run` set, `in` advanced past the terminator.
bool plainBytes(std::string_view& in, std::string_view& run)
{
    const void* z = std::memchr(in.data(), 0, in.size());
    if (!z)
        corrupt("unterminated key field");
    const std::size_t i = std::size_t(static_cast<const char*>(z) - in.data());
    if (i + 1 >= in.size())
        corrupt("unterminated key field");
    if (in[i + 1] != '\0')
        return false;
    run = in.substr(0, i);
    in.remove_prefix(i + 2);
    return true;
}

void readBytesInto(std::string_view& in, std::string& out)
{
    out.clear();
    std::string_view run;
    if (plainBytes(in, run)) {
        out.assign(run.data(), run.size());
        return;
    }
    for (std::size_t i = 0;; ++i) {
        if (i >= in.size())
            corrupt("unterminated key field");
        if (in[i] != '\0') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 1 >= in.size())
            corrupt("unterminated key field");
        if (in[i + 1] == '\xff') {
            out.push_back('\0');
            ++i;
            continue;
        }
        if (in[i + 1] != '\0')
            corrupt("bad escape in key field");
        in.remove_prefix(i + 2);
        return;
    }
}

/// A thread's decode scratch keeps an ordinary key's worth, not the largest
/// key the thread ever decoded.
void trimScratch(std::string& scratch)
{
    if (scratch.capacity() > (64u << 10))
        std::string().swap(scratch);
}

void skipBytes(std::string_view& in)
{
    for (std::size_t i = 0;; ++i) {
        const std::size_t z = in.find('\0', i);
        if (z == std::string_view::npos || z + 1 >= in.size())
            corrupt("unterminated key field");
        if (in[z + 1] == '\0') {
            in.remove_prefix(z + 2);
            return;
        }
        i = z + 1;
    }
}

std::uint64_t zigzag(std::int64_t v) noexcept
{
    return (std::uint64_t(v) << 1) ^ std::uint64_t(v >> 63);
}

std::int64_t unzigzag(std::uint64_t v) noexcept
{
    return std::int64_t(v >> 1) ^ -std::int64_t(v & 1);
}

/// Length of the encoded ascending component at the front of `in`.
std::size_t componentLength(std::string_view in)
{
    if (in.empty())
        corrupt("empty key");
    std::string_view rest = in.substr(1);
    switch (static_cast<unsigned char>(in[0])) {
        case kKeyNull: return 1;
        case kKeyNumber:
            if (in.size() < 11)
                corrupt("truncated number key");
            return 11;
        case kKeyDatetime:
            if (in.size() < 9)
                corrupt("truncated datetime key");
            return 9;
        case kKeyText:
        case kKeyBlob: skipBytes(rest); return in.size() - rest.size();
        default: corrupt("unknown key tag");
    }
}

/// The same for a descending (complemented) component.
std::size_t descComponentLength(std::string_view in)
{
    if (in.empty())
        corrupt("empty key");
    const unsigned char tag = static_cast<unsigned char>(~static_cast<unsigned char>(in[0]));
    switch (tag) {
        case kKeyNull: return 1;
        case kKeyNumber:
            if (in.size() < 11)
                corrupt("truncated number key");
            return 11;
        case kKeyDatetime:
            if (in.size() < 9)
                corrupt("truncated datetime key");
            return 9;
        case kKeyText:
        case kKeyBlob:
            // Complemented terminator is ff ff; complemented escape is ff 00.
            for (std::size_t i = 1; i + 1 < in.size(); ++i) {
                if (static_cast<unsigned char>(in[i]) == 0xff) {
                    const unsigned char next = static_cast<unsigned char>(in[i + 1]);
                    if (next == 0xff)
                        return i + 2;
                    if (next != 0x00)
                        corrupt("bad escape in key field");
                    ++i;
                }
            }
            corrupt("unterminated key field");
        default: corrupt("unknown key tag");
    }
}

}  // namespace

// ----------------------------------------------------------------- keys ---

void appendKey(std::string& out, const Value& v, Collation c, bool desc)
{
    const std::size_t at = out.size();
    switch (v.type()) {
        case Type::Null: out.push_back(char(kKeyNull)); break;
        case Type::Integer: appendNumber(out, v.integer()); break;
        case Type::Real: appendNumber(out, v.real()); break;
        case Type::Datetime:
            out.push_back(char(kKeyDatetime));
            appendBigEndian(out, std::uint64_t(v.datetime().usec) ^ kSignBit);
            break;
        case Type::Text:
            out.push_back(char(kKeyText));
            appendText(out, v.text(), c);
            break;
        case Type::Blob: {
            out.push_back(char(kKeyBlob));
            const BlobView b = v.blob();
            appendBytes(out, reinterpret_cast<const char*>(b.data()), b.size());
            break;
        }
    }
    if (desc) {
        for (std::size_t i = at; i < out.size(); ++i)
            out[i] = char(~static_cast<unsigned char>(out[i]));
    }
}

std::string encodeKey(const Value& v)
{
    std::string out;
    appendKey(out, v);
    return out;
}

std::string encodeKey(const std::vector<Value>& values)
{
    std::string out;
    for (const Value& v : values)
        appendKey(out, v);
    return out;
}

void appendRowidKey(std::string& out, std::int64_t rowid)
{
    appendBigEndian(out, std::uint64_t(rowid) ^ kSignBit);
}

void throwTruncatedKey()
{
    corrupt("truncated fixed-width key field");
}

void decodeKeyInto(std::string_view& in, Value& out, bool borrow)
{
    if (in.empty())
        corrupt("empty key");
    const auto tag = static_cast<unsigned char>(in[0]);
    if (tag >= 0xf9) {
        // Descending: undo the complement on a copy, then decode that.
        const std::size_t n = descComponentLength(in);
        thread_local std::string flipped;
        flipped.assign(in.data(), n);
        for (char& ch : flipped)
            ch = char(~static_cast<unsigned char>(ch));
        std::string_view view = flipped;
        in.remove_prefix(n);
        decodeKeyInto(view, out);
        trimScratch(flipped);
        return;
    }
    in.remove_prefix(1);
    switch (tag) {
        case kKeyNull: out.setNull(); return;
        case kKeyNumber: {
            const double d = doubleFromSortable(readBigEndian(in));
            if (in.size() < 2)
                corrupt("truncated number key");
            const unsigned tail = (unsigned(std::uint8_t(in[0])) << 8) | std::uint8_t(in[1]);
            in.remove_prefix(2);
            std::int64_t i = 0;
            if (tail != 0)
                out.setInteger(std::int64_t(d) + std::int64_t(tail));
            else if (realIsExactInt(d, i))
                out.setInteger(i);
            else if (d == -9223372036854775808.0)
                out.setInteger(std::numeric_limits<std::int64_t>::min());
            else
                out.setReal(d);
            return;
        }
        case kKeyDatetime:
            out.setDatetime(Datetime{std::int64_t(readBigEndian(in) ^ kSignBit)});
            return;
        case kKeyText: {
            std::string_view run;
            if (plainBytes(in, run)) {
                if (borrow)
                    out.setTextRef(run);
                else
                    out.setText(run);
                return;
            }
            thread_local std::string buffer;
            readBytesInto(in, buffer);
            out.setText(buffer);
            trimScratch(buffer);
            return;
        }
        case kKeyBlob: {
            std::string_view run;
            if (plainBytes(in, run)) {
                if (borrow)
                    out.setBlobRef(run.data(), run.size());
                else
                    out.setBlob(run.data(), run.size());
                return;
            }
            thread_local std::string buffer;
            readBytesInto(in, buffer);
            out.setBlob(buffer.data(), buffer.size());
            trimScratch(buffer);
            return;
        }
        default: corrupt("unknown key tag");
    }
}

Value decodeKey(std::string_view& in)
{
    Value v;
    decodeKeyInto(in, v);
    return v;
}

void skipKeys(std::string_view& in, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (in.empty())
            corrupt("empty key");
        const std::size_t n = static_cast<unsigned char>(in[0]) >= 0xf9 ? descComponentLength(in)
                                                                          : componentLength(in);
        in.remove_prefix(n);
    }
}

void successorInto(std::string& out, std::string_view prefix)
{
    out.assign(prefix.data(), prefix.size());
    while (!out.empty()) {
        auto& last = reinterpret_cast<unsigned char&>(out.back());
        if (last != 0xff) {
            ++last;
            return;
        }
        out.pop_back();
    }
}

std::string successor(std::string_view prefix)
{
    std::string out;
    successorInto(out, prefix);
    return out;
}

// ----------------------------------------------------------------- rows ---

void putVarint(std::string& out, std::uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(char(std::uint8_t(v) | 0x80));
        v >>= 7;
    }
    out.push_back(char(std::uint8_t(v)));
}

std::uint64_t getVarint(std::string_view& in)
{
    std::uint64_t v = 0;
    for (unsigned shift = 0; shift <= 63; shift += 7) {
        if (in.empty())
            corrupt("truncated varint");
        const auto byte = static_cast<unsigned char>(in.front());
        in.remove_prefix(1);
        v |= std::uint64_t(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return v;
    }
    corrupt("overlong varint");
}

void putString(std::string& out, std::string_view s)
{
    putVarint(out, s.size());
    out.append(s);
}

std::string getString(std::string_view& in)
{
    const std::uint64_t n = getVarint(in);
    if (n > in.size())
        corrupt("truncated string");
    std::string out(in.substr(0, std::size_t(n)));
    in.remove_prefix(std::size_t(n));
    return out;
}

void putValue(std::string& out, const Value& v)
{
    out.push_back(char(std::uint8_t(v.type())));
    switch (v.type()) {
        case Type::Null: break;
        case Type::Integer: putVarint(out, zigzag(v.integer())); break;
        case Type::Datetime: putVarint(out, zigzag(v.datetime().usec)); break;
        case Type::Real: {
            const double d = v.real();
            char raw[sizeof d];
            std::memcpy(raw, &d, sizeof d);
            out.append(raw, sizeof raw);
            break;
        }
        case Type::Text: putString(out, v.text()); break;
        case Type::Blob: {
            const BlobView b = v.blob();
            putString(out, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
            break;
        }
    }
}

Value getValue(std::string_view& in)
{
    if (in.empty())
        corrupt("truncated row");
    const auto tag = static_cast<unsigned char>(in.front());
    in.remove_prefix(1);
    switch (Type(tag)) {
        case Type::Null: return Value();
        case Type::Integer: return Value(unzigzag(getVarint(in)));
        case Type::Datetime: return Value(Datetime{unzigzag(getVarint(in))});
        case Type::Real: {
            if (in.size() < sizeof(double))
                corrupt("truncated real");
            double d = 0;
            std::memcpy(&d, in.data(), sizeof d);
            in.remove_prefix(sizeof d);
            return Value(d);
        }
        case Type::Text: return Value(getString(in));
        case Type::Blob: {
            const std::string bytes = getString(in);
            return Value::blob(bytes.data(), bytes.size());
        }
    }
    corrupt("unknown value tag");
}

void getValueInto(std::string_view& in, Value& out)
{
    if (in.empty())
        corrupt("truncated row");
    const auto tag = static_cast<unsigned char>(in.front());
    in.remove_prefix(1);
    switch (Type(tag)) {
        case Type::Null: out.setNull(); return;
        case Type::Integer: out.setInteger(unzigzag(getVarint(in))); return;
        case Type::Datetime: out.setDatetime(Datetime{unzigzag(getVarint(in))}); return;
        case Type::Real: {
            if (in.size() < sizeof(double))
                corrupt("truncated real");
            double d = 0;
            std::memcpy(&d, in.data(), sizeof d);
            in.remove_prefix(sizeof d);
            out.setReal(d);
            return;
        }
        case Type::Text: {
            const std::uint64_t n = getVarint(in);
            if (n > in.size())
                corrupt("truncated string");
            out.setText(in.substr(0, std::size_t(n)));
            in.remove_prefix(std::size_t(n));
            return;
        }
        case Type::Blob: {
            const std::uint64_t n = getVarint(in);
            if (n > in.size())
                corrupt("truncated string");
            out.setBlob(in.data(), std::size_t(n));
            in.remove_prefix(std::size_t(n));
            return;
        }
    }
    corrupt("unknown value tag");
}

// Row records (docs/design.md §6.5):
//
//   varint count, varint header bytes, count varint type codes, payloads
//
// Type codes: 0 NULL; 1-6 INTEGER in 1, 2, 3, 4, 6 or 8 little-endian bytes;
// 7 REAL (8 bytes); 8 and 9 the integers 0 and 1 (no payload); 10 DATETIME
// (8 bytes of microseconds); even N >= 12 a BLOB of (N - 12) / 2 bytes; odd
// N >= 13 TEXT of (N - 13) / 2 bytes. A reader finds column k by walking the
// codes alone, and knowing where the payloads start lets it walk codes and
// payloads together in one pass.

namespace {

constexpr std::uint8_t kIntWidth[7] = {0, 1, 2, 3, 4, 6, 8};

std::uint64_t typeCode(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Null: return 0;
        case Type::Integer: {
            const std::int64_t i = v.integer();
            if (i == 0)
                return 8;
            if (i == 1)
                return 9;
            if (i >= -128 && i <= 127)
                return 1;
            if (i >= -32768 && i <= 32767)
                return 2;
            if (i >= -8388608 && i <= 8388607)
                return 3;
            if (i >= INT32_MIN && i <= INT32_MAX)
                return 4;
            if (i >= -(std::int64_t(1) << 47) && i < (std::int64_t(1) << 47))
                return 5;
            return 6;
        }
        case Type::Real: return 7;
        case Type::Datetime: return 10;
        case Type::Text: return 13 + 2 * std::uint64_t(v.text().size());
        case Type::Blob: return 12 + 2 * std::uint64_t(v.blob().size());
    }
    return 0;
}

inline std::size_t payloadSize(std::uint64_t code)
{
    // Codes 0-11: NULL, six integer widths, REAL, 0, 1, DATETIME, unknown.
    static constexpr std::uint8_t kSize[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 8, 0xff};
    if (code >= 12)
        return std::size_t((code - 12) >> 1);
    if (code == 11)
        corrupt("unknown record type code");
    return kSize[code];
}

void putPayload(std::string& out, const Value& v, std::uint64_t code)
{
    if (code >= 1 && code <= 6) {
        const std::uint64_t u = std::uint64_t(v.integer());
        char b[8];
        for (int i = 0; i < 8; ++i)
            b[i] = char(std::uint8_t(u >> (8 * i)));
        out.append(b, kIntWidth[code]);
        return;
    }
    switch (code) {
        case 7: {
            const double d = v.real();
            char b[8];
            std::memcpy(b, &d, 8);
            out.append(b, 8);
            return;
        }
        case 10: {
            const std::uint64_t u = std::uint64_t(v.datetime().usec);
            char b[8];
            for (int i = 0; i < 8; ++i)
                b[i] = char(std::uint8_t(u >> (8 * i)));
            out.append(b, 8);
            return;
        }
        default: break;
    }
    if (code >= 13 && (code & 1))
        out.append(v.text());
    else if (code >= 12)
        out.append(reinterpret_cast<const char*>(v.blob().data()), v.blob().size());
}

/// Little-endian, sign-extended from `width` bytes.
inline std::int64_t readInt(const char* p, unsigned width) noexcept
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
inline void decodeCell(std::uint64_t code, const char* p, std::size_t size, Value& out, bool borrow)
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
inline std::uint64_t headerVarint(const char*& p, const char* end)
{
    if (p < end && !(static_cast<unsigned char>(*p) & 0x80))
        return static_cast<unsigned char>(*p++);
    std::string_view in(p, std::size_t(end - p));
    const std::uint64_t v = getVarint(in);
    p = in.data();
    return v;
}

/// Splits a record into its type codes and its payloads.
struct RecordCursor
{
    const char* code;      ///< next type code
    const char* codeEnd;
    const char* body;      ///< next payload
    const char* bodyEnd;
    std::size_t count;

    explicit RecordCursor(std::string_view in)
    {
        const char* p = in.data();
        const char* end = p + in.size();
        const std::uint64_t n = headerVarint(p, end);
        const std::uint64_t hdr = headerVarint(p, end);
        if (hdr > std::size_t(end - p) || n > hdr)
            corrupt("implausible record header");
        count = std::size_t(n);
        code = p;
        codeEnd = code + hdr;
        body = codeEnd;
        bodyEnd = end;
    }
    /// The next cell's type code and payload; advances past it.
    std::uint64_t next(const char*& payload, std::size_t& size)
    {
        const std::uint64_t c = headerVarint(code, codeEnd);
        size = payloadSize(c);
        if (size > std::size_t(bodyEnd - body))
            corrupt("truncated record");
        payload = body;
        body += size;
        return c;
    }
};

}  // namespace

std::string encodeRow(const std::vector<Value>& values)
{
    std::string out;
    encodeRowInto(out, values.data(), values.size());
    return out;
}

void encodeRowInto(std::string& out, const Value* values, std::size_t count, const std::uint8_t* asNull)
{
    auto code = [&](std::size_t i) { return asNull && asNull[i] ? std::uint64_t(0) : typeCode(values[i]); };
    putVarint(out, count);
    // Header length first: codes are mostly one byte, so measure them.
    std::size_t hdr = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t c = code(i);
        do {
            ++hdr;
            c >>= 7;
        } while (c);
    }
    putVarint(out, hdr);
    for (std::size_t i = 0; i < count; ++i)
        putVarint(out, code(i));
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t c = code(i);
        if (c != 0 && c != 8 && c != 9)
            putPayload(out, values[i], c);
    }
}

std::size_t decodeRowInto(std::string_view in, Value* out, std::size_t width, bool borrow)
{
    RecordCursor r(in);
    const std::size_t have = std::min(r.count, width);
    for (std::size_t i = 0; i < have; ++i) {
        const char* p;
        std::size_t size;
        const std::uint64_t c = r.next(p, size);
        decodeCell(c, p, size, out[i], borrow);
    }
    for (std::size_t i = have; i < width; ++i)
        out[i].setNull();
    return have;
}

std::size_t decodeRowInto(std::string_view in, Value* out, std::size_t width,
                          const std::uint8_t* used, bool fillMissing, bool borrow)
{
    RecordCursor r(in);
    const std::size_t have = std::min(r.count, width);
    for (std::size_t i = 0; i < have; ++i) {
        const char* p;
        std::size_t size;
        const std::uint64_t c = r.next(p, size);
        if (used[i])
            decodeCell(c, p, size, out[i], borrow);
    }
    if (fillMissing) {
        for (std::size_t i = have; i < width; ++i)
            out[i].setNull();
    }
    return have;
}

std::size_t rowColumnCount(std::string_view in)
{
    return std::size_t(getVarint(in));
}

int storedNumber(std::string_view in, std::size_t i, std::int64_t& n, double& r)
{
    RecordCursor rec(in);
    if (i >= rec.count)
        return -1;
    const char* p;
    std::size_t size;
    for (std::size_t k = 0; k < i; ++k)
        rec.next(p, size);
    const std::uint64_t c = rec.next(p, size);
    switch (c) {
        case 0: return 0;
        case 7: std::memcpy(&r, p, 8); return 2;
        case 8: n = 0; return 1;
        case 9: n = 1; return 1;
        default:
            if (c > 6)
                return -1;
            n = readInt(p, kIntWidth[c]);
            return 1;
    }
}

std::vector<Value> decodeRow(std::string_view in)
{
    RecordCursor r(in);
    std::vector<Value> out(r.count);
    for (std::size_t i = 0; i < r.count; ++i) {
        const char* p;
        std::size_t size;
        const std::uint64_t c = r.next(p, size);
        decodeCell(c, p, size, out[i], false);
    }
    return out;
}

}  // namespace sql::internal
