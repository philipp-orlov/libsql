// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// SQLite's core scalar functions and math functions.

#include <cmath>
#include <cstring>
#include <limits>
#include <random>

#include "internal/cnumeric.hpp"
#include "internal/encoding.hpp"
#include "internal/exec.hpp"
#include "internal/util.hpp"
#include "sql/sql.hpp"

namespace sql::internal {

namespace {

const char* const kSqliteVersion = "3.45.1";

bool anyNull(int argc, Args argv)
{
    for (int i = 0; i < argc; ++i) {
        if (argv[i]->isNull())
            return true;
    }
    return false;
}

std::string_view bytesView(const Value& v)
{
    const BlobView b = v.blob();
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

/// A value's bytes up to the first NUL, blob or not: what SQLite's C-string
/// readers (LIKE, GLOB) see.
std::string_view cString(std::string_view v) noexcept
{
    const std::size_t z = v.find('\0');
    return z == std::string_view::npos ? v : v.substr(0, z);
}

/// Text up to the first NUL, as SQLite's text functions see it.
std::string_view cText(const Value& v, std::string& scratch)
{
    std::string_view s = textOf(v, scratch);
    if (v.type() != Type::Blob) {
        const std::size_t z = s.find('\0');
        if (z != std::string_view::npos)
            s = s.substr(0, z);
    }
    return s;
}

std::mt19937_64& rng()
{
    thread_local std::mt19937_64 gen(std::random_device{}());
    return gen;
}

// ----------------------------------------------------------- scalars ----

void fnAbs(FuncContext& ctx, int, Args argv, Value& out)
{
    const Value& v = *argv[0];
    switch (v.type()) {
        case Type::Null: out.setNull(); return;
        case Type::Integer:
            if (v.integer() == std::numeric_limits<std::int64_t>::min())
                ctx.error("integer overflow");
            out.setInteger(v.integer() < 0 ? -v.integer() : v.integer());
            return;
        case Type::Datetime: out.setInteger(std::llabs(v.datetime().usec)); return;
        default: out.setReal(std::fabs(toDouble(v))); return;
    }
}

void fnChanges(FuncContext& ctx, int, Args, Value& out)
{
    out.setInteger(ctx.conn ? ctx.conn->changes.load() : 0);
}

void fnTotalChanges(FuncContext& ctx, int, Args, Value& out)
{
    out.setInteger(ctx.conn ? ctx.conn->totalChanges.load() : 0);
}

void fnLastInsertRowid(FuncContext& ctx, int, Args, Value& out)
{
    out.setInteger(ctx.conn ? ctx.conn->lastInsertRowid.load() : 0);
}

void fnChar(FuncContext& ctx, int argc, Args argv, Value& out)
{
    std::string& s = ctx.buf;
    s.clear();
    for (int i = 0; i < argc; ++i) {
        std::int64_t cp = toInt64(*argv[i]);
        if (cp < 0 || cp > 0x10ffff)
            cp = 0xfffd;
        appendUtf8(s, std::uint32_t(cp));
    }
    out.setText(s);
}

void fnConcat(FuncContext& ctx, int argc, Args argv, Value& out)
{
    std::string& s = ctx.buf;
    s.clear();
    for (int i = 0; i < argc; ++i)
        appendText(*argv[i], s);
    out.setText(s);
}

void fnConcatWs(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (argv[0]->isNull()) {
        out.setNull();
        return;
    }
    std::string sep;
    appendText(*argv[0], sep);
    std::string& s = ctx.buf;
    s.clear();
    bool first = true;
    for (int i = 1; i < argc; ++i) {
        if (argv[i]->isNull())
            continue;
        if (!first)
            s += sep;
        first = false;
        appendText(*argv[i], s);
    }
    out.setText(s);
}

void fnHex(FuncContext& ctx, int, Args argv, Value& out)
{
    static const char* kDigits = "0123456789ABCDEF";
    std::string scratch;
    const std::string_view bytes = argv[0]->isNull() ? std::string_view() : textOf(*argv[0], scratch);
    std::string& s = ctx.buf;
    s.clear();
    s.reserve(bytes.size() * 2);
    for (const char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        s += kDigits[b >> 4];
        s += kDigits[b & 15];
    }
    out.setText(s);
}

// storage_row(x), storage_key(x), storage_rowid(x): what the bytes of a stored record, key or rowid key mean, for
// reading PRAGMA storage_nodes; NULL when the bytes are not one. See pragma_storage.cpp.
void fnStorageRow(FuncContext& ctx, int, Args argv, Value& out)
{
    out.setNull();
    if (argv[0]->type() != Type::Blob)
        return;
    try {
        const BlobView b = argv[0]->blob();
        const std::vector<Value> cells = decodeRow(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
        std::string& s = ctx.buf;
        s = "(";
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (i)
                s += ", ";
            quoteValue(cells[i], s);
        }
        s += ")";
        out.setText(s);
    } catch (const std::exception&) {
        out.setNull();
    }
}

void fnStorageKey(FuncContext& ctx, int, Args argv, Value& out)
{
    out.setNull();
    if (argv[0]->type() != Type::Blob)
        return;
    try {
        const BlobView b = argv[0]->blob();
        std::string_view in(reinterpret_cast<const char*>(b.data()), b.size());
        std::string& s = ctx.buf;
        s = "(";
        for (bool first = true; !in.empty(); first = false) {
            if (!first)
                s += ", ";
            quoteValue(decodeKey(in), s);
        }
        s += ")";
        out.setText(s);
    } catch (const std::exception&) {
        out.setNull();
    }
}

void fnStorageRowid(FuncContext&, int, Args argv, Value& out)
{
    out.setNull();
    if (argv[0]->type() != Type::Blob || argv[0]->blob().size() != 8)
        return;
    const BlobView b = argv[0]->blob();
    std::string_view in(reinterpret_cast<const char*>(b.data()), b.size());
    out.setInteger(readRowidKey(in));
}

void fnInstr(FuncContext& ctx, int, Args argv, Value& out)
{
    if (anyNull(2, argv)) {
        out.setNull();
        return;
    }
    const Value& h = *argv[0];
    const Value& n = *argv[1];
    if (h.type() == Type::Blob && n.type() == Type::Blob) {
        const std::string_view hs = bytesView(h), ns = bytesView(n);
        const std::size_t at = hs.find(ns);
        out.setInteger(at == std::string_view::npos ? 0 : std::int64_t(at) + 1);
        return;
    }
    // SQLite's instr over the whole values (NULs included): it steps one
    // byte plus any continuation bytes after it, compares only where a step
    // starts, and counts the steps.
    std::string a, b;
    const std::string_view hs = textOf(h, a), ns = textOf(n, b);
    (void)ctx;
    std::int64_t steps = 1;
    std::size_t pos = 0;
    while (ns.size() <= hs.size() - pos) {
        if (hs.compare(pos, ns.size(), ns) == 0) {
            out.setInteger(steps);
            return;
        }
        ++steps;
        ++pos;
        while (pos < hs.size() && (static_cast<unsigned char>(hs[pos]) & 0xc0) == 0x80)
            ++pos;
    }
    out.setInteger(0);
}

void fnLength(FuncContext&, int, Args argv, Value& out)
{
    const Value& v = *argv[0];
    switch (v.type()) {
        case Type::Null: out.setNull(); return;
        case Type::Blob: out.setInteger(std::int64_t(v.blob().size())); return;
        default: {
            std::string scratch;
            out.setInteger(std::int64_t(utf8Length(cText(v, scratch))));
            return;
        }
    }
}

void fnOctetLength(FuncContext&, int, Args argv, Value& out)
{
    const Value& v = *argv[0];
    if (v.isNull()) {
        out.setNull();
        return;
    }
    std::string scratch;
    out.setInteger(std::int64_t(textOf(v, scratch).size()));
}

void caseMap(FuncContext& ctx, const Value& v, Value& out, bool upper)
{
    if (v.isNull()) {
        out.setNull();
        return;
    }
    std::string& s = ctx.buf;
    s.clear();
    appendText(v, s);
    for (char& ch : s)
        ch = upper ? upperAscii(ch) : lowerAscii(ch);
    out.setText(s);
}

void fnLower(FuncContext& ctx, int, Args argv, Value& out)
{
    caseMap(ctx, *argv[0], out, false);
}

void fnUpper(FuncContext& ctx, int, Args argv, Value& out)
{
    caseMap(ctx, *argv[0], out, true);
}

void trimImpl(FuncContext& ctx, int argc, Args argv, Value& out, bool left, bool right)
{
    if (argv[0]->isNull() || (argc > 1 && argv[1]->isNull())) {
        out.setNull();
        return;
    }
    // The whole value is trimmed (NULs included); the character set ends at
    // its first NUL, as in SQLite.
    std::string a, b;
    std::string_view s = textOf(*argv[0], a);
    std::vector<std::string_view> set;
    if (argc > 1) {
        const std::string_view chars = cText(*argv[1], b);
        for (std::size_t i = 0; i < chars.size();) {
            const std::size_t start = i;
            utf8Next(chars, i);
            set.push_back(chars.substr(start, i - start));
        }
    } else {
        set.push_back(" ");
    }
    auto matchesFront = [&](std::string_view x) -> std::size_t {
        for (const std::string_view c : set) {
            if (!c.empty() && x.substr(0, c.size()) == c)
                return c.size();
        }
        return 0;
    };
    auto matchesBack = [&](std::string_view x) -> std::size_t {
        for (const std::string_view c : set) {
            if (!c.empty() && x.size() >= c.size() && x.substr(x.size() - c.size()) == c)
                return c.size();
        }
        return 0;
    };
    if (left) {
        while (std::size_t n = matchesFront(s))
            s.remove_prefix(n);
    }
    if (right) {
        while (std::size_t n = matchesBack(s))
            s.remove_suffix(n);
    }
    ctx.buf.assign(s.data(), s.size());
    out.setText(ctx.buf);
}

void fnTrim(FuncContext& ctx, int argc, Args argv, Value& out)
{
    trimImpl(ctx, argc, argv, out, true, true);
}
void fnLtrim(FuncContext& ctx, int argc, Args argv, Value& out)
{
    trimImpl(ctx, argc, argv, out, true, false);
}
void fnRtrim(FuncContext& ctx, int argc, Args argv, Value& out)
{
    trimImpl(ctx, argc, argv, out, false, true);
}

void fnMaxMin(FuncContext& ctx, int argc, Args argv, Value& out, bool max)
{
    const Value* best = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (argv[i]->isNull()) {
            out.setNull();
            return;
        }
        if (!best) {
            best = argv[i];
            continue;
        }
        const int c = compareValues(*argv[i], *best, ctx.collation);
        if (max ? c > 0 : c < 0)
            best = argv[i];
    }
    out = *best;
}

void fnMax(FuncContext& ctx, int argc, Args argv, Value& out)
{
    fnMaxMin(ctx, argc, argv, out, true);
}
void fnMin(FuncContext& ctx, int argc, Args argv, Value& out)
{
    fnMaxMin(ctx, argc, argv, out, false);
}

void fnNullif(FuncContext& ctx, int, Args argv, Value& out)
{
    if (!argv[0]->isNull() && !argv[1]->isNull() &&
        compareValues(*argv[0], *argv[1], ctx.collation) == 0) {
        out.setNull();
        return;
    }
    out = *argv[0];
}

void fnCoalesceEager(FuncContext&, int argc, Args argv, Value& out)
{
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]->isNull()) {
            out = *argv[i];
            return;
        }
    }
    out.setNull();
}

