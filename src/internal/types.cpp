// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/types.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "internal/cnumeric.hpp"
#include "internal/util.hpp"

namespace sql::internal {

const char* toString(Affinity a) noexcept
{
    switch (a) {
        case Affinity::Blob: return "BLOB";
        case Affinity::Text: return "TEXT";
        case Affinity::Numeric: return "NUMERIC";
        case Affinity::Integer: return "INTEGER";
        case Affinity::Real: return "REAL";
    }
    return "BLOB";
}

namespace {

bool containsNoCase(std::string_view hay, std::string_view needle) noexcept
{
    if (needle.size() > hay.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && foldAscii(hay[i + k]) == foldAscii(needle[k]))
            ++k;
        if (k == needle.size())
            return true;
    }
    return false;
}

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

}  // namespace

Affinity affinityOfTypeName(std::string_view t) noexcept
{
    if (containsNoCase(t, "int"))
        return Affinity::Integer;
    if (containsNoCase(t, "char") || containsNoCase(t, "clob") || containsNoCase(t, "text"))
        return Affinity::Text;
    if (t.empty() || containsNoCase(t, "blob"))
        return Affinity::Blob;
    if (containsNoCase(t, "real") || containsNoCase(t, "floa") || containsNoCase(t, "doub"))
        return Affinity::Real;
    return Affinity::Numeric;
}

std::optional<StrictType> strictTypeOfName(std::string_view t) noexcept
{
    if (equalsNoCase(t, "ANY"))
        return StrictType::Any;
    if (equalsNoCase(t, "INT") || equalsNoCase(t, "INTEGER"))
        return StrictType::Integer;
    if (equalsNoCase(t, "REAL"))
        return StrictType::Real;
    if (equalsNoCase(t, "TEXT"))
        return StrictType::Text;
    if (equalsNoCase(t, "BLOB"))
        return StrictType::Blob;
    if (equalsNoCase(t, "DATETIME"))
        return StrictType::Datetime;
    return std::nullopt;
}

std::optional<Collation> collationOfName(std::string_view n) noexcept
{
    if (equalsNoCase(n, "BINARY"))
        return Collation::Binary;
    if (equalsNoCase(n, "NOCASE"))
        return Collation::NoCase;
    if (equalsNoCase(n, "RTRIM"))
        return Collation::RTrim;
    return std::nullopt;
}

const char* toString(Collation c) noexcept
{
    switch (c) {
        case Collation::Binary: return "BINARY";
        case Collation::NoCase: return "NOCASE";
        case Collation::RTrim: return "RTRIM";
    }
    return "BINARY";
}

// ------------------------------------------------------------- numbers ---

NumKind parseNumber(std::string_view s, std::int64_t& iout, double& dout, bool whole) noexcept
{
    std::size_t p = 0, n = s.size();
    while (p < n && isSpace(s[p]))
        ++p;
    const std::size_t start = p;
    bool neg = false;
    if (p < n && (s[p] == '+' || s[p] == '-')) {
        neg = s[p] == '-';
        ++p;
    }
    const std::size_t digitsStart = p;
    while (p < n && isDigit(s[p]))
        ++p;
    std::size_t intDigits = p - digitsStart;
    bool isReal = false;
    std::size_t fracDigits = 0;
    if (p < n && s[p] == '.') {
        std::size_t q = p + 1;
        while (q < n && isDigit(s[q]))
            ++q;
        fracDigits = q - p - 1;
        if (intDigits + fracDigits > 0) {
            isReal = true;
            p = q;
        }
    }
    if (intDigits + fracDigits == 0)
        return NumKind::None;
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        std::size_t q = p + 1;
        if (q < n && (s[q] == '+' || s[q] == '-'))
            ++q;
        if (q < n && isDigit(s[q])) {
            while (q < n && isDigit(s[q]))
                ++q;
            p = q;
            isReal = true;
        }
    }
    const std::size_t end = p;
    if (whole) {
        while (p < n && isSpace(s[p]))
            ++p;
        if (p != n)
            return NumKind::None;
    }
    if (!isReal) {
        // Accumulate as unsigned; anything past int64 becomes a real.
        std::uint64_t acc = 0;
        bool overflow = false;
        for (std::size_t k = digitsStart; k < end; ++k) {
            const unsigned dgt = unsigned(s[k] - '0');
            if (acc > (std::numeric_limits<std::uint64_t>::max() - dgt) / 10) {
                overflow = true;
                break;
            }
            acc = acc * 10 + dgt;
        }
        if (!overflow) {
            if (!neg && acc <= std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
                iout = std::int64_t(acc);
                return NumKind::Integer;
            }
            if (neg && acc <= std::uint64_t(1) << 63) {
                iout = acc == (std::uint64_t(1) << 63) ? std::numeric_limits<std::int64_t>::min()
                                                       : -std::int64_t(acc);
                return NumKind::Integer;
            }
        }
    }
    char buf[400];
    std::string heap;
    const std::size_t len = end - start;
    const char* text;
    if (len < sizeof buf) {
        std::memcpy(buf, s.data() + start, len);
        buf[len] = '\0';
        text = buf;
    } else {
        heap.assign(s.data() + start, len);
        text = heap.c_str();
    }
    dout = parseDouble(text, nullptr);
    return NumKind::Real;
}

