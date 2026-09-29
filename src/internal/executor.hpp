// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Statement execution: plan an access path per table, walk the store, and
// build a Result.
#pragma once

#include "internal/ast.hpp"
#include "internal/catalog.hpp"

namespace sql::internal {

/// Assembles a Result on the executor's behalf; Row and Result keep their
/// constructors private so that nothing outside this file can fabricate one.
struct ResultBuilder
{
    void columns(std::vector<std::string> names)
    {
        result.names_ = std::make_shared<const std::vector<std::string>>(std::move(names));
    }
    void columns(std::shared_ptr<const std::vector<std::string>> names)
    {
        result.names_ = std::move(names);
    }
    void row(std::vector<Value> values)
    {
        result.rows_.push_back(Row(result.names_, std::move(values)));
    }
    void changed(std::uint64_t n) { result.changes_ = n; }
    void lastInsertId(std::int64_t id) { result.lastInsertId_ = id; }

    Result result;
};

/// Runs one statement. `stmt` is taken by reference because planning annotates
/// its column references with the slots they resolved to.
Result execute(Catalog& catalog, Statement& stmt, const Params& params);

}  // namespace sql::internal
