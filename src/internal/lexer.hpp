// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tokenizer for the SQL dialect libsql accepts.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sql/value.hpp"

namespace sql::internal {

enum class Tok
{
    End,
    Identifier,  ///< bare or quoted; `quoted` says which
    Number,      ///< `value` holds an Integer or a Real
    String,      ///< `value` holds Text
    BlobLiteral, ///< X'..'; `value` holds a Blob
    Parameter,   ///< ?
    Punct,       ///< `text` is one of ( ) , . ; * + - / % = <> != < <= > >= ||
};

struct Token
{
    Tok kind = Tok::End;
    std::string text;  ///< spelling for identifiers and punctuation
    Value value;       ///< payload for literals
    bool quoted = false;
    std::size_t offset = 0;

    /// True for a bare identifier spelled `word`, ignoring case. Quoted
    /// identifiers never match, which is what makes "order" a usable column.
    bool isKeyword(std::string_view word) const noexcept;
    bool isPunct(std::string_view p) const noexcept { return kind == Tok::Punct && text == p; }
};

/// Splits `text` into tokens, ending with one Tok::End. Throws SyntaxError.
std::vector<Token> tokenize(std::string_view text);

bool equalsNoCase(std::string_view a, std::string_view b) noexcept;
std::string toLower(std::string_view s);

}  // namespace sql::internal
