// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tokenizer. Comments and whitespace vanish here; everything else becomes a
// token carrying enough information that the parser never re-reads the source.

#include "internal/lexer.hpp"

#include <unordered_set>

#include "internal/cnumeric.hpp"
#include "internal/types.hpp"

namespace sql::internal {

namespace {

[[noreturn]] void unrecognized(std::string_view text, std::size_t at, std::size_t end)
{
    throw Error(ErrorCode::SyntaxError,
                "unrecognized token: \"" + std::string(text.substr(at, end - at)) + "\"");
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool isIdentStart(char c) noexcept
{
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u >= 0x80;
}

bool isIdentChar(char c) noexcept
{
    return isIdentStart(c) || isDigit(c) || c == '$';
}

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int hexDigit(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// Reads the body of a quoted run, honoring the doubled-delimiter escape.
/// Returns false when the run is unterminated.
bool readQuoted(std::string_view text, std::size_t& i, char close, std::string& out)
{
    ++i;  // the opening delimiter
    std::size_t run = i;
    for (;;) {
        if (i >= text.size())
            return false;
        if (text[i] == close) {
            out.append(text.data() + run, i - run);
            if (i + 1 < text.size() && text[i + 1] == close) {
                out += close;
                i += 2;
                run = i;
                continue;
            }
            ++i;
            return true;
        }
        ++i;
    }
}

}  // namespace

namespace {

std::uint16_t punctOps(std::string_view p) noexcept
{
    if (p == "=" || p == "==" || p == "<>" || p == "!=")
        return kOpEquality;
    if (p == "<" || p == "<=" || p == ">" || p == ">=")
        return kOpRelational;
    if (p == "&" || p == "|" || p == "<<" || p == ">>")
        return kOpBitwise;
    if (p == "+" || p == "-")
        return kOpAdditive;
    if (p == "*" || p == "/" || p == "%")
        return kOpMultiplicative;
    if (p == "||" || p == "->" || p == "->>")
        return kOpConcat;
    return 0;
}

std::uint16_t keywordOps(std::string_view w) noexcept
{
    if (w.size() < 2 || w.size() > 7)
        return 0;
    switch (lowerAscii(w[0])) {
        case 'o': return equalsNoCase(w, "OR") ? kOpOr : 0;
        case 'a': return equalsNoCase(w, "AND") ? kOpAnd : 0;
        case 'c': return equalsNoCase(w, "COLLATE") ? kOpCollate : 0;
        case 'i':
            return (equalsNoCase(w, "IS") || equalsNoCase(w, "IN") || equalsNoCase(w, "ISNULL")) ? kOpEquality : 0;
        case 'n': return (equalsNoCase(w, "NOT") || equalsNoCase(w, "NOTNULL")) ? kOpEquality : 0;
        case 'b': return equalsNoCase(w, "BETWEEN") ? kOpEquality : 0;
        case 'l': return equalsNoCase(w, "LIKE") ? kOpEquality : 0;
        case 'g': return equalsNoCase(w, "GLOB") ? kOpEquality : 0;
        case 'r': return equalsNoCase(w, "REGEXP") ? kOpEquality : 0;
        case 'm': return equalsNoCase(w, "MATCH") ? kOpEquality : 0;
        default: return 0;
    }
}

}  // namespace

/// Lowercases `w` into `buf` when it is short enough to be a keyword.
static bool lowerWord(std::string_view w, char (&buf)[16], std::string_view& out) noexcept
{
    if (w.empty() || w.size() >= sizeof buf)
        return false;
    for (std::size_t i = 0; i < w.size(); ++i)
        buf[i] = lowerAscii(w[i]);
    out = std::string_view(buf, w.size());
    return true;
}

/// Words that can never be a bare name: SQLite keywords without %fallback.
/// Asked for nearly every identifier, so it is a hash lookup.
bool isReservedWord(std::string_view w) noexcept
{
    static const std::unordered_set<std::string_view> kWords = {
        "add",       "all",       "alter",      "and",       "as",         "autoincrement",
        "between",   "case",      "check",      "collate",   "commit",     "constraint",
        "create",    "default",   "deferrable", "delete",    "distinct",   "drop",
        "else",      "escape",    "except",     "exists",    "foreign",    "from",
        "group",     "having",    "in",         "index",     "insert",     "intersect",
        "into",      "is",        "isnull",     "join",      "limit",      "not",
        "nothing",   "notnull",   "null",       "on",        "or",         "order",
        "primary",   "references", "select",    "set",       "table",      "then",
        "to",        "transaction", "union",    "unique",    "update",     "using",
        "values",    "when",      "where",
    };
    char buf[16];
    std::string_view lower;
    return lowerWord(w, buf, lower) && kWords.count(lower) != 0;
}

bool isJoinWord(std::string_view w) noexcept
{
    static const std::unordered_set<std::string_view> kWords = {"natural", "left", "right", "full",
                                                                 "outer", "cross", "inner"};
    char buf[16];
    std::string_view lower;
    return lowerWord(w, buf, lower) && kWords.count(lower) != 0;
}

std::vector<Token> tokenize(std::string_view text)
{
    std::vector<Token> out;
    tokenizeInto(text, out);
    return out;
}

void tokenizeInto(std::string_view text, std::vector<Token>& out)
{
    out.clear();
    out.reserve(text.size() / 3 + 4);
    std::size_t i = 0;

    while (i < text.size()) {
        const char c = text[i];

        if (isSpace(c)) {
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            // SQLite lets an unterminated block comment run to the end.
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/'))
                ++i;
            i = i + 1 < text.size() ? i + 2 : text.size();
            continue;
        }

        Token& t = out.emplace_back();
        t.offset = std::uint32_t(i);
        const std::size_t start = i;

        // X'0011ff' -- a blob literal, checked before the identifier rule.
        if ((c == 'x' || c == 'X') && i + 1 < text.size() && text[i + 1] == '\'') {
            ++i;
            std::string hex;
            if (!readQuoted(text, i, '\'', hex) || hex.size() % 2 != 0)
                unrecognized(text, start, i);
            Blob bytes(hex.size() / 2);
            for (std::size_t k = 0; k < bytes.size(); ++k) {
                const int hi = hexDigit(hex[k * 2]), lo = hexDigit(hex[k * 2 + 1]);
                if (hi < 0 || lo < 0)
                    unrecognized(text, start, i);
                bytes[k] = std::byte(hi * 16 + lo);
            }
            t.kind = Tok::BlobLiteral;
            t.value = Value(std::move(bytes));
        } else if (isIdentStart(c)) {
            while (i < text.size() && isIdentChar(text[i]))
                ++i;
            t.kind = Tok::Identifier;
            t.text.assign(text.data() + (start), i - start);
        } else if (c == '"' || c == '`') {
            t.kind = Tok::Identifier;
            t.quoted = true;
            if (!readQuoted(text, i, c, t.text))
                unrecognized(text, start, text.size());
        } else if (c == '[') {
            while (i < text.size() && text[i] != ']')
                ++i;
            if (i >= text.size())
                unrecognized(text, start, text.size());
            t.kind = Tok::Identifier;
            t.quoted = true;
            t.text.assign(text.data() + (start + 1), i - start - 1);
            ++i;
        } else if (c == '\'') {
            std::string s;
            if (!readQuoted(text, i, '\'', s))
                unrecognized(text, start, text.size());
            t.kind = Tok::String;
            t.value = Value(std::move(s));
        } else if (isDigit(c) || (c == '.' && i + 1 < text.size() && isDigit(text[i + 1]))) {
            t.kind = Tok::Number;
            if (c == '0' && i + 2 < text.size() + 0 && (text[i + 1] == 'x' || text[i + 1] == 'X') &&
                i + 2 < text.size() && hexDigit(text[i + 2]) >= 0) {
                i += 2;
                std::uint64_t v = 0;
                std::size_t digits = 0;
                while (i < text.size() && hexDigit(text[i]) >= 0) {
                    v = (v << 4) | std::uint64_t(hexDigit(text[i]));
                    ++digits;
                    ++i;
                }
                if (i < text.size() && isIdentChar(text[i])) {
                    while (i < text.size() && isIdentChar(text[i]))
                        ++i;
                    unrecognized(text, start, i);
                }
                if (digits > 16)
                    throw Error(ErrorCode::SyntaxError,
                                "hex literal too big: " + std::string(text.substr(start, i - start)));
                t.value = Value(std::int64_t(v));
            } else {
                bool fractional = false;
                while (i < text.size() && isDigit(text[i]))
                    ++i;
                if (i < text.size() && text[i] == '.') {
                    fractional = true;
                    ++i;
                    while (i < text.size() && isDigit(text[i]))
                        ++i;
                }
                if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                    std::size_t probe = i + 1;
                    if (probe < text.size() && (text[probe] == '+' || text[probe] == '-'))
                        ++probe;
                    if (probe < text.size() && isDigit(text[probe])) {
                        fractional = true;
                        i = probe;
                        while (i < text.size() && isDigit(text[i]))
                            ++i;
                    } else {
                        unrecognized(text, start, probe);
                    }
                }
                if (i < text.size() && isIdentChar(text[i])) {
                    while (i < text.size() && isIdentChar(text[i]))
                        ++i;
                    unrecognized(text, start, i);
                }
                const std::string_view digits = text.substr(start, i - start);
                std::int64_t iv = 0;
                double dv = 0;
                if (!fractional && parseNumber(digits, iv, dv, true) == NumKind::Integer) {
                    t.value = Value(iv);
                } else {
                    parseNumber(digits, iv, dv, true);
                    t.value = Value(dv);
                    t.minInt = !fractional && digits == "9223372036854775808";
                }
            }
        } else if (c == '?') {
            ++i;
            while (i < text.size() && isDigit(text[i]))
                ++i;
            t.kind = Tok::Parameter;
            t.text.assign(text.data() + (start), i - start);
        } else if (c == ':' || c == '@' || c == '$' || c == '#') {
            ++i;
            while (i < text.size() && isIdentChar(text[i]))
                ++i;
            // TCL-style $a::b(c) suffixes, which SQLite also accepts.
            if (c == '$') {
                while (i + 1 < text.size() && text[i] == ':' && text[i + 1] == ':') {
                    i += 2;
                    while (i < text.size() && isIdentChar(text[i]))
                        ++i;
                }
                if (i < text.size() && text[i] == '(') {
                    while (i < text.size() && text[i] != ')' && !isSpace(text[i]))
                        ++i;
                    if (i < text.size() && text[i] == ')')
                        ++i;
                }
            }
            if (i == start + 1)
                unrecognized(text, start, i);
            t.kind = Tok::Parameter;
            t.text.assign(text.data() + (start), i - start);
        } else {
            // Longest operator first: ->> then the two-character ones.
            t.kind = Tok::Punct;
            const char n1 = i + 1 < text.size() ? text[i + 1] : '\0';
            const char n2 = i + 2 < text.size() ? text[i + 2] : '\0';
            std::size_t len = 1;
            switch (c) {
                case '-': len = n1 == '>' ? (n2 == '>' ? 3 : 2) : 1; break;
                case '<': len = (n1 == '>' || n1 == '=' || n1 == '<') ? 2 : 1; break;
                case '>': len = (n1 == '=' || n1 == '>') ? 2 : 1; break;
                case '!':
                    if (n1 != '=')
                        unrecognized(text, start, start + 1);
                    len = 2;
                    break;
                case '=': len = n1 == '=' ? 2 : 1; break;
                case '|': len = n1 == '|' ? 2 : 1; break;
                case '(': case ')': case ',': case '.': case ';': case '*': case '+':
                case '/': case '%': case '&': case '~': break;
                default: unrecognized(text, start, start + 1);
            }
            t.text.assign(text.data() + i, len);
            i += len;
        }
        t.length = std::uint32_t(i - start);
        if (t.kind == Tok::Punct)
            t.ops = punctOps(t.text);
        else if (t.kind == Tok::Identifier) {
            if (!t.quoted)
                t.ops = keywordOps(t.text);
            t.reserved = isReservedWord(t.text);
            t.joinWord = !t.reserved && isJoinWord(t.text);
        }
    }

    Token end;
    end.offset = std::uint32_t(text.size());
    out.push_back(std::move(end));
}

bool isCompleteStatement(std::string_view text)
{
    // A token-level version of sqlite3_complete: track whether the last
    // significant token is a ';' outside a CREATE TRIGGER body.
    std::vector<Token> toks;
    try {
        toks = tokenize(text);
    } catch (const Error&) {
        // An unterminated quote is incomplete; anything else is "complete"
        // in the sense that running it will report the error.
        for (char q : {'\'', '"', '`'}) {
            std::size_t n = 0;
            for (char ch : text)
                n += ch == q;
            if (n % 2)
                return false;
        }
        return true;
    }
    // State machine over tokens: in a trigger, `;` only completes after END.
    bool sawSemi = false;
    int state = 0;  // 0 normal, 1 saw CREATE, 2 saw CREATE [TEMP] TRIGGER, 3 in body
    for (std::size_t k = 0; k + 1 < toks.size(); ++k) {
        const Token& t = toks[k];
        sawSemi = false;
        if (t.isPunct(";")) {
            if (state == 3) {
                continue;
            }
            state = 0;
            sawSemi = true;
            continue;
        }
        switch (state) {
            case 0:
                if (t.isKeyword("CREATE"))
                    state = 1;
                break;
            case 1:
                if (t.isKeyword("TRIGGER"))
                    state = 2;
                else if (!(t.isKeyword("TEMP") || t.isKeyword("TEMPORARY")))
                    state = 4;
                break;
            case 2:
                if (t.isKeyword("BEGIN"))
                    state = 3;
                break;
            case 3:
                if (t.isKeyword("END") && k + 1 < toks.size() &&
                    (toks[k + 1].isPunct(";") || toks[k + 1].kind == Tok::End)) {
                    // END closes the body only when a ';' follows.
                    if (toks[k + 1].isPunct(";")) {
                        state = 0;
                        sawSemi = true;
                        ++k;
                    }
                }
                break;
            default: break;
        }
    }
    return sawSemi;
}

}  // namespace sql::internal
