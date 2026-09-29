// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Recursive-descent parser: tokens in, statements out.
#pragma once

#include <string_view>
#include <vector>

#include "internal/ast.hpp"

namespace sql::internal {

/// Parses one or more `;`-separated statements. A trailing semicolon is
/// optional. `parameters`, when given, receives the number of `?` placeholders
/// across the whole batch. Throws SyntaxError or Unsupported.
std::vector<Statement> parse(std::string_view text, std::size_t* parameters = nullptr);

}  // namespace sql::internal
