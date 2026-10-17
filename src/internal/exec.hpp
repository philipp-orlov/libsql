// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The executor's runtime: what a compiled statement looks like and what it
// runs against.
//
// A statement compiles into a Program that owns its own parse tree, annotated
// in place by the planner, and one flat frame of value slots. Every column of
// every source, every aggregate and window result and every computed key
// lives at a fixed slot, so evaluating an expression is a walk over the tree
// that reads slots -- correlated sub-queries included, since an outer column
// is just a slot further up the same frame.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "internal/ast.hpp"
#include "internal/catalog.hpp"
#include "internal/func.hpp"
#include "sql/json.hpp"

namespace sql::internal {

struct Session;
struct ConnectionState;
struct SelectPlan;
struct Program;

// ------------------------------------------------------------ runtime ----

/// Everything evaluation needs: the frame, the bound parameters, the session
/// (catalogs, connection state) and the function context.
struct Ctx
{
    Value* slots = nullptr;
    const std::vector<Value>* params = nullptr;
    Session* session = nullptr;
    FuncContext* fctx = nullptr;
    /// Trigger nesting depth.
    int depth = 0;
    /// Identifies this execution of the program, so uncorrelated sub-query
    /// results are reused within it and recomputed by the next one.
    std::uint64_t run = 0;
};

/// Rows of a fixed width, stored flat. Clearing keeps both the vector and
/// each cell's buffer, so a store that is refilled every run stops
/// allocating once it has grown.
class RowStore
{
public:
    void reset(std::size_t width)
    {
        width_ = width;
        rows_ = 0;
    }
    std::size_t width() const noexcept { return width_; }
    std::size_t size() const noexcept { return rows_; }
    bool empty() const noexcept { return rows_ == 0; }
    Value* append()
    {
        const std::size_t need = (rows_ + 1) * width_;
        if (cells_.size() < need)
            cells_.resize(need);
        return &cells_[rows_++ * width_];
    }
    void appendRow(const Value* v)
    {
        Value* out = append();
        for (std::size_t i = 0; i < width_; ++i)
            out[i] = v[i];
    }
    Value* row(std::size_t i) noexcept { return &cells_[i * width_]; }
    const Value* row(std::size_t i) const noexcept { return &cells_[i * width_]; }
    void truncate(std::size_t n) noexcept
    {
        if (n < rows_)
            rows_ = n;
    }
    /// Frees the cells outright once they exceed `cells`, so one huge run
    /// does not pin its memory for the life of the plan.
    void releaseAbove(std::size_t cells)
    {
        if (cells_.capacity() > cells) {
            std::vector<Value>().swap(cells_);
            rows_ = 0;
        }
    }
    /// Gives memory back after an unusually large run.
    void shrink()
    {
        if (cells_.capacity() > (rows_ * width_) * 4 + 1024) {
            std::vector<Value> fresh(cells_.begin(), cells_.begin() + std::ptrdiff_t(rows_ * width_));
            cells_.swap(fresh);
        }
    }

private:
    std::vector<Value> cells_;
    std::size_t width_ = 0;
    std::size_t rows_ = 0;
};

/// Receives the rows a select produces. `row` returns false to stop early.
class RowSink
{
public:
    virtual ~RowSink() = default;
    virtual bool row(Ctx& c, const Value* values, std::size_t count) = 0;
    /// The same for values the caller no longer needs: a sink may move them.
    virtual bool take(Ctx& c, Value* values, std::size_t count) { return row(c, values, count); }
};

// ------------------------------------------------------- expressions ----

/// Function nodes whose arguments must not all be evaluated up front; kept in
/// Expr::op by the planner.
enum LazyFn : std::uint8_t
{
    kEager = 0,
    kCoalesce,  ///< coalesce, ifnull
    kIif,
    kIdentity,  ///< likely, unlikely, likelihood
};

/// Evaluates `e`. Leaves come back as a reference to where they already live;
/// computed values land in `e.scratch`.
const Value& eval(const Expr& e, Ctx& c);
/// `!e` is true; otherwise the expression's SQL truth (NULL is false).
bool holds(const Expr* e, Ctx& c);
/// Comparison of two values after SQLite's affinity rules for the
/// expressions they came from. Returns false (and sets `isNull`) when either
/// side is NULL.
int compareOperands(const Expr& l, const Value& a, const Expr& r, const Value& b, Collation coll,
                    bool& isNull);
/// The collation a comparison between `l` and `r` uses.
Collation comparisonCollation(const Expr& l, const Expr& r) noexcept;
/// Affinity SQLite applies to a comparison operand; Blob means none.
Affinity comparisonAffinity(const Expr& self, const Expr& other) noexcept;

// --------------------------------------------------------- sub-queries ---

struct SubqueryPlan
{
    enum class Kind : std::uint8_t
    {
        Scalar,
        Exists,
        In,
        Rows,  ///< a FROM sub-query or CTE: materialized rows
    };
    Kind kind = Kind::Scalar;
    std::unique_ptr<SelectPlan> select;
    bool correlated = false;
    /// Runs with this statement execution's results already cached?
    std::uint64_t cachedRun = 0;
    // Scalar / Exists
    Value scalar;
    bool exists = false;
    // In
    std::set<std::string> keys;
    bool hasNull = false;
    std::vector<Affinity> keyAffinity;  ///< applied to the left-hand values
    std::vector<Affinity> rowAffinity;  ///< applied to the sub-query's values
    std::vector<Collation> keyCollation;
    std::size_t width = 1;
    // Rows
    RowStore rows;
    /// Rows produced by code rather than a select (schema tables, pragmas).
    std::function<void(Ctx&, RowStore&)> generator;
    // Recursive CTE: `select` is the initial part, `recursive` the rest.
    std::unique_ptr<SelectPlan> recursive;
    bool recursiveUnionAll = true;
    const Value* current = nullptr;  ///< the queue row the recursive part reads
    std::vector<std::size_t> queueOrder;   ///< ORDER BY columns of the CTE
    std::vector<bool> queueDesc;
    Expr* limit = nullptr;
    Expr* offset = nullptr;
};

// ------------------------------------------------------------ sources ----

/// How a table source reads its rows.
struct AccessPath
{
    enum class Kind : std::uint8_t
    {
        Scan,
        Rowid,  ///< rowid equality or range (rowid-keyed tables)
        Key,    ///< leading declared-key columns (clustered tables)
        Index,
    };
    Kind kind = Kind::Scan;
    const Index* index = nullptr;
    /// Values pinning the leading key/index parts.
    std::vector<const Expr*> equals;
    /// Per pinned part: affinity applied to the value before encoding.
    std::vector<Affinity> equalAffinity;
    /// Multi-probe: the last pinned part takes each value of this IN list.
    const Expr* inList = nullptr;
    Affinity inAffinity = Affinity::Blob;
    const Expr* low = nullptr;
    const Expr* high = nullptr;
    bool lowStrict = false, highStrict = false;  ///< > and < rather than >= and <=
    Affinity rangeAffinity = Affinity::Blob;
    /// The conjuncts the bounds came from, so a run can skip re-checking the
    /// ones its window enforces exactly (see SourceNode::enforced).
    std::vector<const Expr*> equalTerms;
    const Expr* lowTerm = nullptr;
    const Expr* highTerm = nullptr;
    // scratch
    std::string prefix, lo, hi, probe;
};

struct SourceNode
{
    enum class Kind : std::uint8_t
    {
        Table,
        Rows,      ///< materialized rows of a sub-query, view or CTE
        Function,  ///< table-valued function
        Recursive, ///< the recursive reference inside a recursive CTE
        Single,    ///< no FROM clause: one empty row
        Pseudo,    ///< NEW, OLD, excluded, a row being written: slots, never scanned
    };
    Kind kind = Kind::Table;
    std::string name;   ///< table/view/CTE/function name
    std::string alias;  ///< what qualifies its columns
    std::string schema;
    std::vector<std::string> columns;
    std::vector<Affinity> affinity;
    std::vector<Collation> collation;
    std::vector<bool> hidden;
    bool qualifiedOnly = false;  ///< `excluded`: only `excluded.x` reaches it
    std::size_t base = 0;
    std::size_t width = 0;
    JoinType join = JoinType::Inner;
    std::size_t written = 0;  ///< position in the FROM clause as written
    /// ON (and USING/NATURAL) conjuncts, checked once this source is positioned.
    std::vector<Expr*> onTerms;
    /// USING/NATURAL: column positions here that coalesce with an earlier source.
    std::vector<int> usingColumns;
    std::vector<std::pair<int, int>> usingLinks;  ///< (this column, slot of the left column)

