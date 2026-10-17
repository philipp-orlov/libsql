// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Planning and running whole statements.
#pragma once

#include <string>
#include <vector>

#include "internal/plan.hpp"
#include "sql/sql.hpp"

namespace sql::internal {

/// Assembles a Result on the executor's behalf; Row and Result keep their
/// constructors private so that nothing outside this layer can fabricate one.
struct ResultBuilder
{
    void columns(std::vector<std::string> names, std::vector<std::string> qualified = {})
    {
        auto n = std::make_shared<ColumnNames>();
        n->names = std::move(names);
        n->qualified = std::move(qualified);
        result.names_ = std::move(n);
    }
    void columns(std::shared_ptr<const ColumnNames> names) { result.names_ = std::move(names); }
    void row(std::vector<Value> values)
    {
        if (each) {
            add(values.data(), values.size(), true);
            return;
        }
        result.rows_.push_back(Row(result.names_, Values(std::move(values))));
    }
    void row(const Value* values, std::size_t count) { add(const_cast<Value*>(values), count, false); }
    /// The same, taking the values: the caller's array is scratch.
    void take(Value* values, std::size_t count) { add(values, count, true); }
    void reserve(std::size_t rows) { result.rows_.reserve(rows); }
    std::size_t rows() const noexcept { return result.rows_.size(); }
    void changed(std::uint64_t n) { result.changes_ = n; }
    void lastInsertId(std::int64_t id) { result.lastInsertId_ = id; }
    bool hasColumns() const noexcept { return result.names_ != nullptr; }

    Result result;
    /// Statement::each: rows go here instead of into `result`.
    const RowCallback* each = nullptr;
    bool stopped = false;  ///< the callback asked for no more rows

private:
    void add(Value* values, std::size_t count, bool move)
    {
        if (each) {
            if (!stopped && !(*each)(RowView(values, count, &result.names_)))
                stopped = true;
            return;
        }
        Row& r = result.rows_.emplace_back();
        r.names_ = result.names_;
        r.values_.fill(values, count, move);
    }
};

/// Plans `p` (its node and text set) against the session's schemas. `outer`
/// is the scope trigger bodies see (NEW, OLD); null at top level.
void planStatement(Program& p, Session& s, Scope* outer);

/// True when the program's plan was made against the schemas `s` sees now.
bool planCurrent(const Program& p, Session& s);

/// Runs a planned program at top level, filling `out`.
void runProgram(Program& p, Session& s, const std::vector<Value>& params, ResultBuilder& out,
                FuncContext& fctx);

/// Runs a planned program from a trigger body; returns rows changed.
std::int64_t runNested(Program& p, Session& s, int depth, FuncContext& fctx);

/// The pragma and schema-table helpers (pragma.cpp).
struct PragmaResult
{
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
};
PragmaResult runPragma(Session& s, const std::string& schema, const std::string& name,
                       const Value* value, bool call);
std::vector<std::string> pragmaColumns(const std::string& name);
/// sqlite_schema rows for store `i`.
void schemaRows(Session& s, std::size_t i, RowStore& out);

/// DDL (ddl.cpp).
std::unique_ptr<ExecPlan> planDdl(Program& p, Session& s);

/// EXPLAIN / EXPLAIN QUERY PLAN listing (explain.cpp).
void explainProgram(Program& p, bool queryPlan, ResultBuilder& out);

}  // namespace sql::internal