void fnIifEager(FuncContext&, int, Args argv, Value& out)
{
    out = isTrue(*argv[0]) ? *argv[1] : *argv[2];
}

void fnIdentity(FuncContext&, int, Args argv, Value& out)
{
    out = *argv[0];
}

void fnQuote(FuncContext& ctx, int, Args argv, Value& out)
{
    ctx.buf.clear();
    quoteValue(*argv[0], ctx.buf);
    out.setText(ctx.buf);
}

void fnRandom(FuncContext&, int, Args, Value& out)
{
    out.setInteger(std::int64_t(rng()()));
}

void fnRandomblob(FuncContext&, int, Args argv, Value& out)
{
    std::int64_t n = toInt64(*argv[0]);
    if (n < 1)
        n = 1;
    if (n > (1 << 30))
        throw Error(ErrorCode::Runtime, "string or blob too big");
    std::string bytes(std::size_t(n), '\0');
    for (char& b : bytes)
        b = char(rng()());
    out.setBlob(bytes.data(), bytes.size());
}

void fnReplace(FuncContext& ctx, int, Args argv, Value& out)
{
    if (anyNull(3, argv)) {
        out.setNull();
        return;
    }
    std::string a, b, c;
    const std::string_view s = textOf(*argv[0], a);
    const std::string_view from = textOf(*argv[1], b);
    // SQLite reads the pattern as a C string: one starting with NUL is empty.
    // An empty pattern hands the input back: numbers keep their type, a
    // blob comes back as text (SQLite has read it as text by then).
    if (from.empty() || from[0] == '\0') {
        if (argv[0]->type() == Type::Blob) {
            ctx.buf.assign(s.data(), s.size());
            out.setText(ctx.buf);
        } else {
            out = *argv[0];
        }
        return;
    }
    const std::string_view to = textOf(*argv[2], c);
    std::string& r = ctx.buf;
    r.clear();
    std::size_t i = 0;
    while (true) {
        const std::size_t at = s.find(from, i);
        if (at == std::string_view::npos)
            break;
        r.append(s.substr(i, at - i));
        r.append(to);
        i = at + from.size();
    }
    r.append(s.substr(i));
    out.setText(r);
}

