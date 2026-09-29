// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The statement tree the parser builds and the executor walks.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "sql/json.hpp"
#include "sql/sql.hpp"

namespace sql::internal {

// ---------------------------------------------------------- expressions ---

enum class ExprKind
{
    Literal,
    Column,
    Parameter,
    Unary,
    Binary,
    IsNull,     ///< `lhs IS [NOT] NULL`, negated by `negated`
    InList,     ///< `lhs [NOT] IN (list...)`
    Between,    ///< `lhs [NOT] BETWEEN list[0] AND list[1]`
    Like,       ///< `lhs [NOT] LIKE rhs`
    Aggregate,  ///< `COUNT(*)` when `lhs` is null, else `op(lhs)`
};

enum class AggregateOp
{
    Count,
    Sum,
    Avg,
    Min,
    Max,
};

const char* toString(AggregateOp) noexcept;

enum class UnaryOp
{
    Negate,
    Plus,
    Not,
};

enum class BinaryOp
{
    Or,
    And,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Concat,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

/// One node for every shape. Only the fields its `kind` names are meaningful,
/// which keeps the executor's switch flat and the parser free of casts.
struct Expr
{
    ExprKind kind = ExprKind::Literal;

    Value literal;                ///< Literal
    std::string table, column;    ///< Column; `table` may be empty or an alias
    std::size_t parameter = 0;    ///< Parameter: zero-based position of its `?`
    UnaryOp unary = UnaryOp::Not; ///< Unary
    BinaryOp binary = BinaryOp::Eq;
    ExprPtr lhs, rhs;
    std::vector<ExprPtr> list;  ///< InList, Between
    bool negated = false;       ///< IsNull, InList, Between, Like
    AggregateOp aggregate = AggregateOp::Count;

    /// Filled in by the planner: the position of `table.column` in the row
    /// being evaluated. -1 until then.
    int slot = -1;
    /// Also from the planner, for Aggregate nodes: where this aggregate's
    /// running total sits in the group's accumulators.
    int aggregateSlot = -1;
    /// Also from the planner, for Column nodes: the declared type of that
    /// column, which is the affinity comparisons against it use.
    Type columnType = Type::Null;

    /// Human-readable rendering, used to name unaliased result columns.
    std::string describe() const;

    /// Height of the subtree rooted here (a leaf is 1). The parser refuses a
    /// tree taller than kMaxExprHeight, which bounds every recursive walk of it
    /// -- planning, evaluation, describe() and destruction.
    unsigned height = 1;
};

inline constexpr unsigned kMaxExprHeight = 2048;

// ----------------------------------------------------------- statements ---

/// A statement's plan against one schema snapshot, owned by the executor and
/// kept on the statement so the next run against the same schema reuses it.
struct SelectPlan;
struct WritePlan;

struct CreateTableStmt
{
    std::string name;
    bool ifNotExists = false;
    std::vector<ColumnInfo> columns;
    /// From a table-level `PRIMARY KEY (a, b, ...)`, in key order; empty when
    /// the key was declared inline or not at all.
    std::vector<std::string> tablePrimaryKey;
    /// One entry per table-level `UNIQUE (a, b, ...)`.
    std::vector<std::vector<std::string>> uniques;
};

struct DropTableStmt
{
    std::string name;
    bool ifExists = false;
};

struct CreateIndexStmt
{
    std::string name;
    std::string table;
    std::vector<std::string> columns;
    bool unique = false;
    bool ifNotExists = false;
};

struct DropIndexStmt
{
    std::string name;
    bool ifExists = false;
};

struct InsertStmt
{
    std::string table;
    std::vector<std::string> columns;  ///< empty means "every column, in order"
    std::vector<std::vector<ExprPtr>> rows;
    std::shared_ptr<WritePlan> plan;
};

struct SelectStmt
{
    struct Item
    {
        ExprPtr expr;       ///< null when this is a star
        std::string alias;  ///< from AS, or derived from the expression
        bool aliased = false;  ///< true only when the alias was written out
        bool star = false;
        std::string starTable;  ///< `t.*`; empty for a bare `*`
    };

    /// The first source has no ON clause; each later one is INNER JOINed.
    struct Source
    {
        std::string table;
        std::string alias;
        ExprPtr on;
    };

    struct OrderTerm
    {
        ExprPtr expr;
        bool descending = false;
        /// Set by the planner when the term named a result column by alias or
        /// by position, in which case `expr` is not evaluated.
        int output = -1;
    };

    std::vector<Item> items;
    std::vector<Source> sources;
    ExprPtr where;
    std::vector<ExprPtr> groupBy;
    ExprPtr having;
    std::vector<OrderTerm> order;
    std::int64_t limit = -1;  ///< -1 for no limit
    std::int64_t offset = 0;
    /// Set by a trailing `FOR JSON ...`, which turns the rows into one TEXT
    /// cell holding the document.
    std::optional<JsonOptions> forJson;
    std::shared_ptr<SelectPlan> plan;
};

struct UpdateStmt
{
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    ExprPtr where;
    std::shared_ptr<WritePlan> plan;
};

struct DeleteStmt
{
    std::string table;
    ExprPtr where;
    std::shared_ptr<WritePlan> plan;
};

using Statement = std::variant<CreateTableStmt, DropTableStmt, CreateIndexStmt, DropIndexStmt,
                               InsertStmt, SelectStmt, UpdateStmt, DeleteStmt>;

/// True for the statements that need a write transaction.
bool mutates(const Statement& s) noexcept;

}  // namespace sql::internal
