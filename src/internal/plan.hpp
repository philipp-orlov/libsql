// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Planning: resolving names to slots and functions, and turning SELECT trees
// into SelectPlans. The DML and DDL planners build on the same Planner.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "internal/exec.hpp"

namespace sql::internal {

/// A compiled statement: its own parse tree (annotated), its frame, and the
/// plans built from it, valid for the schema snapshots it was planned against.
struct ResultBuilder;

/// What a planned non-SELECT statement runs.
struct ExecPlan
{
    virtual ~ExecPlan() = default;
    /// Runs the statement; rows (RETURNING, PRAGMA, EXPLAIN) go to `out` when
    /// it is given. Returns the number of rows changed.
    virtual std::int64_t run(Ctx& c, ResultBuilder* out) = 0;
};

struct Program
{
    std::string text;  ///< the statement's source; spans index into it
    std::unique_ptr<StatementNode> node;
    std::vector<std::unique_ptr<StatementNode>> extraAsts;  ///< views, CTE copies
    std::vector<std::unique_ptr<std::string>> extraTexts;   ///< their sources
    std::vector<std::unique_ptr<SelectStmt>> ownedSelects;  ///< split recursive CTEs
    std::vector<std::unique_ptr<struct CteDef>> ctes;
    std::vector<std::unique_ptr<SubqueryPlan>> subqueries;
    std::vector<std::unique_ptr<SelectPlan>> selects;
    std::vector<ExprPtr> owned;  ///< synthetic expressions
    std::vector<std::shared_ptr<Table>> ownedTables;  ///< stand-ins for views written via triggers
    std::size_t frameSize = 0;
    std::vector<Value> frame;
    /// Schema snapshots the plan depends on, by store index.
    std::vector<std::shared_ptr<const Schema>> schemas;
    std::vector<std::uint64_t> schemaStamps;  ///< their stamps when planned
    std::unique_ptr<SelectPlan> select;  ///< SELECT statements
    std::unique_ptr<ExecPlan> exec;      ///< everything but SELECT
    bool active = false;                 ///< currently running (re-entrancy guard)
};

struct CteDef
{
    CommonTableExpr* cte = nullptr;
    std::string_view text;    ///< source the CTE's spans index into
    SubqueryPlan* plan = nullptr;  ///< materialized rows, once planned
    bool recursive = false;
    bool planning = false;    ///< inside its own definition
    std::vector<std::string> names;  ///< known once the initial part is planned
    struct Scope* home = nullptr;
};

struct Scope
{
    Scope* parent = nullptr;
    CorePlan* core = nullptr;
    std::vector<SourceNode>* sources = nullptr;
    std::size_t visible = static_cast<std::size_t>(-1);  ///< sources visible (ON clauses)
    std::vector<CteDef*> ctes;
    std::vector<ResultColumn>* resultColumns = nullptr;
    SubqueryPlan* owner = nullptr;  ///< the sub-query this scope belongs to
    std::string_view text;          ///< source the scope's spans index into
    bool trigger = false;           ///< RAISE() allowed
};

enum ResolveFlag : unsigned
{
    kAllowAggregate = 1u << 0,
    kAllowWindow = 1u << 1,
    kAllowAlias = 1u << 2,
    kInAggregate = 1u << 3,
    kNoSubquery = 1u << 4,   ///< CHECK, DEFAULT, generated, index expressions
    kOnlyDeterministic = 1u << 5,
};

class Planner
{
public:
    Planner(Program& prog, Session& session) : prog_(prog), session_(session) {}

    /// Plans a whole select. `outer` is the enclosing scope for correlated
    /// references (null at top level).
    std::unique_ptr<SelectPlan> planSelect(SelectStmt& s, Scope* outer, std::string_view text,
                                           SubqueryPlan* owner);
    /// Resolves names and functions in `e`, in place.
    void resolve(ExprPtr& e, Scope& scope, unsigned flags);
    std::size_t allocate(std::size_t n);
    SubqueryPlan* newSubquery(SubqueryPlan::Kind kind);
    /// Adds a table as a source of `scope`'s core (DML targets, NEW/OLD).
    SourceNode& addTableSource(std::vector<SourceNode>& sources, const Table& t,
                               std::size_t storeIndex, std::string alias);
    /// Splits on top-level AND.
    static void conjuncts(Expr* e, std::vector<Expr*>& out);
    /// Finds a table by optionally qualified name; null if none.
    const Table* findTable(std::string_view schema, std::string_view name, std::size_t& store);
    Session& session() noexcept { return session_; }
    Program& program() noexcept { return prog_; }
    /// The text an expression's spans index into, for the scope being planned.
    ExprPtr copyExpr(const Expr& e, std::string_view text);
    /// Plans one FROM clause into `core.sources`.
    void planFrom(std::vector<FromItem>& from, CorePlan& core, Scope& scope);
    /// Distributes conjuncts over the sources' levels and plans access paths.
    void planFilters(CorePlan& core, Expr* where);
    void planFiltersImpl(CorePlan& core, Expr* where);

private:
    void planCore(SelectCore& core, CorePlan& plan, Scope& scope, SelectPlan& sp, bool first,
                  std::vector<OrderTerm>* order, bool compound);
    void resolveColumn(ExprPtr& e, Scope& scope, unsigned flags);
    void resolveFunction(ExprPtr& e, Scope& scope, unsigned flags);
    void resolveWindow(Expr& e, Scope& scope, unsigned flags);
    void planSubqueryExpr(Expr& e, Scope& scope);
    void addSource(FromItem& item, CorePlan& core, Scope& scope);
    SubqueryPlan* planCte(CteDef& d, Scope& scope);
    void setSourceFromSelect(SourceNode& src, SelectPlan& sp);
    void expandStar(const ResultColumn& rc, CorePlan& plan, SelectPlan& sp, bool first);
    void computeExprMeta(Expr& e);
    SubqueryPlan* planView(const View& v, std::size_t store);
    SubqueryPlan* planSchemaTable(std::string_view schema, std::string_view name);
    SubqueryPlan* planPragmaFunction(std::string_view pragma, std::vector<ExprPtr>& args,
                                     Scope& scope);
    std::string outputName(const Expr& e, const CorePlan& plan, std::string_view text, bool sub);

    Program& prog_;
    Session& session_;
    std::vector<std::string> viewsInProgress_;
};

/// Structural equality of two resolved expressions (ORDER BY/GROUP BY matching,
/// expression indexes).
bool sameExpr(const Expr& a, const Expr& b) noexcept;

/// Plans a SELECT statement program.
void planSelectProgram(Program& p, Session& session);

}  // namespace sql::internal
