// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Storage classes, the dynamically typed cell, and the conversions between
// them.

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "internal/cnumeric.hpp"
#include "internal/lexer.hpp"
#include "sql/sql.hpp"

namespace sql {

using internal::equalsNoCase;

const char* toString(ErrorCode c) noexcept
{
    switch (c) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::SyntaxError: return "syntax error";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::InvalidArgument: return "invalid argument";
        case ErrorCode::NoSuchTable: return "no such table";
        case ErrorCode::TableExists: return "table already exists";
        case ErrorCode::NoSuchColumn: return "no such column";
        case ErrorCode::AmbiguousColumn: return "ambiguous column name";
        case ErrorCode::NoSuchIndex: return "no such index";
        case ErrorCode::IndexExists: return "index already exists";
        case ErrorCode::TypeMismatch: return "type mismatch";
        case ErrorCode::ConstraintViolation: return "constraint violation";
        case ErrorCode::BadTransaction: return "transaction is no longer usable";
        case ErrorCode::ReadOnly: return "database is read-only";
        case ErrorCode::Internal: return "internal error";
    }
    return "unknown error";
}

const char* toString(Type t) noexcept
{
    switch (t) {
        case Type::Null: return "NULL";
        case Type::Integer: return "INTEGER";
        case Type::Real: return "REAL";
        case Type::Datetime: return "DATETIME";
        case Type::Text: return "TEXT";
        case Type::Blob: return "BLOB";
    }
    return "NULL";
}

namespace {

[[noreturn]] void mismatch(const std::string& what)
{
    throw Error(ErrorCode::TypeMismatch, what);
}

/// Days since 1970-01-01 for a proleptic Gregorian date. Hinnant's algorithm.
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) noexcept
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + std::int64_t(doe) - 719468;
}

void civilFromDays(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = std::int64_t(yoe) + era * 400 + (m <= 2);
}