void fnRound(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (argv[0]->isNull() || (argc > 1 && argv[1]->isNull())) {
        out.setNull();
        return;
    }
    std::int64_t n = argc > 1 ? toInt64(*argv[1]) : 0;
    if (n > 30)
        n = 30;
    if (n < 0)
        n = 0;
    double r = toDouble(*argv[0]);
    if (r < -4503599627370496.0 || r > 4503599627370496.0) {
        // Already an integer; nothing to round.
    } else if (n == 0) {
        r = r >= 0 ? double(std::int64_t(r + 0.5)) : -double(std::int64_t(-r + 0.5));
    } else {
        Value prec(n), val(r);
        const Value* args[2] = {&prec, &val};
        std::string s;
        sqlitePrintf(ctx, "%!.*f", 2, args, s);
        r = parseDouble(s.c_str(), nullptr);
    }
    out.setReal(r);
}

void fnSign(FuncContext&, int, Args argv, Value& out)
{
    const Value& v = *argv[0];
    double d;
    switch (v.type()) {
        case Type::Null: out.setNull(); return;
        case Type::Integer: out.setInteger(v.integer() > 0 ? 1 : (v.integer() < 0 ? -1 : 0)); return;
        case Type::Real: d = v.real(); break;
        case Type::Datetime: d = double(v.datetime().usec); break;
        default: {
            std::int64_t i = 0;
            switch (parseNumber(v.type() == Type::Text ? std::string_view(v.text()) : bytesView(v),
                                i, d, true)) {
                case NumKind::None: out.setNull(); return;
                case NumKind::Integer: d = double(i); break;
                case NumKind::Real: break;
            }
        }
    }
    out.setInteger(d > 0 ? 1 : (d < 0 ? -1 : 0));
}