bool realIsExactInt(double d, std::int64_t& out) noexcept
{
    if (!(d > -9223372036854775808.0 && d < 9223372036854775808.0))
        return false;
    const std::int64_t i = std::int64_t(d);
    if (double(i) != d)
        return false;
    if (i == std::numeric_limits<std::int64_t>::min() || i == std::numeric_limits<std::int64_t>::max())
        return false;
    out = i;
    return true;
}

namespace {

std::int64_t saturate(double d) noexcept
{
    if (std::isnan(d))
        return 0;
    if (d <= -9223372036854775808.0)
        return std::numeric_limits<std::int64_t>::min();
    if (d >= 9223372036854775807.0)
        return std::numeric_limits<std::int64_t>::max();
    return std::int64_t(d);
}

std::string_view bytesOf(const Value& v) noexcept
{
    if (v.type() == Type::Text)
        return v.text();
    const BlobView b = v.blob();
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

/// The integer prefix of text: sign and digits only, as sqlite3Atoi64 reads it
/// for CAST AS INTEGER.
std::int64_t integerPrefix(std::string_view s) noexcept
{
    std::size_t p = 0;
    while (p < s.size() && isSpace(s[p]))
        ++p;
    bool neg = false;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
        neg = s[p] == '-';
        ++p;
    }
    std::uint64_t acc = 0;
    bool overflow = false;
    while (p < s.size() && isDigit(s[p])) {
        const unsigned dgt = unsigned(s[p] - '0');
        if (!overflow) {
            if (acc > (std::numeric_limits<std::uint64_t>::max() - dgt) / 10)
                overflow = true;
            else
                acc = acc * 10 + dgt;
        }
        ++p;
    }
    if (neg) {
        if (overflow || acc >= (std::uint64_t(1) << 63))
            return std::numeric_limits<std::int64_t>::min();
        return -std::int64_t(acc);
    }
    if (overflow || acc > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return std::int64_t(acc);
}

}  // namespace

std::int64_t toInt64(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Null: return 0;
        case Type::Integer: return v.integer();
        case Type::Real: return saturate(v.real());
        case Type::Datetime: return v.datetime().usec;
        case Type::Text:
        case Type::Blob: return integerPrefix(bytesOf(v));
    }
    return 0;
}

double toDouble(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Null: return 0.0;
        case Type::Integer: return double(v.integer());
        case Type::Real: return v.real();
        case Type::Datetime: return double(v.datetime().usec);
        case Type::Text:
        case Type::Blob: {
            std::int64_t i = 0;
            double d = 0;
            switch (parseNumber(bytesOf(v), i, d, false)) {
                case NumKind::Integer: return double(i);
                case NumKind::Real: return d;
                case NumKind::None: return 0.0;
            }
        }
    }
    return 0.0;
}

void numericOperand(const Value& v, Value& out)
{
    switch (v.type()) {
        case Type::Null: out.setNull(); return;
        case Type::Integer: out.setInteger(v.integer()); return;
        case Type::Real: out.setReal(v.real()); return;
        case Type::Datetime: out.setInteger(v.datetime().usec); return;
        case Type::Text:
        case Type::Blob: {
            std::int64_t i = 0;
            double d = 0;
            switch (parseNumber(bytesOf(v), i, d, false)) {
                case NumKind::Integer: out.setInteger(i); return;
                case NumKind::Real: out.setReal(d); return;
                case NumKind::None: out.setInteger(0); return;
            }
        }
    }
}

bool isTrue(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Null: return false;
        case Type::Integer: return v.integer() != 0;
        case Type::Real: return v.real() != 0.0;
        case Type::Datetime: return v.datetime().usec != 0;
        case Type::Text:
        case Type::Blob: return toDouble(v) != 0.0;
    }
    return false;
}

// ---------------------------------------------------------------- text ---