/// Floor division, which `/` is not for negative numerators.
std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept
{
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

bool digitsToInt(std::string_view s, std::int64_t& out) noexcept
{
    if (s.empty())
        return false;
    std::int64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

}  // namespace

std::optional<Type> typeFromName(std::string_view name) noexcept
{
    static const struct
    {
        const char* spelling;
        Type type;
    } kNames[] = {
        {"NULL", Type::Null},         {"INT", Type::Integer},     {"INTEGER", Type::Integer},
        {"BIGINT", Type::Integer},    {"SMALLINT", Type::Integer}, {"REAL", Type::Real},
        {"FLOAT", Type::Real},        {"DOUBLE", Type::Real},     {"NUMERIC", Type::Real},
        {"TEXT", Type::Text},         {"VARCHAR", Type::Text},    {"CHAR", Type::Text},
        {"STRING", Type::Text},       {"DATETIME", Type::Datetime},
        {"TIMESTAMP", Type::Datetime}, {"DATE", Type::Datetime},  {"BLOB", Type::Blob},
        {"BINARY", Type::Blob},
    };
    for (const auto& entry : kNames) {
        if (equalsNoCase(name, entry.spelling))
            return entry.type;
    }
    return std::nullopt;
}

// ------------------------------------------------------------ datetime ----

Datetime parseDatetime(std::string_view text)
{
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    if (!text.empty() && (text.back() == 'Z' || text.back() == 'z'))
        text.remove_suffix(1);

    std::int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, micros = 0;
    const bool dated = text.size() >= 10 && text[4] == '-' && text[7] == '-' &&
                       digitsToInt(text.substr(0, 4), year) &&
                       digitsToInt(text.substr(5, 2), month) &&
                       digitsToInt(text.substr(8, 2), day);
    if (!dated)
        mismatch("'" + std::string(text) + "' is not a datetime");

    std::string_view rest = text.substr(10);
    if (!rest.empty()) {
        if (rest[0] != ' ' && rest[0] != 'T' && rest[0] != 't')
            mismatch("'" + std::string(text) + "' is not a datetime");
        rest.remove_prefix(1);
        if (rest.size() < 5 || rest[2] != ':' || !digitsToInt(rest.substr(0, 2), hour) ||
            !digitsToInt(rest.substr(3, 2), minute))
            mismatch("'" + std::string(text) + "' is not a datetime");
        rest.remove_prefix(5);
        if (!rest.empty() && rest[0] == ':') {
            if (rest.size() < 3 || !digitsToInt(rest.substr(1, 2), second))
                mismatch("'" + std::string(text) + "' is not a datetime");
            rest.remove_prefix(3);
        }
        if (!rest.empty() && rest[0] == '.') {
            rest.remove_prefix(1);
            std::string frac(rest.substr(0, 6));
            if (frac.empty() || !digitsToInt(frac, micros))
                mismatch("'" + std::string(text) + "' is not a datetime");
            for (std::size_t i = frac.size(); i < 6; ++i)
                micros *= 10;
            rest.remove_prefix(std::min<std::size_t>(rest.size(), 6));
        }
        if (!rest.empty())
            mismatch("'" + std::string(text) + "' is not a datetime");
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
        mismatch("'" + std::string(text) + "' is not a valid calendar time");

    const std::int64_t days = daysFromCivil(year, unsigned(month), unsigned(day));
    const std::int64_t secs = days * 86400 + hour * 3600 + minute * 60 + second;
    return Datetime{secs * 1000000 + micros};
}

std::string formatDatetime(Datetime when)
{
    const std::int64_t secs = floorDiv(when.usec, 1000000);
    const std::int64_t micros = when.usec - secs * 1000000;
    const std::int64_t days = floorDiv(secs, 86400);
    const std::int64_t rem = secs - days * 86400;

    std::int64_t year = 0;
    unsigned month = 0, day = 0;
    civilFromDays(days, year, month, day);

    char buf[64];
    int n = std::snprintf(buf, sizeof buf, "%04" PRId64 "-%02u-%02u %02" PRId64 ":%02" PRId64
                                           ":%02" PRId64,
                          year, month, day, rem / 3600, (rem / 60) % 60, rem % 60);
    if (micros != 0)
        n += std::snprintf(buf + n, sizeof buf - std::size_t(n), ".%06" PRId64, micros);
    return std::string(buf, std::size_t(n));
}

std::size_t formatDatetimeIso(Datetime when, char* buf, std::size_t size)
{
    if (size < 32)
        throw Error(ErrorCode::InvalidArgument, "formatDatetimeIso needs a 32-byte buffer");

    const std::int64_t secs = floorDiv(when.usec, 1000000);
    const std::int64_t micros = when.usec - secs * 1000000;
    const std::int64_t days = floorDiv(secs, 86400);
    const std::int64_t rem = secs - days * 86400;

    std::int64_t year = 0;
    unsigned month = 0, day = 0;
    civilFromDays(days, year, month, day);

    // Years outside four digits would push the fraction and the 'Z' past the
    // promised 32 bytes, so they saturate rather than overflow the buffer.
    year = std::min<std::int64_t>(std::max<std::int64_t>(year, -999), 9999);

    char scratch[40];
    int n = std::snprintf(scratch, sizeof scratch,
                          "%04" PRId64 "-%02u-%02uT%02" PRId64 ":%02" PRId64 ":%02" PRId64, year,
                          month, day, rem / 3600, (rem / 60) % 60, rem % 60);
    if (micros != 0)
        n += std::snprintf(scratch + n, sizeof scratch - std::size_t(n), ".%06" PRId64, micros);
    scratch[n++] = 'Z';

    std::memcpy(buf, scratch, std::size_t(n));
    return std::size_t(n);
}

// --------------------------------------------------------------- value ----

Value Value::blob(const void* data, std::size_t size)
{
    const auto* p = static_cast<const std::byte*>(data);
    return Value(Blob(p, p + size));
}

std::int64_t Value::integer() const
{
    if (type() != Type::Integer)
        mismatch(std::string("value is ") + toString(type()) + ", not INTEGER");
    return std::get<std::int64_t>(v_);
}

double Value::real() const
{
    if (type() != Type::Real)
        mismatch(std::string("value is ") + toString(type()) + ", not REAL");
    return std::get<double>(v_);
}

Datetime Value::datetime() const
{
    if (type() != Type::Datetime)
        mismatch(std::string("value is ") + toString(type()) + ", not DATETIME");
    return std::get<Datetime>(v_);
}

const std::string& Value::text() const
{
    if (type() != Type::Text)
        mismatch(std::string("value is ") + toString(type()) + ", not TEXT");
    return std::get<std::string>(v_);
}

const Blob& Value::blob() const
{
    if (type() != Type::Blob)
        mismatch(std::string("value is ") + toString(type()) + ", not BLOB");
    return std::get<Blob>(v_);
}

std::int64_t Value::toInteger() const
{
    return cast(Type::Integer).integer();
}

double Value::toReal() const
{
    return cast(Type::Real).real();
}

std::string Value::toText() const
{
    switch (type()) {
        case Type::Null: return std::string();
        case Type::Text: return std::get<std::string>(v_);
        case Type::Integer: return std::to_string(std::get<std::int64_t>(v_));
        case Type::Datetime: return formatDatetime(std::get<Datetime>(v_));
        case Type::Real: {
            const double d = std::get<double>(v_);
            // The shortest spelling that reads back as the same double, so
            // exact values stay tidy and inexact ones stay honest.
            char buf[40];
            const int n = internal::formatShortest(buf, sizeof buf, d);
            return std::string(buf, std::size_t(n));
        }
        case Type::Blob: {
            static const char* kHex = "0123456789abcdef";
            const Blob& b = std::get<Blob>(v_);
            std::string out(b.size() * 2, '\0');
            for (std::size_t i = 0; i < b.size(); ++i) {
                const auto byte = static_cast<unsigned char>(b[i]);
                out[i * 2] = kHex[byte >> 4];
                out[i * 2 + 1] = kHex[byte & 0x0f];
            }
            return out;
        }
    }
    return std::string();
}

std::string Value::toLiteral() const
{
    switch (type()) {
        case Type::Null: return "NULL";
        case Type::Blob: return "x'" + toText() + "'";
        case Type::Text:
        case Type::Datetime: {
            const std::string raw = toText();
            std::string out = "'";
            for (const char c : raw) {
                if (c == '\'')
                    out += "''";
                else
                    out += c;
            }
            return out + "'";
        }
        default: return toText();
    }
}

bool Value::truthy() const noexcept
{
    switch (type()) {
        case Type::Null: return false;
        case Type::Integer: return std::get<std::int64_t>(v_) != 0;
        case Type::Real: return std::get<double>(v_) != 0.0;
        case Type::Datetime: return std::get<Datetime>(v_).usec != 0;
        case Type::Text: return !std::get<std::string>(v_).empty();
        case Type::Blob: return !std::get<Blob>(v_).empty();
    }
    return false;
}

Value Value::cast(Type target) const
{
    if (isNull() || target == type() || target == Type::Null)
        return *this;

    switch (target) {
        case Type::Integer:
            switch (type()) {
                case Type::Real: {
                    const double d = std::get<double>(v_);
                    if (!(d >= -9.2233720368547758e18 && d <= 9.2233720368547758e18))
                        mismatch("real value is out of INTEGER range");
                    return Value(std::int64_t(d));
                }
                case Type::Datetime: return Value(std::get<Datetime>(v_).usec);
                case Type::Text: {
                    const std::string& s = std::get<std::string>(v_);
                    char* end = nullptr;
                    const long long v = std::strtoll(s.c_str(), &end, 10);
                    if (end == s.c_str() || *end != '\0')
                        mismatch("'" + s + "' is not an INTEGER");
                    return Value(std::int64_t(v));
                }
                default: break;
            }
            break;

        case Type::Real:
            switch (type()) {
                case Type::Integer: return Value(double(std::get<std::int64_t>(v_)));
                case Type::Datetime: return Value(double(std::get<Datetime>(v_).usec));
                case Type::Text: {
                    const std::string& s = std::get<std::string>(v_);
                    char* end = nullptr;
                    const double v = internal::parseDouble(s.c_str(), &end);
                    if (end == s.c_str() || *end != '\0')
                        mismatch("'" + s + "' is not a REAL");
                    return Value(v);
                }
                default: break;
            }
            break;

        case Type::Datetime:
            switch (type()) {
                case Type::Integer: return Value(Datetime{std::get<std::int64_t>(v_)});
                case Type::Text: return Value(parseDatetime(std::get<std::string>(v_)));
                default: break;
            }
            break;

        case Type::Text:
            if (type() == Type::Blob) {
                const Blob& b = std::get<Blob>(v_);
                return Value(std::string(reinterpret_cast<const char*>(b.data()), b.size()));
            }
            return Value(toText());

        case Type::Blob:
            if (type() == Type::Text) {
                const std::string& s = std::get<std::string>(v_);
                return Value::blob(s.data(), s.size());
            }
            break;

        case Type::Null: break;
    }
    mismatch(std::string("cannot store ") + toString(type()) + " as " + toString(target));
}

bool operator==(const Value& a, const Value& b) noexcept
{
    return a.v_ == b.v_;
}

namespace {

/// Ordering rank: integers and reals share one so they interleave numerically.
int rankOf(Type t) noexcept
{
    switch (t) {
        case Type::Null: return 0;
        case Type::Integer:
        case Type::Real: return 1;
        case Type::Datetime: return 2;
        case Type::Text: return 3;
        case Type::Blob: return 4;
    }
    return 0;
}

template <class T>
int cmp3(const T& a, const T& b) noexcept
{
    return a < b ? -1 : (b < a ? 1 : 0);
}

}  // namespace

int compare(const Value& a, const Value& b) noexcept
{
    const int ra = rankOf(a.type()), rb = rankOf(b.type());
    if (ra != rb)
        return ra < rb ? -1 : 1;

    switch (a.type()) {
        case Type::Null: return 0;
        case Type::Integer:
            if (b.type() == Type::Integer)
                return cmp3(std::get<std::int64_t>(a.v_), std::get<std::int64_t>(b.v_));
            return cmp3(double(std::get<std::int64_t>(a.v_)), std::get<double>(b.v_));
        case Type::Real:
            if (b.type() == Type::Real)
                return cmp3(std::get<double>(a.v_), std::get<double>(b.v_));
            return cmp3(std::get<double>(a.v_), double(std::get<std::int64_t>(b.v_)));
        case Type::Datetime:
            return cmp3(std::get<Datetime>(a.v_).usec, std::get<Datetime>(b.v_).usec);
        case Type::Text:
            return cmp3(std::get<std::string>(a.v_).compare(std::get<std::string>(b.v_)), 0);
        case Type::Blob: {
            const Blob& x = std::get<Blob>(a.v_);
            const Blob& y = std::get<Blob>(b.v_);
            const std::size_t n = std::min(x.size(), y.size());
            if (n) {
                const int c = std::memcmp(x.data(), y.data(), n);
                if (c)
                    return c < 0 ? -1 : 1;
            }
            return cmp3(x.size(), y.size());
        }
    }
    return 0;
}

}  // namespace sql