void fnSoundex(FuncContext&, int, Args argv, Value& out)
{
    static const unsigned char kCode[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0, 1, 2, 6, 2, 3, 0, 1, 0, 2, 0,
        2, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0, 1, 2, 6, 2, 3, 0, 1, 0,
        2, 0, 2, 0, 0, 0, 0, 0,
    };
    std::string scratch;
    const std::string_view s = argv[0]->isNull() ? std::string_view() : cText(*argv[0], scratch);
    std::size_t i = 0;
    while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i])))
        ++i;
    char r[5] = "?000";
    if (i < s.size()) {
        unsigned char prev = kCode[static_cast<unsigned char>(s[i]) & 0x7f];
        r[0] = upperAscii(s[i]);
        int j = 1;
        for (++i; j < 4 && i < s.size(); ++i) {
            const unsigned char code = kCode[static_cast<unsigned char>(s[i]) & 0x7f];
            if (code > 0) {
                if (code != prev) {
                    r[j++] = char('0' + code);
                    prev = code;
                }
            } else {
                prev = 0;
            }
        }
        while (j < 4)
            r[j++] = '0';
        out.setText(std::string_view(r, 4));
        return;
    }
    out.setText("?000");
}

void fnSqliteVersion(FuncContext&, int, Args, Value& out)
{
    out.setText(kSqliteVersion);
}

void fnSourceId(FuncContext&, int, Args, Value& out)
{
    out.setText("libsql " + std::string(::sql::version()) + " (SQLite " + kSqliteVersion +
                " language)");
}

void fnLibsqlVersion(FuncContext&, int, Args, Value& out)
{
    out.setText(::sql::version());
}

const char* const kCompileOptions[] = {
    "ENABLE_MATH_FUNCTIONS", "ENABLE_JSON1", "OMIT_LOAD_EXTENSION", "SOUNDEX", "THREADSAFE=0",
    "DQS=0", "LIBSQL",
};