void realToText(double d, std::string& out)
{
    if (std::isnan(d)) {
        out += "NaN";
        return;
    }
    if (std::isinf(d)) {
        out += d < 0 ? "-Inf" : "Inf";
        return;
    }
    if (d == 0.0) {
        out += "0.0";
        return;
    }
    char buf[64];
    int n = formatPrecision(buf, sizeof buf, d, 15);
    if (n <= 0 || std::size_t(n) >= sizeof buf) {
        out += "0.0";
        return;
    }
    std::string_view s(buf, std::size_t(n));
    const std::size_t e = s.find_first_of("eE");
    const std::string_view mantissa = s.substr(0, e);
    if (mantissa.find('.') != std::string_view::npos) {
        out.append(s);
        return;
    }
    out.append(mantissa);
    out += ".0";
    if (e != std::string_view::npos)
        out.append(s.substr(e));
}

std::string realToText(double d)
{
    std::string s;
    realToText(d, s);
    return s;
}

void appendText(const Value& v, std::string& out)
{
    switch (v.type()) {
        case Type::Null: return;
        case Type::Integer: {
            char buf[24];
            const int n = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.integer()));
            out.append(buf, std::size_t(n));
            return;
        }
        case Type::Real: realToText(v.real(), out); return;
        case Type::Datetime: out += formatDatetime(v.datetime()); return;
        case Type::Text: out += v.text(); return;
        case Type::Blob: out.append(bytesOf(v)); return;
    }
}

std::string_view textOf(const Value& v, std::string& scratch)
{
    if (v.type() == Type::Text || v.type() == Type::Blob)
        return bytesOf(v);
    scratch.clear();
    appendText(v, scratch);
    return scratch;
}

// --------------------------------------------------------- conversions ---

namespace {

/// NUMERIC affinity on text: only a well-formed number converts, and a real
/// that is exactly an integer becomes one.
void numericFromText(Value& v, bool preferInt)
{
    std::int64_t i = 0;
    double d = 0;
    switch (parseNumber(bytesOf(v), i, d, true)) {
        case NumKind::Integer: v.setInteger(i); return;
        case NumKind::Real: {
            std::int64_t asInt = 0;
            if (preferInt && realIsExactInt(d, asInt))
                v.setInteger(asInt);
            else
                v.setReal(d);
            return;
        }
        case NumKind::None: return;
    }
}

void toTextInPlace(Value& v)
{
    std::string s;
    appendText(v, s);
    v.setText(s);
}

}  // namespace

void applyAffinity(Value& v, Affinity a)
{
    const Type t = v.type();
    if (t == Type::Null || t == Type::Blob || t == Type::Datetime)
        return;
    switch (a) {
        case Affinity::Blob: return;
        case Affinity::Text:
            if (t == Type::Integer || t == Type::Real)
                toTextInPlace(v);
            return;
        case Affinity::Numeric:
        case Affinity::Integer:
            if (t == Type::Text) {
                numericFromText(v, true);
            } else if (t == Type::Real) {
                std::int64_t i = 0;
                if (realIsExactInt(v.real(), i))
                    v.setInteger(i);
            }
            return;
        case Affinity::Real:
            if (t == Type::Text)
                numericFromText(v, false);
            if (v.type() == Type::Integer)
                v.setReal(double(v.integer()));
            return;
    }
}

void applyComparisonAffinity(Value& v, Affinity a)
{
    const Type t = v.type();
    switch (a) {
        case Affinity::Blob: return;
        case Affinity::Text:
            if (t == Type::Integer || t == Type::Real)
                toTextInPlace(v);
            return;
        default:
            if (t == Type::Text)
                numericFromText(v, false);
            return;
    }
}

void castTo(Value& v, Affinity a)
{
    const Type t = v.type();
    if (t == Type::Null)
        return;
    switch (a) {
        case Affinity::Blob:
            if (t != Type::Blob) {
                std::string s;
                appendText(v, s);
                v.setBlob(s.data(), s.size());
            }
            return;
        case Affinity::Text:
            if (t != Type::Text) {
                std::string s;
                appendText(v, s);
                v.setText(s);
            }
            return;
        case Affinity::Integer: v.setInteger(toInt64(v)); return;
        case Affinity::Real: v.setReal(toDouble(v)); return;
        case Affinity::Numeric: {
            if (t == Type::Integer)
                return;
            if (t == Type::Datetime) {
                v.setInteger(v.datetime().usec);
                return;
            }
            double d = 0;
            std::int64_t i = 0;
            if (t == Type::Real) {
                d = v.real();
            } else {
                switch (parseNumber(bytesOf(v), i, d, false)) {
                    case NumKind::Integer: v.setInteger(i); return;
                    case NumKind::None: v.setInteger(0); return;
                    case NumKind::Real: break;
                }
            }
            if (realIsExactInt(d, i))
                v.setInteger(i);
            else
                v.setReal(d);
            return;
        }
    }
}

