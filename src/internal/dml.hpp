// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Writing rows: the constraint checks, index maintenance, triggers and
// foreign-key actions shared by INSERT, UPDATE, DELETE, REPLACE and upsert.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "internal/plan.hpp"
#include "internal/raise.hpp"

namespace sql::internal {

struct TablePlan;
struct StatementResult;

/// A trigger body planned for one recursion depth: each statement is its own
/// program, with NEW and OLD at fixed slots of its frame.
struct TriggerInstance
{
    std::vector<std::unique_ptr<Program>> body;
    std::vector<std::size_t> newBase, oldBase;  ///< per body statement
    std::unique_ptr<Program> whenProgram;
    Expr* when = nullptr;
    std::size_t whenNew = 0, whenOld = 0;
};

struct TriggerPlan
{
    const Trigger* trigger = nullptr;
    std::vector<int> updateOf;  ///< columns for UPDATE OF
    std::vector<std::unique_ptr<TriggerInstance>> byDepth;
};

struct IndexWriter
{
    const Index* index = nullptr;
    std::vector<ExprPtr> newParts, oldParts;  ///< expression parts, null for columns
    ExprPtr newWhere, oldWhere;               ///< partial index predicate
    /// UPDATE: the entry (without the row key) the old row had, and whether
    /// the new row's is identical -- then the entry is left alone.
    std::string oldEntry, oldFlags;
    bool unchanged = false;
};

struct FkParent
{
    const ForeignKey* fk = nullptr;
    const Table* parent = nullptr;
    std::size_t store = 0;
    std::vector<int> parentColumns;  ///< in the parent, matching fk->columns
    const Index* parentIndex = nullptr;  ///< null when the parent key is its primary key
    bool parentIsRowid = false;
};

struct FkChild
{
    const Table* child = nullptr;
    const ForeignKey* fk = nullptr;
    std::size_t store = 0;
    std::vector<int> parentColumns;
    TablePlan* plan = nullptr;  ///< the child's writer, for CASCADE / SET NULL
    const Index* childIndex = nullptr;  ///< an index whose leading columns are the FK
};

struct TablePlan
{
    const Table* table = nullptr;
    std::size_t store = 0;
    std::size_t newBase = 0, oldBase = 0, excludedBase = 0;
    bool view = false;  ///< a view written through INSTEAD OF triggers
    std::vector<ExprPtr> defaults;     ///< per column; null when constant or none
    std::vector<ExprPtr> generated;    ///< per column, against the new row
    std::vector<ExprPtr> oldGenerated; ///< per column, against the old row
    std::vector<ExprPtr> checks;
    std::vector<std::string> checkNames;
    std::vector<IndexWriter> indexes;
    std::vector<TriggerPlan> triggers;
    std::vector<FkParent> parents;
    std::vector<FkChild> children;
    bool fkPlanned = false;
    // scratch
    std::string key, oldKey, payload, entry, probe;
    std::string flags;  ///< an index entry's value: see entryFor
};

/// The conflict algorithm in force for one statement.
struct WriteMode
{
    Conflict statement = Conflict::Default;  ///< OR ... / REPLACE INTO
};

struct UpsertPlan
{
    const Index* index = nullptr;  ///< null: the primary key; ignored when `any`
    bool primaryKey = false;
    bool any = false;              ///< no conflict target: any uniqueness constraint
    bool doNothing = false;
    std::vector<std::pair<int, Expr*>> set;  ///< column, value (against old row + excluded)
    Expr* where = nullptr;
    Expr* targetWhere = nullptr;
};

class Writer
{
public:
    Writer(Planner& planner, Program& prog);

    /// Plans everything needed to write `t` (and, with foreign keys on, the
    /// tables its actions reach).
    TablePlan& plan(const Table& t, std::size_t store);

    /// Fills the new row's defaults (and NULLs) into slots[newBase...].
    void initRow(TablePlan& tp, Ctx& c);
    /// Inserts the new row in slots[newBase...]. Returns false when the row
    /// was skipped (OR IGNORE, RAISE(IGNORE), DO NOTHING). `upserts` may be
    /// empty. Sets `rowid` to the row's rowid (rowid tables).
    bool insert(TablePlan& tp, Ctx& c, Conflict mode, std::vector<UpsertPlan>* upserts,
                std::int64_t& rowid, bool fireTriggers = true);
    /// Rewrites the row stored under `oldKey` (old values in slots[oldBase...])
    /// with the new values in slots[newBase...]. False when skipped.
    bool update(TablePlan& tp, Ctx& c, Conflict mode, const std::string& oldKey,
                const std::vector<int>* changed, bool fireTriggers = true);
    /// Deletes the row under `key` whose values are in slots[oldBase...].
    bool remove(TablePlan& tp, Ctx& c, const std::string& key, bool fireTriggers = true);

