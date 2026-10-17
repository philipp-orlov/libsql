// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The statement tree the parser builds. Planning works on a tree it owns (a
// fresh parse of the statement's text), and writes its resolutions -- slots,
// functions, affinities, sub-plans -- into the nodes, so evaluation is a walk
// over this same tree.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "internal/types.hpp"
#include "sql/json.hpp"
#include "sql/sql.hpp"

namespace sql::internal {

struct FunctionDef;
struct SelectStmt;
struct Expr;
struct SubqueryPlan;
using ExprPtr = std::unique_ptr<Expr>;

// ---------------------------------------------------------- expressions ---

enum class ExprKind : std::uint8_t
{
    Literal,
    Column,     ///< [schema.][table.]column; resolved to a frame slot
    Parameter,  ///< `param` is the zero-based parameter index
    Unary,
    Binary,
    Is,         ///< lhs IS [NOT] rhs (null-safe equality); also IS [NOT] DISTINCT FROM, ISNULL, NOTNULL
    InList,     ///< lhs [NOT] IN (list...)
    InSelect,   ///< lhs [NOT] IN (select)
    Between,    ///< lhs [NOT] BETWEEN list[0] AND list[1]
    Like,       ///< lhs [NOT] LIKE/GLOB rhs [ESCAPE extra]
    Cast,       ///< CAST(lhs AS name)
    Case,       ///< CASE [lhs] WHEN list[2k] THEN list[2k+1] ... [ELSE rhs] END
    Collate,    ///< lhs COLLATE name
    Function,   ///< scalar call: name(list...)
    Aggregate,  ///< aggregate call, after resolution
    Window,     ///< function call with OVER
    Subquery,   ///< (SELECT ...)
    Exists,     ///< [NOT] EXISTS (SELECT ...)
    RowValue,   ///< (a, b, ...)
    Raise,      ///< RAISE(IGNORE|ROLLBACK|ABORT|FAIL[, message])
    Slot,       ///< planner-made: read frame slot `slot` (a computed value)
};

enum class UnaryOp : std::uint8_t
{
    Negate,
    Plus,
    Not,
    BitNot,
};

enum class BinaryOp : std::uint8_t
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
    BitAnd,
    BitOr,
    ShiftLeft,
    ShiftRight,
};

enum class LikeKind : std::uint8_t
{
    Like,
    Glob,
    Regexp,
    Match,
};

enum class RaiseKind : std::uint8_t
{
    Ignore,
    Rollback,
    Abort,
    Fail,
};

const char* spell(BinaryOp op) noexcept;

struct OrderTerm
{
    ExprPtr expr;
    bool desc = false;
    std::uint8_t nulls = 0;  ///< 0 default, 1 NULLS FIRST, 2 NULLS LAST
    /// Resolved: the result column this term names (alias or position), or -1.
    int output = -1;
};

enum class FrameUnit : std::uint8_t
{
    Rows,
    Range,
    Groups,
};

enum class FrameBound : std::uint8_t
{
    UnboundedPreceding,
    Preceding,
    CurrentRow,
    Following,
    UnboundedFollowing,
};

enum class FrameExclude : std::uint8_t
{
    NoOthers,
    CurrentRow,
    Group,
    Ties,
};

struct WindowSpec
{
    std::string base;  ///< a named window this one builds on, or empty
    std::vector<ExprPtr> partition;
    std::vector<OrderTerm> order;
    bool hasFrame = false;
    FrameUnit unit = FrameUnit::Range;
    FrameBound startKind = FrameBound::UnboundedPreceding;
    FrameBound endKind = FrameBound::CurrentRow;
    ExprPtr startExpr, endExpr;
    FrameExclude exclude = FrameExclude::NoOthers;
};

/// One node for every expression shape. Only the fields its `kind` names are
/// meaningful.
struct Expr
{
    // User-provided, so make_unique<Expr>() runs the member initializers
    // without first zero-filling the whole node.
    Expr() noexcept {}