bool strictCoerce(Value& v, StrictType t)
{
    const Type have = v.type();
    if (have == Type::Null || t == StrictType::Any)
        return true;
    switch (t) {
        case StrictType::Any: return true;
        case StrictType::Integer:
            if (have == Type::Integer)
                return true;
            if (have == Type::Real) {
                std::int64_t i = 0;
                if (!realIsExactInt(v.real(), i))
                    return false;
                v.setInteger(i);
                return true;
            }
            if (have == Type::Text) {
                Value probe = v;
                numericFromText(probe, true);
                if (probe.type() != Type::Integer)
                    return false;
                v = probe;
                return true;
            }
            return false;
        case StrictType::Real:
            if (have == Type::Real)
                return true;
            if (have == Type::Integer) {
                v.setReal(double(v.integer()));
                return true;
            }
            if (have == Type::Text) {
                Value probe = v;
                numericFromText(probe, false);
                if (probe.type() == Type::Integer)
                    probe.setReal(double(probe.integer()));
                if (probe.type() != Type::Real)
                    return false;
                v = probe;
                return true;
            }
            return false;
        case StrictType::Text:
            if (have == Type::Text)
                return true;
            if (have == Type::Integer || have == Type::Real) {
                toTextInPlace(v);
                return true;
            }
            return false;
        case StrictType::Blob: return have == Type::Blob;
        case StrictType::Datetime:
            if (have == Type::Datetime)
                return true;
            if (have == Type::Text) {
                try {
                    v.setDatetime(parseDatetime(v.text()));
                    return true;
                } catch (const Error&) {
                    return false;
                }
            }
            if (have == Type::Integer) {
                v.setDatetime(Datetime{v.integer()});
                return true;
            }
            return false;
    }
    return false;
}

// ----------------------------------------------------------- ordering ----

int compareIntReal(std::int64_t i, double r) noexcept
{
    if (std::isnan(r))
        return 1;
    if (r < -9223372036854775808.0)
        return 1;
    if (r >= 9223372036854775808.0)
        return -1;
    const std::int64_t y = std::int64_t(r);
    if (i < y)
        return -1;
    if (i > y)
        return 1;
    const double s = double(i);
    if (s < r)
        return -1;
    if (s > r)
        return 1;
    return 0;
}

