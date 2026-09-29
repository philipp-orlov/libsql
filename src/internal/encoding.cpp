// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Value serialisation. See encoding.hpp for what each form promises.

#include "internal/encoding.hpp"

#include <algorithm>
#include <cstring>

namespace sql::internal {

namespace {

/// Key tags, ordered to match sql::compare's ranking of the storage classes.
enum : unsigned char
{
    kKeyNull = 0x01,
    kKeyInteger = 0x02,
    kKeyReal = 0x03,
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
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(char(std::uint8_t(v >> shift)));
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

/// 0x00 is the terminator, so a literal zero byte is escaped as 00 ff and the
/// run ends with 00 00. Order survives because ff outranks every 00-prefixed
/// continuation and 00 is below every ordinary byte.
void appendBytes(std::string& out, const char* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(data[i]);
        if (data[i] == '\0')
            out.push_back('\xff');
    }
    out.push_back('\0');
    out.push_back('\0');
}

/// Unescapes a terminated run into `out`, which keeps its buffer.
void readBytesInto(std::string_view& in, std::string& out)
{
    out.clear();
    // The common case has no escapes: find the terminator and copy once.
    const std::size_t zero = in.find('\0');
    if (zero == std::string_view::npos)
        corrupt("unterminated key field");
    if (zero + 1 < in.size() && in[zero + 1] == '\0') {
        out.append(in.data(), zero);
        in.remove_prefix(zero + 2);
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

std::string readBytes(std::string_view& in)
{
    std::string out;
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
        return out;
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

}  // namespace

// ----------------------------------------------------------------- keys ---

void appendKey(std::string& out, const Value& v)
{
    switch (v.type()) {
        case Type::Null: out.push_back(char(kKeyNull)); break;
        case Type::Integer:
            out.push_back(char(kKeyInteger));
            appendBigEndian(out, std::uint64_t(v.integer()) ^ kSignBit);
            break;
        case Type::Real:
            out.push_back(char(kKeyReal));
            appendBigEndian(out, sortableFromDouble(v.real()));
            break;
        case Type::Datetime:
            out.push_back(char(kKeyDatetime));
            appendBigEndian(out, std::uint64_t(v.datetime().usec) ^ kSignBit);
            break;
        case Type::Text: {
            out.push_back(char(kKeyText));
            const std::string& s = v.text();
            appendBytes(out, s.data(), s.size());
            break;
        }
        case Type::Blob: {
            out.push_back(char(kKeyBlob));
            const Blob& b = v.blob();
            appendBytes(out, reinterpret_cast<const char*>(b.data()), b.size());
            break;
        }
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

Value decodeKey(std::string_view& in)
{
    if (in.empty())
        corrupt("empty key");
    const auto tag = static_cast<unsigned char>(in[0]);
    in.remove_prefix(1);
    switch (tag) {
        case kKeyNull: return Value();
        case kKeyInteger: return Value(std::int64_t(readBigEndian(in) ^ kSignBit));
        case kKeyReal: return Value(doubleFromSortable(readBigEndian(in)));
        case kKeyDatetime:
            return Value(Datetime{std::int64_t(readBigEndian(in) ^ kSignBit)});
        case kKeyText: return Value(readBytes(in));
        case kKeyBlob: {
            const std::string bytes = readBytes(in);
            return Value::blob(bytes.data(), bytes.size());
        }
        default: corrupt("unknown key tag");
    }
}

void decodeKeyInto(std::string_view& in, Value& out)
{
    if (in.empty())
        corrupt("empty key");
    const auto tag = static_cast<unsigned char>(in[0]);
    in.remove_prefix(1);
    switch (tag) {
        case kKeyNull: out.setNull(); return;
        case kKeyInteger: out.setInteger(std::int64_t(readBigEndian(in) ^ kSignBit)); return;
        case kKeyReal: out.setReal(doubleFromSortable(readBigEndian(in))); return;
        case kKeyDatetime:
            out.setDatetime(Datetime{std::int64_t(readBigEndian(in) ^ kSignBit)});
            return;
        case kKeyText: {
            thread_local std::string buffer;
            readBytesInto(in, buffer);
            out.setText(buffer);
            return;
        }
        case kKeyBlob: {
            thread_local std::string buffer;
            readBytesInto(in, buffer);
            out.setBlob(buffer.data(), buffer.size());
            return;
        }
        default: corrupt("unknown key tag");
    }
}

void skipKeys(std::string_view& in, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        (void)decodeKey(in);
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
            const Blob& b = v.blob();
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

std::string encodeRow(const std::vector<Value>& values)
{
    std::string out;
    encodeRowInto(out, values.data(), values.size());
    return out;
}

void encodeRowInto(std::string& out, const Value* values, std::size_t count)
{
    putVarint(out, count);
    for (std::size_t i = 0; i < count; ++i)
        putValue(out, values[i]);
}

void decodeRowInto(std::string_view in, Value* out, std::size_t width)
{
    const std::uint64_t count = getVarint(in);
    if (count > in.size() + 1)
        corrupt("implausible column count");
    const std::size_t have = std::size_t(std::min<std::uint64_t>(count, width));
    for (std::size_t i = 0; i < have; ++i)
        getValueInto(in, out[i]);
    for (std::size_t i = have; i < width; ++i)
        out[i].setNull();
}

std::vector<Value> decodeRow(std::string_view in)
{
    const std::uint64_t count = getVarint(in);
    if (count > in.size() + 1)
        corrupt("implausible column count");
    std::vector<Value> out;
    out.reserve(std::size_t(count));
    for (std::uint64_t i = 0; i < count; ++i)
        out.push_back(getValue(in));
    return out;
}

}  // namespace sql::internal