    ExprKind kind = ExprKind::Literal;
    std::uint8_t op = 0;  ///< UnaryOp, BinaryOp, LikeKind or RaiseKind
    bool negated = false; ///< Is (IS NOT), InList, InSelect, Between, Like, Exists
    bool distinct = false;  ///< Aggregate/Window: f(DISTINCT x)
    bool star = false;      ///< count(*)

    Value literal;       ///< Literal; Raise message
    std::string name;    ///< Column name; function name; collation name; cast type name; window name
    std::string table;   ///< Column qualifier
    std::string schema;  ///< Column schema qualifier
    std::size_t param = 0;

    ExprPtr lhs, rhs, extra;   ///< extra: LIKE's ESCAPE
    std::vector<ExprPtr> list; ///< arguments, IN list, CASE arms, BETWEEN bounds, row values
    std::unique_ptr<SelectStmt> select;  ///< Subquery, Exists, InSelect
    std::unique_ptr<WindowSpec> window;  ///< Window
    ExprPtr filter;                      ///< FILTER (WHERE ...)
    std::vector<OrderTerm> orderBy;      ///< ORDER BY inside an aggregate's arguments

    std::uint32_t begin = 0, end = 0;  ///< source span, for result column names
    unsigned height = 1;

    // ---- written by the planner ----
    int slot = -1;                 ///< Column/Aggregate/Window/Slot: frame slot
    const FunctionDef* fn = nullptr;
    SubqueryPlan* sub = nullptr;   ///< Subquery/Exists/InSelect
    Affinity affinity = Affinity::Blob;     ///< expression affinity (column's, cast's)
    bool hasAffinity = false;               ///< a column or CAST: affinity applies in comparisons
    bool strictDatetime = false;            ///< a DATETIME column: legacy-style comparison
    Collation collation = Collation::Binary;
    std::uint8_t collationStrength = 0;     ///< 0 none, 1 from a column, 2 explicit COLLATE
    std::uint32_t escapeChar = 0;           ///< LIKE: constant ESCAPE character, resolved
    int aggIndex = -1;                      ///< Aggregate: index in the core's aggregate list
    mutable Value scratch;                  ///< where computed results live
    mutable std::vector<const Value*> argv; ///< Function: argument pointers, reused

    std::string describe() const;
};

/// libsql bounds expression trees more generously than SQLite's 1000, so that
/// long flat chains (`a OR b OR ...`) generated by applications keep working.
inline constexpr unsigned kMaxExprHeight = 2048;

ExprPtr makeExpr(ExprKind kind);

// ------------------------------------------------------------- SELECT ----

struct ResultColumn
{
    ExprPtr expr;  ///< null for a star
    std::string alias;
    bool star = false;
    std::string starTable;  ///< `t.*`; empty for a bare `*`
    std::uint32_t begin = 0, end = 0;
};

enum class JoinType : std::uint8_t
{
    Inner,
    Left,
    Right,
    Full,
    Cross,
};

struct FromItem
{
    enum class Kind : std::uint8_t
    {
        Table,
        Subquery,
        Function,
        Group,  ///< a parenthesized join: `(a JOIN b)`
    };
    Kind kind = Kind::Table;
    std::string schema, name, alias;
    std::unique_ptr<SelectStmt> subquery;
    std::vector<ExprPtr> args;      ///< table-valued function arguments
    std::vector<FromItem> group;    ///< Group
    /// How this item joins the items before it; ignored on the first.
    JoinType join = JoinType::Inner;
    bool natural = false;
    ExprPtr on;
    std::vector<std::string> usingColumns;
    std::string indexedBy;
    bool notIndexed = false;
};

struct NamedWindow
{
    std::string name;
    WindowSpec spec;
};

struct SelectCore
{
    bool distinct = false;
    std::vector<ResultColumn> columns;
    std::vector<FromItem> from;
    ExprPtr where;
    std::vector<ExprPtr> groupBy;
    ExprPtr having;
    std::vector<NamedWindow> windows;
    /// Non-empty for `VALUES (...), (...)`.
    std::vector<std::vector<ExprPtr>> values;
};

enum class CompoundOp : std::uint8_t
{
    UnionAll,
    Union,
    Intersect,
    Except,
};

