// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tokenizer for the SQLite language (plus libsql's FOR JSON, which needs no
// tokens of its own).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "internal/util.hpp"
#include "sql/value.hpp"

namespace sql::internal {

enum class Tok : std::uint8_t
{
    End,
    Identifier,   ///< bare or quoted; `quoted` says which
    Number,       ///< `value` holds an Integer or a Real
    String,       ///< `value` holds Text
    BlobLiteral,  ///< X'..'; `value` holds a Blob
    Parameter,    ///< ?, ?NNN, :name, @name, $name; `text` is the spelling
    Punct,        ///< `text` is the operator or punctuation
};

/// Expression operator levels a token can continue (Token::ops), so each
/// precedence level of the parser can rule a token out with one test.
enum : std::uint16_t
{
    kOpOr = 1u << 0,
    kOpAnd = 1u << 1,
    kOpEquality = 1u << 2,  ///< = == <> != IS ISNULL NOTNULL NOT IN BETWEEN LIKE GLOB REGEXP MATCH
    kOpRelational = 1u << 3,
    kOpBitwise = 1u << 4,
    kOpAdditive = 1u << 5,
    kOpMultiplicative = 1u << 6,
    kOpConcat = 1u << 7,  ///< || -> ->>
    kOpCollate = 1u << 8,
};

struct Token
{
    Tok kind = Tok::End;
    bool quoted = false;
    std::uint16_t ops = 0;  ///< kOp* levels this token can continue
    /// For an unsigned integer literal that only fits once negated
    /// (9223372036854775808), so `-9223372036854775808` can be INT64_MIN.
    bool minInt = false;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::string text;  ///< spelling for identifiers, parameters and punctuation
    Value value;       ///< payload for literals

    /// True for a bare identifier spelled `word`, ignoring case. Quoted
    /// identifiers never match, which is what makes "order" a usable column.
    bool isKeyword(std::string_view word) const noexcept
    {
        return kind == Tok::Identifier && !quoted && equalsNoCase(text, word);
    }
    /// The same for a literal: its folded spelling is a compile-time
    /// constant, so most checks are a length and one word compared.
    template <std::size_t N>
    bool isKeyword(const char (&word)[N]) const noexcept
    {
        constexpr std::size_t n = N - 1;
        if (kind != Tok::Identifier || quoted || text.size() != n)
            return false;
        if (fold != foldWord(word, n < 8 ? n : 8))
            return false;
        return n <= 8 || equalsNoCase(std::string_view(text).substr(8), std::string_view(word + 8, n - 8));
    }
    /// Up to the first eight characters of `w`, uppercased (ASCII) and packed
    /// little-endian: what Token::fold holds for an identifier.
    static constexpr std::uint64_t foldWord(const char* w, std::size_t n) noexcept
    {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n && i < 8; ++i) {
            const char c = w[i] >= 'a' && w[i] <= 'z' ? char(w[i] - 32) : w[i];
            v |= std::uint64_t(static_cast<unsigned char>(c)) << (8 * i);
        }
        return v;
    }
    std::uint64_t fold = 0;  ///< identifiers: foldWord of the spelling
    bool isPunct(std::string_view p) const noexcept { return kind == Tok::Punct && text == p; }
    bool reserved = false;  ///< an identifier spelled like a reserved word
    bool joinWord = false;  ///< NATURAL LEFT RIGHT FULL OUTER CROSS INNER
};

/// Words that can never be a bare name: SQLite keywords without %fallback.
bool isReservedWord(std::string_view w) noexcept;
/// The words that can start a join operator.
bool isJoinWord(std::string_view w) noexcept;

/// Splits `text` into tokens, ending with one Tok::End. Throws SyntaxError.
std::vector<Token> tokenize(std::string_view text);
/// The same into `out`, which keeps its capacity.
void tokenizeInto(std::string_view text, std::vector<Token>& out);

/// True when `text` ends with a complete statement: a `;` outside quotes,
/// comments and CREATE TRIGGER bodies (sqlite3_complete).
bool isCompleteStatement(std::string_view text);

}  // namespace sql::internal
