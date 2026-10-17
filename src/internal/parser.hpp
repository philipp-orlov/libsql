// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Text to statement trees, for the SQLite grammar plus FOR JSON.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "internal/ast.hpp"

namespace sql::internal {

struct ParsedBatch
{
    std::vector<StatementNode> statements;
    /// One entry per parameter slot (1-based number - 1): the name as spelled
    /// (":a", "@a", "$a", "?7") or empty for a plain `?`.
    std::vector<std::string> parameterNames;
};

/// Parses every `;`-separated statement in `text`. Throws SyntaxError.
ParsedBatch parse(std::string_view text);

/// Parses exactly one statement (used for stored schema SQL).
StatementNode parseOne(std::string_view text);

/// Parses a standalone expression (used for stored defaults and generated
/// columns).
ExprPtr parseExpression(std::string_view text);

}  // namespace sql::internal