    // Table
    const Table* table = nullptr;
    std::size_t schemaIndex = 0;
    AccessPath access;
    std::size_t decodeWidth = 0;
    /// Per column: referenced by the statement, so loadRow decodes it.
    std::vector<std::uint8_t> used;
    bool needRowid = false;
    /// Index-only access, decided at the first open once every reference to
    /// the source is known: 0 undecided, 1 reads rows, 2 reads only entries.
    std::uint8_t coverState = 0;
    /// Index-only: per component of an entry (the index parts, then the
    /// parts of the row key for a clustered table) the column it fills, or
    /// -1 to skip it.
    std::vector<int> coverColumns;
    /// SELECT DISTINCT over exactly these columns and nothing else (set by
    /// the planner); with a covering index that leads with them the scan
    /// seeks past each distinct prefix instead of reading every entry.
    std::vector<int> distinctColumns;
    std::size_t skipComponents = 0;  ///< entry components a skip steps over
    std::string skipKey;             ///< scratch: the next prefix to seek to
    std::vector<ExprPtr> generated;  ///< virtual generated columns, by column position
    std::string indexedBy;
    bool notIndexed = false;
    // Rows
    SubqueryPlan* rows = nullptr;
    // Function
    const TableFunctionDef* function = nullptr;
    std::vector<Expr*> args;
    std::unique_ptr<TableFunctionCursor> fnCursor;
    // Recursive
    const Value* recursiveRow = nullptr;