int compareText(std::string_view a, std::string_view b, Collation c) noexcept
{
    switch (c) {
        case Collation::Binary: break;
        case Collation::NoCase: {
            const std::size_t n = a.size() < b.size() ? a.size() : b.size();
            for (std::size_t i = 0; i < n; ++i) {
                const unsigned char x = static_cast<unsigned char>(foldAscii(a[i]));
                const unsigned char y = static_cast<unsigned char>(foldAscii(b[i]));
                if (x != y)
                    return x < y ? -1 : 1;
            }
            return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
        }
        case Collation::RTrim:
            while (!a.empty() && a.back() == ' ')
                a.remove_suffix(1);
            while (!b.empty() && b.back() == ' ')
                b.remove_suffix(1);
            break;
    }
    const int r = a.compare(b);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

namespace {

int rank(Type t) noexcept
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

}  // namespace

int compareValues(const Value& a, const Value& b, Collation c) noexcept
{
    const Type ta = a.type(), tb = b.type();
    const int ra = rank(ta), rb = rank(tb);
    if (ra != rb)
        return ra < rb ? -1 : 1;
    switch (ra) {
        case 0: return 0;
        case 1:
            if (ta == Type::Integer && tb == Type::Integer) {
                const std::int64_t x = a.integer(), y = b.integer();
                return x < y ? -1 : (x > y ? 1 : 0);
            }
            if (ta == Type::Real && tb == Type::Real) {
                const double x = a.real(), y = b.real();
                return x < y ? -1 : (x > y ? 1 : 0);
            }
            if (ta == Type::Integer)
                return compareIntReal(a.integer(), b.real());
            return -compareIntReal(b.integer(), a.real());
        case 2: {
            const std::int64_t x = a.datetime().usec, y = b.datetime().usec;
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        case 3: return compareText(a.text(), b.text(), c);
        default: {
            const std::string_view x = bytesOf(a), y = bytesOf(b);
            const int r = x.compare(y);
            return r < 0 ? -1 : (r > 0 ? 1 : 0);
        }
    }
}

// -------------------------------------------------------------- UTF-8 ----

std::size_t utf8Length(std::string_view s) noexcept
{
    // SQLite's SQLITE_SKIP_UTF8: a lead byte >= 0xC0 swallows the
    // continuation bytes after it; any other byte, a stray continuation byte
    // included, is a character of its own.
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++n) {
        if (static_cast<unsigned char>(s[i++]) >= 0xC0) {
            while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
                ++i;
        }
    }
    return n;
}

std::uint32_t utf8Next(std::string_view s, std::size_t& i) noexcept
{
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0xC0)
        return c;
    std::uint32_t cp;
    int extra;
    if (c < 0xE0) {
        cp = c & 0x1F;
        extra = 1;
    } else if (c < 0xF0) {
        cp = c & 0x0F;
        extra = 2;
    } else {
        cp = c & 0x07;
        extra = 3;
    }
    while (extra-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    return cp;
}

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// ---------------------------------------------------------- patterns -----

namespace {

std::uint32_t foldCp(std::uint32_t c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/// SQLite's patternCompare, for LIKE and GLOB alike. Recursion happens only at
/// a `%`/`*`, and a failed sub-match there stops the whole match, which keeps
/// the worst case polynomial.
enum class Match
{
    Yes,
    No,
    NoWildcardMatch,
};

Match patternCompare(std::string_view pat, std::size_t pi, std::string_view str, std::size_t si,
                     bool glob, bool noCase, std::uint32_t esc)
{
    const std::uint32_t matchAll = glob ? '*' : '%';
    const std::uint32_t matchOne = glob ? '?' : '_';
    const std::uint32_t matchSet = glob ? '[' : 0;
    while (pi < pat.size()) {
        std::uint32_t c = utf8Next(pat, pi);
        if (c == matchAll) {
            // Skip further wildcards; each `matchOne` among them eats a char.
            while (pi < pat.size()) {
                std::size_t save = pi;
                const std::uint32_t c2 = utf8Next(pat, pi);
                if (c2 == matchAll)
                    continue;
                if (c2 == matchOne) {
                    if (si >= str.size())
                        return Match::NoWildcardMatch;
                    utf8Next(str, si);
                    continue;
                }
                pi = save;
                break;
            }
            if (pi >= pat.size())
                return Match::Yes;
            std::size_t peek = pi;
            std::uint32_t next = utf8Next(pat, peek);
            if (next == esc && esc != 0) {
                if (peek >= pat.size())
                    return Match::NoWildcardMatch;
            }
            while (si < str.size()) {
                const Match r = patternCompare(pat, pi, str, si, glob, noCase, esc);
                if (r != Match::No)
                    return r;
                utf8Next(str, si);
            }
            return Match::NoWildcardMatch;
        }
        if (c == matchOne) {
            if (si >= str.size())
                return Match::No;
            utf8Next(str, si);
            continue;
        }
        if (glob && c == matchSet) {
            if (si >= str.size())
                return Match::No;
            const std::uint32_t sc = utf8Next(str, si);
            bool invert = false, seen = false;
            std::uint32_t prior = 0;
            if (pi < pat.size() && pat[pi] == '^') {
                invert = true;
                ++pi;
            }
            if (pi < pat.size() && pat[pi] == ']') {
                seen = sc == ']';
                ++pi;
            }
            bool closed = false;
            while (pi < pat.size()) {
                std::uint32_t pc = utf8Next(pat, pi);
                if (pc == ']') {
                    closed = true;
                    break;
                }
                if (pc == '-' && prior != 0 && pi < pat.size() && pat[pi] != ']') {
                    const std::uint32_t hi = utf8Next(pat, pi);
                    if (sc >= prior && sc <= hi)
                        seen = true;
                    prior = 0;
                } else {
                    if (sc == pc)
                        seen = true;
                    prior = pc;
                }
            }
            if (!closed || seen == invert)
                return Match::No;
            continue;
        }
        if (!glob && esc != 0 && c == esc) {
            if (pi >= pat.size())
                return Match::No;
            c = utf8Next(pat, pi);
        }
        if (si >= str.size())
            return Match::No;
        const std::uint32_t sc = utf8Next(str, si);
        if (c != sc && !(noCase && foldCp(c) == foldCp(sc)))
            return Match::No;
    }
    return si >= str.size() ? Match::Yes : Match::No;
}

}  // namespace

bool likeMatch(std::string_view pattern, std::string_view text, std::uint32_t escape,
               bool caseSensitive) noexcept
{
    return patternCompare(pattern, 0, text, 0, false, !caseSensitive, escape) == Match::Yes;
}

bool globMatch(std::string_view pattern, std::string_view text) noexcept
{
    return patternCompare(pattern, 0, text, 0, true, false, 0) == Match::Yes;
}

}  // namespace sql::internal
