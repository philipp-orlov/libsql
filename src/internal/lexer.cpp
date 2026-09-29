// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tokenizer. Comments and whitespace vanish here; everything else becomes a
// token carrying enough information that the parser never re-reads the source.

#include "internal/lexer.hpp"

#include "internal/cnumeric.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace sql::internal {

bool equalsNoCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto x = std::toupper(static_cast<unsigned char>(a[i]));
        const auto y = std::toupper(static_cast<unsigned char>(b[i]));
        if (x != y)
            return false;
    }
    return true;
}

std::string toLower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool Token::isKeyword(std::string_view word) const noexcept
{
    return kind == Tok::Identifier && !quoted && equalsNoCase(text, word);
}

namespace {

[[noreturn]] void syntax(const std::string& what, std::size_t at)
{
    throw Error(ErrorCode::SyntaxError, what + " at offset " + std::to_string(at));
}

bool isIdentStart(char c) noexcept
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
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

/// Reads the body of a quoted run, honouring the doubled-delimiter escape.
std::string readQuoted(std::string_view text, std::size_t& i, char close)
{
    const std::size_t open = i;
    std::string out;
    ++i;  // the opening delimiter
    for (;;) {
        if (i >= text.size())
            syntax("unterminated quoted literal", open);
        if (text[i] == close) {
            if (i + 1 < text.size() && text[i + 1] == close) {
                out += close;
                i += 2;
                continue;
            }
            ++i;
            return out;
        }
        out += text[i++];
    }
}

}  // namespace

std::vector<Token> tokenize(std::string_view text)
{
    std::vector<Token> out;
    std::size_t i = 0;

    while (i < text.size()) {
        const char c = text[i];

        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t open = i;
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/'))
                ++i;
            if (i + 1 >= text.size())
                syntax("unterminated block comment", open);
            i += 2;
            continue;
        }

        Token t;
        t.offset = i;

        // X'0011ff' -- a blob literal, checked before the identifier rule.
        if ((c == 'x' || c == 'X') && i + 1 < text.size() && text[i + 1] == '\'') {
            ++i;
            const std::size_t open = i;
            const std::string hex = readQuoted(text, i, '\'');
            if (hex.size() % 2 != 0)
                syntax("blob literal needs an even number of hex digits", open);
            Blob bytes(hex.size() / 2);
            for (std::size_t k = 0; k < bytes.size(); ++k) {
                const int hi = hexDigit(hex[k * 2]), lo = hexDigit(hex[k * 2 + 1]);
                if (hi < 0 || lo < 0)
                    syntax("blob literal has a non-hex digit", open);
                bytes[k] = std::byte(hi * 16 + lo);
            }
            t.kind = Tok::BlobLiteral;
            t.value = Value(std::move(bytes));
            out.push_back(std::move(t));
            continue;
        }

        if (isIdentStart(c)) {
            const std::size_t start = i;
            while (i < text.size() && isIdentChar(text[i]))
                ++i;
            t.kind = Tok::Identifier;
            t.text = std::string(text.substr(start, i - start));
            out.push_back(std::move(t));
            continue;
        }

        if (c == '"' || c == '`') {
            t.kind = Tok::Identifier;
            t.quoted = true;
            t.text = readQuoted(text, i, c);
            out.push_back(std::move(t));
            continue;
        }
        if (c == '[') {
            const std::size_t start = ++i;
            while (i < text.size() && text[i] != ']')
                ++i;
            if (i >= text.size())
                syntax("unterminated [identifier]", t.offset);
            t.kind = Tok::Identifier;
            t.quoted = true;
            t.text = std::string(text.substr(start, i - start));
            ++i;
            out.push_back(std::move(t));
            continue;
        }

        if (c == '\'') {
            t.kind = Tok::String;
            t.value = Value(readQuoted(text, i, '\''));
            out.push_back(std::move(t));
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            const std::size_t start = i;
            bool fractional = false;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
                ++i;
            if (i < text.size() && text[i] == '.') {
                fractional = true;
                ++i;
                while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
                    ++i;
            }
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                std::size_t probe = i + 1;
                if (probe < text.size() && (text[probe] == '+' || text[probe] == '-'))
                    ++probe;
                if (probe < text.size() && std::isdigit(static_cast<unsigned char>(text[probe]))) {
                    fractional = true;
                    i = probe;
                    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
                        ++i;
                }
            }
            const std::string digits(text.substr(start, i - start));
            t.kind = Tok::Number;
            if (fractional) {
                t.value = Value(parseDouble(digits.c_str(), nullptr));
            } else {
                errno = 0;
                char* end = nullptr;
                const long long v = std::strtoll(digits.c_str(), &end, 10);
                // Integers that do not fit stay exact-ish as reals rather than
                // silently wrapping.
                if (errno == ERANGE)
                    t.value = Value(parseDouble(digits.c_str(), nullptr));
                else
                    t.value = Value(std::int64_t(v));
            }
            out.push_back(std::move(t));
            continue;
        }

        if (c == '?') {
            t.kind = Tok::Parameter;
            t.text = "?";
            ++i;
            out.push_back(std::move(t));
            continue;
        }

        static const char* kTwoChar[] = {"<>", "!=", "<=", ">=", "==", "||"};
        if (i + 1 < text.size()) {
            const std::string_view pair = text.substr(i, 2);
            for (const char* op : kTwoChar) {
                if (pair == op) {
                    t.kind = Tok::Punct;
                    t.text = op;
                    i += 2;
                    break;
                }
            }
            if (t.kind == Tok::Punct) {
                out.push_back(std::move(t));
                continue;
            }
        }

        if (std::string_view("(),.;*+-/%=<>").find(c) != std::string_view::npos) {
            t.kind = Tok::Punct;
            t.text = std::string(1, c);
            ++i;
            out.push_back(std::move(t));
            continue;
        }

        syntax(std::string("unexpected character '") + c + "'", i);
    }

    Token end;
    end.offset = text.size();
    out.push_back(std::move(end));
    return out;
}

}  // namespace sql::internal