void fnCompileOptionUsed(FuncContext&, int, Args argv, Value& out)
{
    if (argv[0]->isNull()) {
        out.setNull();
        return;
    }
    std::string scratch;
    std::string_view name = textOf(*argv[0], scratch);
    if (name.size() > 7 && equalsNoCase(name.substr(0, 7), "SQLITE_"))
        name.remove_prefix(7);
    for (const char* o : kCompileOptions) {
        const std::string_view opt(o);
        const std::size_t eq = opt.find('=');
        if (equalsNoCase(opt, name) || (eq != std::string_view::npos && equalsNoCase(opt.substr(0, eq), name))) {
            out.setInteger(1);
            return;
        }
    }
    out.setInteger(0);
}

void fnCompileOptionGet(FuncContext&, int, Args argv, Value& out)
{
    const std::int64_t n = toInt64(*argv[0]);
    if (argv[0]->isNull() || n < 0 || n >= std::int64_t(std::size(kCompileOptions))) {
        out.setNull();
        return;
    }
    out.setText(kCompileOptions[n]);
}

/// substr(X, Y[, Z]) exactly as SQLite's substrFunc computes it.
void fnSubstr(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (anyNull(argc, argv)) {
        out.setNull();
        return;
    }
    const Value& v = *argv[0];
    std::int64_t p1 = toInt64(*argv[1]);
    std::int64_t p2 = 0;
    bool negP2 = false;
    if (argc == 3) {
        p2 = toInt64(*argv[2]);
        if (p2 < 0) {
            p2 = p2 == std::numeric_limits<std::int64_t>::min() ? std::numeric_limits<std::int64_t>::max() : -p2;
            negP2 = true;
        }
    } else {
        p2 = 1000000000;
    }
    const bool blob = v.type() == Type::Blob;
    std::string scratch;
    const std::string_view s = blob ? bytesView(v) : cText(v, scratch);
    // SQLite sees a zero-length blob as a null pointer and returns NULL.
    if (blob && s.empty()) {
        out.setNull();
        return;
    }
    std::int64_t len = blob ? std::int64_t(s.size()) : 0;
    if (p1 < 0) {
        if (!blob)
            len = std::int64_t(utf8Length(s));
        p1 += len;
        if (p1 < 0) {
            p2 += p1;
            if (p2 < 0)
                p2 = 0;
            p1 = 0;
        }
    } else if (p1 > 0) {
        --p1;
    } else if (p2 > 0) {
        --p2;
    }
    if (negP2) {
        p1 -= p2;
        if (p1 < 0) {
            p2 += p1;
            p1 = 0;
        }
    }
    if (blob) {
        if (p1 > std::int64_t(s.size()))
            p1 = std::int64_t(s.size());
        if (p1 + p2 > std::int64_t(s.size()))
            p2 = std::int64_t(s.size()) - p1;
        if (p2 < 0)
            p2 = 0;
        out.setBlob(s.data() + p1, std::size_t(p2));
        return;
    }
    std::size_t i = 0;
    for (std::int64_t k = 0; k < p1 && i < s.size(); ++k)
        utf8Next(s, i);
    const std::size_t start = i;
    for (std::int64_t k = 0; k < p2 && i < s.size(); ++k)
        utf8Next(s, i);
    ctx.buf.assign(s.data() + start, i - start);
    out.setText(ctx.buf);
}

void fnTypeof(FuncContext&, int, Args argv, Value& out)
{
    switch (argv[0]->type()) {
        case Type::Null: out.setText("null"); return;
        case Type::Integer: out.setText("integer"); return;
        case Type::Real: out.setText("real"); return;
        case Type::Datetime: out.setText("datetime"); return;
        case Type::Text: out.setText("text"); return;
        case Type::Blob: out.setText("blob"); return;
    }
}

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void fnUnhex(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (argv[0]->isNull() || (argc > 1 && argv[1]->isNull())) {
        out.setNull();
        return;
    }
    std::string a, b;
    const std::string_view s = textOf(*argv[0], a);
    const std::string_view ignore = argc > 1 ? textOf(*argv[1], b) : std::string_view();
    std::string& r = ctx.buf;
    r.clear();
    std::size_t i = 0;
    while (i < s.size()) {
        if (hexValue(s[i]) < 0) {
            std::size_t at = i;
            const std::uint32_t cp = utf8Next(s, at);
            bool skip = false;
            for (std::size_t k = 0; k < ignore.size();) {
                if (utf8Next(ignore, k) == cp)
                    skip = true;
            }
            if (!skip) {
                out.setNull();
                return;
            }
            i = at;
            continue;
        }
        if (i + 1 >= s.size() || hexValue(s[i + 1]) < 0) {
            out.setNull();
            return;
        }
        r += char(hexValue(s[i]) * 16 + hexValue(s[i + 1]));
        i += 2;
    }
    out.setBlob(r.data(), r.size());
}