struct CommonTableExpr
{
    std::string name;
    std::vector<std::string> columns;
    std::unique_ptr<SelectStmt> select;
    std::uint8_t materialized = 0;  ///< 0 default, 1 MATERIALIZED, 2 NOT MATERIALIZED
};

struct WithClause
{
    bool recursive = false;
    std::vector<CommonTableExpr> ctes;
};

struct SelectStmt
{
    std::unique_ptr<WithClause> with;
    std::vector<SelectCore> cores;
    std::vector<CompoundOp> ops;  ///< between cores; cores.size() - 1 entries
    std::vector<OrderTerm> order;
    ExprPtr limit, offset;
    std::optional<JsonOptions> forJson;
};

// ---------------------------------------------------------------- DML ----

enum class Conflict : std::uint8_t
{
    Default,  ///< none given: the constraint's own, else ABORT
    Rollback,
    Abort,
    Fail,
    Ignore,
    Replace,
};

struct SetClause
{
    std::vector<std::string> columns;  ///< one, or several for `(a, b) = (...)`
    ExprPtr value;
};

struct IndexedColumn
{
    ExprPtr expr;          ///< a Column node, or any expression for an expression index
    std::string collation; ///< empty when not given
    bool desc = false;
    std::uint32_t begin = 0, end = 0;
};

struct Upsert
{
    std::vector<IndexedColumn> target;
    ExprPtr targetWhere;
    bool doNothing = false;
    std::vector<SetClause> set;
    ExprPtr where;
};

struct InsertStmt
{
    std::unique_ptr<WithClause> with;
    Conflict orAction = Conflict::Default;
    std::string schema, table, alias;
    std::vector<std::string> columns;
    bool defaultValues = false;
    std::unique_ptr<SelectStmt> select;  ///< VALUES arrive as a select of one VALUES core
    std::vector<Upsert> upserts;
    std::vector<ResultColumn> returning;
};

struct UpdateStmt
{
    std::unique_ptr<WithClause> with;
    Conflict orAction = Conflict::Default;
    std::string schema, table, alias;
    std::string indexedBy;
    bool notIndexed = false;
    std::vector<SetClause> set;
    std::vector<FromItem> from;
    ExprPtr where;
    std::vector<ResultColumn> returning;
    std::vector<OrderTerm> order;
    ExprPtr limit, offset;
};

struct DeleteStmt
{
    std::unique_ptr<WithClause> with;
    std::string schema, table, alias;
    std::string indexedBy;
    bool notIndexed = false;
    ExprPtr where;
    std::vector<ResultColumn> returning;
    std::vector<OrderTerm> order;
    ExprPtr limit, offset;
};

// ---------------------------------------------------------------- DDL ----

enum class FkAction : std::uint8_t
{
    NoAction,
    Restrict,
    SetNull,
    SetDefault,
    Cascade,
};

struct ForeignKeyDef
{
    std::vector<std::string> columns;  ///< child columns
    std::string table;                 ///< parent
    std::vector<std::string> parentColumns;  ///< empty: the parent's primary key
    FkAction onDelete = FkAction::NoAction;
    FkAction onUpdate = FkAction::NoAction;
    bool deferred = false;
};

struct CheckDef
{
    std::string name;
    ExprPtr expr;
};

struct ColumnDef
{
    std::string name;
    std::string type;  ///< declared type, as written (normalized spacing)
    bool primaryKey = false;
    bool pkDesc = false;
    bool autoincrement = false;
    Conflict pkConflict = Conflict::Default;
    bool notNull = false;
    Conflict notNullConflict = Conflict::Default;
    bool unique = false;
    Conflict uniqueConflict = Conflict::Default;
    std::vector<CheckDef> checks;
    ExprPtr defaultExpr;
    std::string collation;
    ExprPtr generated;
    bool stored = false;
    std::vector<ForeignKeyDef> references;
};

struct TableConstraint
{
    enum class Kind : std::uint8_t
    {
        PrimaryKey,
        Unique,
        Check,
        ForeignKey,
    };
    Kind kind = Kind::PrimaryKey;
    std::string name;
    std::vector<IndexedColumn> columns;
    Conflict conflict = Conflict::Default;
    bool autoincrement = false;
    ExprPtr check;
    ForeignKeyDef fk;
};