    /// Loads the row stored under `key` into slots[base...] of `tp`'s table.
    bool loadStored(TablePlan& tp, Ctx& c, std::string_view key, std::size_t base);
    /// The storage key of the row in slots[base...].
    void storageKey(TablePlan& tp, Ctx& c, std::size_t base, std::string& out);

    /// Writes `ix`'s entries for every stored row of tp's table, enforcing
    /// uniqueness (CREATE INDEX, REINDEX).
    void backfill(TablePlan& tp, Ctx& c, const Index& ix);
    /// PRAGMA integrity_check: looks up the entry every row of the table
    /// implies in `ix`, reporting each missing one through `missing` (with
    /// the row's rowid, or its ordinal for a WITHOUT ROWID table). Returns how
    /// many entries the index should hold.
    std::uint64_t verifyEntries(TablePlan& tp, Ctx& c, const Index& ix,
                                const std::function<void(std::int64_t row)>& missing);

    /// Runs triggers of `time`/`event` for the row(s) in tp's new/old slots.
    /// Returns false if RAISE(IGNORE) asked to skip the row.
    bool fire(TablePlan& tp, Ctx& c, TriggerTime time, TriggerEvent event,
              const std::vector<int>* changed);

    /// Number of rows changed by trigger programs (not counted in changes()).
    std::int64_t nested = 0;
    /// The last insert() resolved its conflict with DO UPDATE.
    bool upserted = false;

private:
    void planFks(TablePlan& tp);
    ExprPtr bindRowExpr(const std::string& sql, TablePlan& tp, std::size_t base, unsigned flags);
    void computeGenerated(TablePlan& tp, Ctx& c, std::size_t base, bool stored, bool virt);
    bool applyColumnRules(TablePlan& tp, Ctx& c, Conflict mode);
    bool checkConstraints(TablePlan& tp, Ctx& c, Conflict mode);
    /// Writes the new row under `key`; with `unique`, refuses (returns false)
    /// when the key is taken, which is how a deferred key check fails.
    bool writeRow(TablePlan& tp, Ctx& c, const std::string& key, bool unique = false);
    void addEntries(TablePlan& tp, Ctx& c, const std::string& key, bool skipUnchanged = false);
    void removeEntries(TablePlan& tp, Ctx& c, const std::string& key, bool skipUnchanged = false);
    /// The entry (without the row key) the row at `base` has in `iw`'s
    /// index; false when a partial index leaves the row out. With `flags`,
    /// also the entry's value: a bit per component (index parts, then the
    /// row key's parts for a clustered table) holding a REAL that is a whole
    /// number, which the key alone would read back as an INTEGER. Empty when
    /// there is none, so most entries store nothing.
    bool entryFor(TablePlan& tp, IndexWriter& iw, Ctx& c, std::size_t base, bool isNew,
                  std::string& out, bool& hasNull, std::string* flags = nullptr);
    /// Finds a row (other than `self`) that collides on `iw`; its key in `found`.
    bool findConflict(TablePlan& tp, IndexWriter& iw, Ctx& c, const std::string& self,
                      std::string& found);
    void fkChildCheck(TablePlan& tp, Ctx& c, std::size_t base, int delta, const std::vector<int>* changed);
    void fkParentActions(TablePlan& tp, Ctx& c, bool isDelete, const std::vector<int>* changed);
    std::int64_t countChildren(FkChild& fc, Ctx& c, std::size_t parentBase, TablePlan& parentPlan);
    void runTriggerBody(TriggerPlan& tr, TablePlan& tp, Ctx& c);

    Planner& planner_;
    Program& prog_;
    std::map<const Table*, std::unique_ptr<TablePlan>> plans_;
};

/// The conflict-violation error for constraint kind `k`, thrown according to
/// `mode` (FAIL and ROLLBACK use their own exception types).
[[noreturn]] void conflictError(Conflict mode, Constraint k, const std::string& message);

}  // namespace sql::internal
