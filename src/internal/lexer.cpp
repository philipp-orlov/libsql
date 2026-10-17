// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tokenizer. Comments and whitespace vanish here; everything else becomes a
// token carrying enough information that the parser never re-reads the source.

#include "internal/lexer.hpp"


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

/// The words the lexer flags: reserved (SQLite keywords without %fallback,
/// never a bare name), join words, and the operator words of the expression
/// grammar. Looked up for every identifier, through a small open-addressing
/// table keyed by the token's folded spelling.
struct WordInfo
{
    const char* word;
    bool reserved;
    bool join;
    std::uint16_t ops;
};

constexpr WordInfo kWordList[] = {
    {"ADD", true, false, 0},          {"ALL", true, false, 0},
    {"ALTER", true, false, 0},        {"AND", true, false, kOpAnd},
    {"AS", true, false, 0},           {"AUTOINCREMENT", true, false, 0},
    {"BETWEEN", true, false, kOpEquality}, {"CASE", true, false, 0},
    {"CHECK", true, false, 0},        {"COLLATE", true, false, kOpCollate},
    {"COMMIT", true, false, 0},       {"CONSTRAINT", true, false, 0},
    {"CREATE", true, false, 0},       {"DEFAULT", true, false, 0},
    {"DEFERRABLE", true, false, 0},   {"DELETE", true, false, 0},
    {"DISTINCT", true, false, 0},     {"DROP", true, false, 0},
    {"ELSE", true, false, 0},         {"ESCAPE", true, false, 0},
    {"EXCEPT", true, false, 0},       {"EXISTS", true, false, 0},
    {"FOREIGN", true, false, 0},      {"FROM", true, false, 0},
    {"GROUP", true, false, 0},        {"HAVING", true, false, 0},
    {"IN", true, false, kOpEquality}, {"INDEX", true, false, 0},
    {"INSERT", true, false, 0},       {"INTERSECT", true, false, 0},
    {"INTO", true, false, 0},         {"IS", true, false, kOpEquality},
    {"ISNULL", true, false, kOpEquality}, {"JOIN", true, false, 0},
    {"LIMIT", true, false, 0},        {"NOT", true, false, kOpEquality},
    {"NOTHING", true, false, 0},      {"NOTNULL", true, false, kOpEquality},
    {"NULL", true, false, 0},         {"ON", true, false, 0},
    {"OR", true, false, kOpOr},       {"ORDER", true, false, 0},
    {"PRIMARY", true, false, 0},      {"REFERENCES", true, false, 0},
    {"SELECT", true, false, 0},       {"SET", true, false, 0},
    {"TABLE", true, false, 0},        {"THEN", true, false, 0},
    {"TO", true, false, 0},           {"TRANSACTION", true, false, 0},
    {"UNION", true, false, 0},        {"UNIQUE", true, false, 0},
    {"UPDATE", true, false, 0},       {"USING", true, false, 0},
    {"VALUES", true, false, 0},       {"WHEN", true, false, 0},
    {"WHERE", true, false, 0},
    {"NATURAL", false, true, 0},      {"LEFT", false, true, 0},
    {"RIGHT", false, true, 0},        {"FULL", false, true, 0},
    {"OUTER", false, true, 0},        {"CROSS", false, true, 0},
    {"INNER", false, true, 0},
    {"LIKE", false, false, kOpEquality}, {"GLOB", false, false, kOpEquality},
    {"REGEXP", false, false, kOpEquality}, {"MATCH", false, false, kOpEquality},
};

constexpr std::size_t kWordSlots = 256;  // a power of two, under a third full

constexpr std::size_t wordSlot(std::uint64_t fold, std::size_t length) noexcept
{
    std::uint64_t h = (fold ^ (std::uint64_t(length) << 56)) * 0x9E3779B97F4A7C15ull;
    return std::size_t(h >> 56) & (kWordSlots - 1);
}

constexpr std::size_t cstrlen(const char* s) noexcept
{
    std::size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

class WordTable
{
public:
    WordTable() noexcept
    {
        for (std::int16_t& s : slots_)
            s = -1;
        for (std::size_t i = 0; i < sizeof kWordList / sizeof kWordList[0]; ++i) {
            const std::size_t n = cstrlen(kWordList[i].word);
            const std::uint64_t f = Token::foldWord(kWordList[i].word, n);
            std::size_t at = wordSlot(f, n);
            while (slots_[at] >= 0)
                at = (at + 1) & (kWordSlots - 1);
            slots_[at] = std::int16_t(i);
            folds_[at] = f;
            lengths_[at] = std::uint8_t(n);
        }
    }

    /// The entry spelled `w` (any case), or null. `fold` is foldWord(w).
    const WordInfo* find(std::string_view w, std::uint64_t fold) const noexcept
    {
        if (w.empty() || w.size() > 15)
            return nullptr;
        for (std::size_t at = wordSlot(fold, w.size());; at = (at + 1) & (kWordSlots - 1)) {
            const std::int16_t i = slots_[at];
            if (i < 0)
                return nullptr;
            if (folds_[at] == fold && lengths_[at] == w.size() &&
                (w.size() <= 8 || equalsNoCase(w.substr(8), std::string_view(kWordList[i].word + 8))))
                return &kWordList[i];
        }
    }

private:
    std::int16_t slots_[kWordSlots];
    std::uint64_t folds_[kWordSlots] = {};
    std::uint8_t lengths_[kWordSlots] = {};
};

const WordTable& words() noexcept
{
    static const WordTable table;
    return table;
}

const WordInfo* lookupWord(std::string_view w) noexcept
{
    return words().find(w, Token::foldWord(w.data(), w.size()));
}

}  // namespace

bool isReservedWord(std::string_view w) noexcept
{
    const WordInfo* info = lookupWord(w);
    return info && info->reserved;
}

bool isJoinWord(std::string_view w) noexcept
{
    const WordInfo* info = lookupWord(w);
    return info && info->join;
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
            t.fold = Token::foldWord(t.text.data(), t.text.size());
            if (const WordInfo* info = words().find(t.text, t.fold)) {
                if (!t.quoted)
                    t.ops = info->ops;
                t.reserved = info->reserved;
                t.joinWord = info->join;
            }
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