void fnUnicode(FuncContext&, int, Args argv, Value& out)
{
    std::string scratch;
    const std::string_view s = argv[0]->isNull() ? std::string_view() : cText(*argv[0], scratch);
    if (s.empty()) {
        out.setNull();
        return;
    }
    std::size_t i = 0;
    out.setInteger(std::int64_t(utf8Next(s, i)));
}

void fnZeroblob(FuncContext&, int, Args argv, Value& out)
{
    std::int64_t n = toInt64(*argv[0]);
    if (n < 0)
        n = 0;
    if (n > (1 << 30))
        throw Error(ErrorCode::Runtime, "string or blob too big");
    const std::string zeros(std::size_t(n), '\0');
    out.setBlob(zeros.data(), zeros.size());
}

void fnLike(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (anyNull(argc, argv)) {
        out.setNull();
        return;
    }
    std::string a, b, c;
    const std::string_view pat = cString(textOf(*argv[0], a)), str = cString(textOf(*argv[1], b));
    std::uint32_t esc = 0;
    if (argc == 3) {
        const std::string_view e = textOf(*argv[2], c);
        std::size_t i = 0;
        if (e.empty() || (esc = utf8Next(e, i), i != e.size()))
            ctx.error("ESCAPE expression must be a single character");
    }
    const bool cs = ctx.conn && ctx.conn->caseSensitiveLike;
    out.setInteger(likeMatch(pat, str, esc, cs) ? 1 : 0);
}

void fnGlob(FuncContext&, int, Args argv, Value& out)
{
    if (anyNull(2, argv)) {
        out.setNull();
        return;
    }
    std::string a, b;
    out.setInteger(globMatch(cString(textOf(*argv[0], a)), cString(textOf(*argv[1], b))) ? 1 : 0);
}

void fnLoadExtension(FuncContext& ctx, int, Args, Value&)
{
    ctx.error("not authorized");
}

// -------------------------------------------------------------- math ----

/// Math functions see only numbers: NULL or non-numeric text gives NULL.
bool mathArg(const Value& v, double& d)
{
    switch (v.type()) {
        case Type::Integer: d = double(v.integer()); return true;
        case Type::Real: d = v.real(); return true;
        case Type::Datetime: d = double(v.datetime().usec); return true;
        case Type::Text: {
            std::int64_t i = 0;
            switch (parseNumber(v.text(), i, d, true)) {
                case NumKind::Integer: d = double(i); return true;
                case NumKind::Real: return true;
                case NumKind::None: return false;
            }
            return false;
        }
        default: return false;
    }
}

void mathResult(double r, Value& out)
{
    if (std::isnan(r))
        out.setNull();
    else
        out.setReal(r);
}

template <double (*F)(double)>
void math1(FuncContext&, int, Args argv, Value& out)
{
    double d;
    if (!mathArg(*argv[0], d)) {
        out.setNull();
        return;
    }
    mathResult(F(d), out);
}

template <double (*F)(double, double)>
void math2(FuncContext&, int, Args argv, Value& out)
{
    double a, b;
    if (!mathArg(*argv[0], a) || !mathArg(*argv[1], b)) {
        out.setNull();
        return;
    }
    mathResult(F(a, b), out);
}

double degrees(double x)
{
    return x * 180.0 / M_PI;
}
double radians(double x)
{
    return x * M_PI / 180.0;
}
double ln(double x)
{
    return std::log(x);
}
double powFn(double a, double b)
{
    return std::pow(a, b);
}
double modFn(double a, double b)
{
    return std::fmod(a, b);
}
double atan2Fn(double a, double b)
{
    return std::atan2(a, b);
}
double sinFn(double x) { return std::sin(x); }
double cosFn(double x) { return std::cos(x); }
double tanFn(double x) { return std::tan(x); }
double asinFn(double x) { return std::asin(x); }
double acosFn(double x) { return std::acos(x); }
double atanFn(double x) { return std::atan(x); }
double sinhFn(double x) { return std::sinh(x); }
double coshFn(double x) { return std::cosh(x); }
double tanhFn(double x) { return std::tanh(x); }
double asinhFn(double x) { return std::asinh(x); }
double acoshFn(double x) { return std::acosh(x); }
double atanhFn(double x) { return std::atanh(x); }
double expFn(double x) { return std::exp(x); }
double sqrtFn(double x) { return std::sqrt(x); }
double log10Fn(double x) { return std::log10(x); }
double log2Fn(double x) { return std::log2(x); }