    // ---- scan state ----
    nosql::Cursor cursor;
    bool started = false;
    bool exhausted = false;
    bool point = false;       ///< a single exact lookup
    std::size_t probeAt = 0;  ///< multi-probe position / Rows position
    std::vector<std::string> probes;
    /// Conjuncts this open's window guarantees for every row it yields, which
    /// the join loop therefore does not evaluate again.
    std::vector<const Expr*> enforced;
    /// The level's filters and ON terms minus `enforced`, rebuilt per open
    /// only when something is enforced.
    std::vector<Expr*> liveFilters, liveOn;
    std::string rowKey;       ///< storage key of the current row (RIGHT/FULL joins)
    bool keepRowKey = false;  ///< rowKey is only filled for RIGHT/FULL joins
    std::string_view entryKey, entryValue;  ///< the cursor's current entry
    const nosql::Db* rowsDb = nullptr;      ///< the table's rows, for index lookups
    std::size_t rowIndex = 0;
    std::vector<const Value*> argv;

    // runtime state for RIGHT/FULL joins
    std::unordered_set<std::string> matchedKeys;

    /// An inner source whose rows do not depend on the sources before it is
    /// read once per execution and replayed from memory (SQLite's automatic
    /// materialization). `localFilters` are the terms that read only this
    /// source; they are applied while filling.
    bool cacheable = false;
    std::vector<Expr*> localFilters;
    std::vector<Expr*> joinFilters;  ///< the rest of this level's terms
    RowStore cache;
    std::uint64_t cachedRun = 0;
    std::size_t cachePos = 0;
    bool replaying = false;
};

/// Marks column `col` of a table source as read by the statement.
inline void useColumn(SourceNode& s, std::size_t col)
{
    // The INTEGER PRIMARY KEY column is the rowid, which every read fills.
    if (s.table && int(col) == s.table->rowidAlias && s.table->keyedByRowid()) {
        s.needRowid = true;
        return;
    }
    s.decodeWidth = std::max(s.decodeWidth, col + 1);
    if (s.used.size() < s.decodeWidth)
        s.used.resize(s.columns.size() > s.decodeWidth ? s.columns.size() : s.decodeWidth, 0);
    s.used[col] = 1;
}

/// Every stored column is read (writes, virtual generated columns).
inline void useAllColumns(SourceNode& s, std::size_t columns)
{
    s.decodeWidth = columns;
    s.used.assign(columns, 1);
}

/// Rows beyond which a source is not cached but scanned each time.
inline constexpr std::size_t kSourceCacheRows = 65536;

// -------------------------------------------------------------- select ---

struct AggInfo
{
    Expr* call = nullptr;
    const FunctionDef* def = nullptr;
    bool countStar = false;
    bool minMax = false;  ///< min() or max(), for the bare-column rule
};

struct WinInfo
{
    Expr* call = nullptr;
    const FunctionDef* def = nullptr;  ///< aggregate used as a window, or null for a built-in
    std::uint8_t builtin = 0;          ///< row_number... see run_select.cpp
    WindowSpec* spec = nullptr;        ///< resolved (named windows merged)
    std::size_t specIndex = 0;         ///< windows sharing PARTITION/ORDER share an index
};

struct OrderedStep
{
    std::vector<Value> keys;
    std::vector<Value> args;
};

struct AggSlot
{
    std::unique_ptr<AggregateState> state;
    std::unique_ptr<std::set<std::string>> distinct;
    std::vector<OrderedStep> ordered;
};

/// One group of an aggregate query: the first row seen (or the row of the
/// min/max), and the aggregates' running states.
struct Group
{
    std::string key;
    std::vector<Value> sample;
    std::vector<AggSlot> aggs;
};

/// The groups of one run, kept on the plan and reused by the next run, so a
/// repeated aggregate query stops allocating once its groups have been seen.
struct GroupTable
{
    std::vector<std::unique_ptr<Group>> groups;
    std::size_t used = 0;
    std::unordered_map<std::string_view, std::size_t> index;
    std::vector<std::uint32_t> order;
};

/// One SELECT core (or VALUES list) after planning.
struct CorePlan
{
    SelectCore* core = nullptr;
    std::vector<SourceNode> sources;
    std::size_t slotBegin = 0, slotEnd = 0;  ///< the sources' slots
    /// Conjuncts checked once source d is positioned (inner joins), or at the
    /// end when pushing down is not safe.
    std::vector<std::vector<Expr*>> levelFilters;
    std::vector<Expr*> finalFilters;
    std::vector<Expr*> outputs;   ///< visible columns, then hidden sort keys
    std::size_t visible = 0;
    std::vector<ExprPtr> synthetic;  ///< columns a `*` expanded to
    bool aggregate = false;
    std::vector<AggInfo> aggs;
    std::vector<Expr*> groupBy;
    std::vector<Collation> groupCollation;
    Expr* having = nullptr;
    std::vector<WinInfo> windows;
    std::vector<WindowSpec*> windowSpecs;  ///< distinct specs, by specIndex
    bool distinct = false;
    bool hasRightJoin = false;
    bool singleMinMax = false;
    /// `SELECT count(*) FROM t` and nothing else: the store's entry count
    /// answers it without reading a row.
    bool countRows = false;
    std::vector<std::vector<Expr*>> values;  ///< VALUES rows
    /// End of the frame range planned for this core (sources, aggregate and
    /// window slots); snapshots for grouping and windows copy
    /// [slotBegin, frameEnd).
    std::size_t frameEnd = 0;
    // scratch
    std::string key;
    std::vector<Value> outRow;
    std::set<std::string> seen;
    std::vector<const Value*> argv;
    GroupTable groupTable;
};

/// A whole select statement (compound, ORDER BY, LIMIT) after planning.
struct SelectPlan
{
    std::vector<CorePlan> cores;
    std::vector<CompoundOp> ops;
    std::size_t columns = 0;  ///< visible result width
    std::vector<std::string> names;      ///< result names (declared names for columns)
    std::vector<std::string> subNames;   ///< names when used as a sub-query's columns
    std::vector<std::string> qualified;  ///< alias.column for plain column outputs, else ""
    std::vector<Affinity> affinity;      ///< for sub-query columns
    std::vector<Collation> collation;
    std::vector<std::uint8_t> collationStrength;
    std::vector<bool> hasAffinity;
    std::vector<std::string> declTypes;
    struct SortKey
    {
        std::size_t column;  ///< index into the produced row (visible or hidden)
        bool desc = false;
        std::uint8_t nulls = 0;
        Collation collation = Collation::Binary;
    };
    std::vector<SortKey> order;
    std::size_t rowWidth = 0;  ///< visible + hidden sort keys
    Expr* limit = nullptr;
    Expr* offset = nullptr;
    std::vector<std::string> jsonPaths;  ///< FOR JSON property paths
    /// FOR JSON: the rows below are rendered into one TEXT value, so the plan
    /// presents one column named `json`; these describe the rows themselves.
    std::optional<JsonOptions> forJson;
    std::size_t innerColumns = 0;
    std::vector<std::string> innerNames, innerQualified;
    /// The result's column names, shared by every Result this plan produces.
    std::shared_ptr<const ColumnNames> sharedNames;
    std::size_t lastRows = 0;  ///< rows the last top-level run returned (capped)
    // scratch
    RowStore sorted;
    std::vector<std::uint32_t> orderIndex;
};

/// Runs `p`, delivering visible rows to `sink`.
void runSelect(SelectPlan& p, Ctx& c, RowSink& sink);
/// Runs a sub-query plan as its kind requires, caching when uncorrelated.
void runSubquery(SubqueryPlan& s, Ctx& c);

/// Opens/advances one source (filling its slots). Defined in access.cpp.
void openSource(SourceNode& s, Ctx& c);
/// Settles whether the source can be read from an index alone (and, for a
/// plain scan, switches it to the narrowest covering index). Idempotent.
void decideCovering(SourceNode& s);
bool nextSource(SourceNode& s, Ctx& c);
void closeSource(SourceNode& s);
/// Sets the source's slots to NULL (outer join padding).
void nullSource(SourceNode& s, Ctx& c);
/// Which frame slots hold values when a source is opened: everything below
/// `outer` (enclosing queries, parameters' neighbors) plus the sources placed
/// before it in join order.
struct ReadySet
{
    std::size_t outer = 0;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;  ///< [begin, end)
    bool has(std::size_t slot) const noexcept
    {
        if (slot < outer)
            return true;
        for (const auto& r : ranges) {
            if (slot >= r.first && slot < r.second)
                return true;
        }
        return false;
    }
};

/// Chooses an access path for a table source from the conjuncts available.
void planAccess(SourceNode& s, const std::vector<Expr*>& conjuncts, const ReadySet& ready);
/// How good the chosen path is, for join ordering: higher is narrower.
int accessRank(const SourceNode& s) noexcept;

// ------------------------------------------------------------- session ---

/// Per-connection settings and counters (PRAGMAs, changes()).
struct ConnectionState
{
    bool foreignKeys = false;
    bool recursiveTriggers = false;
    bool caseSensitiveLike = false;
    bool deferForeignKeys = false;
    bool ignoreCheckConstraints = false;
    bool queryOnly = false;
    bool reverseUnorderedSelects = false;
    bool legacyAlterTable = false;
    /// Milliseconds a writer waits for the writer slot; 0 waits as long as it
    /// takes. Read by transactions on any thread.
    std::atomic<std::int64_t> busyTimeout{0};
    std::int64_t cacheSize = -2000;
    /// PRAGMA temp_store: 0 default, 1 file, 2 memory (sorts never spill).
    std::atomic<int> tempStore{0};
    /// Bytes a sort may hold before it spills sorted runs to a temp file.
    std::atomic<std::uint64_t> sortMemory{64ull << 20};
    // Transactions of one Database may run on different threads, so the
    // counters they all update are atomic.
    std::atomic<std::int64_t> changes{0};
    std::atomic<std::int64_t> totalChanges{0};
    std::atomic<std::int64_t> lastInsertRowid{0};
    std::atomic<std::uint64_t> runCounter{0};  ///< statement executions, for sub-query caches
};

/// One open store (main, temp or attached) inside a session.
struct SessionStore
{
    /// Views of strings the connection owns for as long as the store is open.
    std::string_view name;
    std::string_view path;
    nosql::Env* env = nullptr;
    nosql::Txn* txn = nullptr;
    Catalog* catalog = nullptr;
};

/// The transaction(s) a statement runs in, across every store it can see.
struct Session
{
    ConnectionState* conn = nullptr;
    std::vector<SessionStore> stores;  ///< 0 main, 1 temp (when open), then attached
    bool writable = false;
    std::int64_t deferredFkViolations = 0;
    std::int64_t immediateFkViolations = 0;
    /// Looks up a schema by name ("main", "temp", attached); -1 if unknown.
    int storeIndex(std::string_view name) const;
    Catalog& catalog(std::size_t i) { return *stores[i].catalog; }
    /// Called when a statement needs the temp store and it is not open yet.
    std::function<void(Session&)> openTemp;
    /// Set from another thread by Database::interrupt().
    const std::atomic<bool>* interrupt = nullptr;
    /// True inside an explicit transaction (PRAGMA foreign_keys is then a no-op).
    bool explicitTxn = false;
    /// Opens the statement's nested transaction, so that a failure part way
    /// through undoes the statement alone. Set by the connection when the
    /// statement runs inside a larger transaction; DML calls it before its
    /// first write unless that write cannot fail half-done. Idempotent.
    std::function<void()> beginAtomic;
    void ensureAtomic()
    {
        if (beginAtomic)
            beginAtomic();
    }
};

}  // namespace sql::internal