struct CreateTableStmt
{
    std::uint32_t nameBegin = 0;  ///< offset of the object name, for the stored SQL text
    bool temp = false;
    bool ifNotExists = false;
    std::string schema, name;
    std::vector<ColumnDef> columns;
    std::vector<TableConstraint> constraints;
    bool withoutRowid = false;
    bool strict = false;
    std::unique_ptr<SelectStmt> asSelect;
};

struct CreateIndexStmt
{
    std::uint32_t nameBegin = 0;  ///< offset of the object name, for the stored SQL text
    bool unique = false;
    bool ifNotExists = false;
    std::string schema, name, table;
    std::vector<IndexedColumn> columns;
    ExprPtr where;
};

struct CreateViewStmt
{
    std::uint32_t nameBegin = 0;  ///< offset of the object name, for the stored SQL text
    bool temp = false;
    bool ifNotExists = false;
    std::string schema, name;
    std::vector<std::string> columns;
    std::unique_ptr<SelectStmt> select;
};

struct StatementNode;

enum class TriggerTime : std::uint8_t
{
    Before,
    After,
    InsteadOf,
};

enum class TriggerEvent : std::uint8_t
{
    Delete,
    Insert,
    Update,
};

struct CreateTriggerStmt
{
    std::uint32_t nameBegin = 0;  ///< offset of the object name, for the stored SQL text
    bool temp = false;
    bool ifNotExists = false;
    std::string schema, name, table;
    TriggerTime time = TriggerTime::Before;
    TriggerEvent event = TriggerEvent::Insert;
    std::vector<std::string> updateOf;
    ExprPtr when;
    std::vector<std::unique_ptr<StatementNode>> body;
};

struct DropStmt
{
    enum class Kind : std::uint8_t
    {
        Table,
        Index,
        View,
        Trigger,
    };
    Kind kind = Kind::Table;
    bool ifExists = false;
    std::string schema, name;
};

struct AlterTableStmt
{
    enum class Action : std::uint8_t
    {
        RenameTable,
        RenameColumn,
        AddColumn,
        DropColumn,
    };
    Action action = Action::RenameTable;
    std::string schema, table;
    std::string column, newName;
    ColumnDef def;
    std::uint32_t defBegin = 0, defEnd = 0;  ///< span of the column definition
};

struct TransactionStmt
{
    enum class Kind : std::uint8_t
    {
        Begin,
        Commit,
        Rollback,
        Savepoint,
        Release,
        RollbackTo,
    };
    Kind kind = Kind::Begin;
    std::uint8_t mode = 0;  ///< BEGIN: 0 deferred, 1 immediate, 2 exclusive
    std::string name;
};

struct PragmaStmt
{
    std::string schema, name;
    ExprPtr value;  ///< null for a query
};

struct AttachStmt
{
    ExprPtr file;
    ExprPtr schema;
};

struct DetachStmt
{
    ExprPtr schema;
};

struct AnalyzeStmt
{
    std::string schema, name;
};

struct ReindexStmt
{
    std::string schema, name;
};

struct VacuumStmt
{
    std::string schema;
    ExprPtr into;
};

struct ExplainStmt
{
    bool queryPlan = false;
    std::unique_ptr<StatementNode> statement;
};

using StatementBody =
    std::variant<SelectStmt, InsertStmt, UpdateStmt, DeleteStmt, CreateTableStmt,
                 CreateIndexStmt, CreateViewStmt, CreateTriggerStmt, DropStmt, AlterTableStmt,
                 TransactionStmt, PragmaStmt, AttachStmt, DetachStmt, AnalyzeStmt, ReindexStmt,
                 VacuumStmt, ExplainStmt>;

struct StatementNode
{
    StatementBody body;
    std::uint32_t begin = 0, end = 0;  ///< span in the source text, without the ';'
};

/// True for statements that need a write transaction.
bool mutates(const StatementNode& s) noexcept;
/// True for BEGIN/COMMIT/ROLLBACK/SAVEPOINT/RELEASE.
bool controlsTransaction(const StatementNode& s) noexcept;

}  // namespace sql::internal