/// ceil, floor, trunc keep integers as integers.
template <double (*F)(double)>
void integral(FuncContext&, int, Args argv, Value& out)
{
    const Value& v = *argv[0];
    if (v.type() == Type::Integer) {
        out.setInteger(v.integer());
        return;
    }
    double d;
    if (!mathArg(v, d)) {
        out.setNull();
        return;
    }
    mathResult(F(d), out);
}

double ceilFn(double x) { return std::ceil(x); }
double floorFn(double x) { return std::floor(x); }
double truncFn(double x) { return std::trunc(x); }

void fnLog(FuncContext&, int argc, Args argv, Value& out)
{
    double x, b = 10;
    if (argc == 2) {
        if (!mathArg(*argv[0], b) || !mathArg(*argv[1], x)) {
            out.setNull();
            return;
        }
        if (b <= 0 || b == 1 || x <= 0) {
            out.setNull();
            return;
        }
        mathResult(std::log(x) / std::log(b), out);
        return;
    }
    if (!mathArg(*argv[0], x) || x <= 0) {
        out.setNull();
        return;
    }
    mathResult(std::log10(x), out);
}

void fnPi(FuncContext&, int, Args, Value& out)
{
    out.setReal(M_PI);
}

void add(FunctionRegistry& r, const char* name, int minArgs, int maxArgs, ScalarFn fn,
         unsigned flags = kDeterministic)
{
    FunctionDef d;
    d.name = name;
    d.minArgs = minArgs;
    d.maxArgs = maxArgs;
    d.flags = flags;
    d.scalar = fn;
    r.add(std::move(d));
}

}  // namespace

void quoteValue(const Value& v, std::string& out)
{
    switch (v.type()) {
        case Type::Null: out += "NULL"; return;
        case Type::Integer:
        case Type::Datetime:
            if (v.type() == Type::Datetime) {
                out += '\'';
                out += formatDatetime(v.datetime());
                out += '\'';
                return;
            }
            appendText(v, out);
            return;
        case Type::Real: {
            const double d = v.real();
            std::string s;
            realToText(d, s);
            if (std::isfinite(d) && parseDouble(s.c_str(), nullptr) != d) {
                char buf[64];
                const int n = formatDouble(buf, sizeof buf, "%.20e", d);
                s.assign(buf, std::size_t(n));
            }
            out += s;
            return;
        }
        case Type::Text: {
            out += '\'';
            for (const char ch : v.text()) {
                if (ch == '\0')
                    break;
                out += ch;
                if (ch == '\'')
                    out += '\'';
            }
            out += '\'';
            return;
        }
        case Type::Blob: {
            static const char* kDigits = "0123456789ABCDEF";
            out += "X'";
            for (const std::byte b : v.blob()) {
                out += kDigits[std::uint8_t(b) >> 4];
                out += kDigits[std::uint8_t(b) & 15];
            }
            out += '\'';
            return;
        }
    }
}

void registerCoreFunctions(FunctionRegistry& r)
{
    add(r, "abs", 1, 1, fnAbs);
    add(r, "changes", 0, 0, fnChanges, 0);
    add(r, "char", 0, -1, fnChar);
    add(r, "coalesce", 2, -1, fnCoalesceEager);
    add(r, "concat", 1, -1, fnConcat);
    add(r, "concat_ws", 2, -1, fnConcatWs);
    add(r, "glob", 2, 2, fnGlob);
    add(r, "hex", 1, 1, fnHex);
    add(r, "storage_key", 1, 1, fnStorageKey);
    add(r, "storage_row", 1, 1, fnStorageRow);
    add(r, "storage_rowid", 1, 1, fnStorageRowid);
    add(r, "ifnull", 2, 2, fnCoalesceEager);
    add(r, "iif", 3, 3, fnIifEager);
    add(r, "instr", 2, 2, fnInstr, kDeterministic | kNeedsCollation);
    add(r, "last_insert_rowid", 0, 0, fnLastInsertRowid, 0);
    add(r, "length", 1, 1, fnLength);
    add(r, "like", 2, 3, fnLike);
    add(r, "likelihood", 2, 2, fnIdentity);
    add(r, "likely", 1, 1, fnIdentity);
    add(r, "load_extension", 1, 2, fnLoadExtension, 0);
    add(r, "lower", 1, 1, fnLower);
    add(r, "ltrim", 1, 2, fnLtrim);
    add(r, "max", 2, -1, fnMax, kDeterministic | kNeedsCollation);
    add(r, "min", 2, -1, fnMin, kDeterministic | kNeedsCollation);
    add(r, "nullif", 2, 2, fnNullif, kDeterministic | kNeedsCollation);
    add(r, "octet_length", 1, 1, fnOctetLength);
    add(r, "quote", 1, 1, fnQuote);
    add(r, "random", 0, 0, fnRandom, 0);
    add(r, "randomblob", 1, 1, fnRandomblob, 0);
    add(r, "replace", 3, 3, fnReplace);
    add(r, "round", 1, 2, fnRound);
    add(r, "rtrim", 1, 2, fnRtrim);
    add(r, "sign", 1, 1, fnSign);
    add(r, "soundex", 1, 1, fnSoundex);
    add(r, "sqlite_compileoption_get", 1, 1, fnCompileOptionGet);
    add(r, "sqlite_compileoption_used", 1, 1, fnCompileOptionUsed);
    add(r, "sqlite_source_id", 0, 0, fnSourceId);
    add(r, "sqlite_version", 0, 0, fnSqliteVersion);
    add(r, "libsql_version", 0, 0, fnLibsqlVersion);
    add(r, "substr", 2, 3, fnSubstr);
    add(r, "substring", 2, 3, fnSubstr);
    add(r, "total_changes", 0, 0, fnTotalChanges, 0);
    add(r, "trim", 1, 2, fnTrim);
    add(r, "typeof", 1, 1, fnTypeof);
    add(r, "unhex", 1, 2, fnUnhex);
    add(r, "unicode", 1, 1, fnUnicode);
    add(r, "unlikely", 1, 1, fnIdentity);
    add(r, "upper", 1, 1, fnUpper);
    add(r, "zeroblob", 1, 1, fnZeroblob);
}

void registerMathFunctions(FunctionRegistry& r)
{
    add(r, "acos", 1, 1, math1<acosFn>);
    add(r, "acosh", 1, 1, math1<acoshFn>);
    add(r, "asin", 1, 1, math1<asinFn>);
    add(r, "asinh", 1, 1, math1<asinhFn>);
    add(r, "atan", 1, 1, math1<atanFn>);
    add(r, "atan2", 2, 2, math2<atan2Fn>);
    add(r, "atanh", 1, 1, math1<atanhFn>);
    add(r, "ceil", 1, 1, integral<ceilFn>);
    add(r, "ceiling", 1, 1, integral<ceilFn>);
    add(r, "cos", 1, 1, math1<cosFn>);
    add(r, "cosh", 1, 1, math1<coshFn>);
    add(r, "degrees", 1, 1, math1<degrees>);
    add(r, "exp", 1, 1, math1<expFn>);
    add(r, "floor", 1, 1, integral<floorFn>);
    add(r, "ln", 1, 1, math1<ln>);
    add(r, "log", 1, 2, fnLog);
    add(r, "log10", 1, 1, math1<log10Fn>);
    add(r, "log2", 1, 1, math1<log2Fn>);
    add(r, "mod", 2, 2, math2<modFn>);
    add(r, "pi", 0, 0, fnPi);
    add(r, "pow", 2, 2, math2<powFn>);
    add(r, "power", 2, 2, math2<powFn>);
    add(r, "radians", 1, 1, math1<radians>);
    add(r, "sin", 1, 1, math1<sinFn>);
    add(r, "sinh", 1, 1, math1<sinhFn>);
    add(r, "sqrt", 1, 1, math1<sqrtFn>);
    add(r, "tan", 1, 1, math1<tanFn>);
    add(r, "tanh", 1, 1, math1<tanhFn>);
    add(r, "trunc", 1, 1, integral<truncFn>);
}

}  // namespace sql::internal
