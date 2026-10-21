// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The schema as one transaction sees it, plus that transaction's handles on
// the sub-databases it has touched and its rowid bookkeeping.
//
// On disk everything lives in the sub-database `sql_catalog`:
//
//   'o' + lower(name)   table, index or view: kind, name, table, id, sql
//   'g' + lower(name)   trigger: name, table, id, sql
//   'a' + id            AUTOINCREMENT high-water mark (sqlite_sequence)
//   'p' + name          persistent pragma (user_version, application_id)
//   'v'                 schema version, bumped by every DDL statement
//   'n'                 next object id
//
// Rows of a table live in sub-database "t#<id>", entries of an index in
// "i#<id>"; the names carry no object name, so renaming is a catalog edit.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "internal/schema.hpp"
#include "nosql/nosql.hpp"

namespace sql::internal {

/// The newest decoded schema of one store, shared by the transactions of one
/// connection.
struct SchemaCache
{
    std::mutex mtx;
    std::shared_ptr<const Schema> schema;
    std::uint64_t validAtTxn = 0;
    bool catalogSeen = false;
};

class Catalog
{
public:
    Catalog() = default;
    Catalog(nosql::Txn& txn, SchemaCache& cache) { open(txn, cache); }
    Catalog(const Catalog&) = delete;
    /// Starts serving a new transaction, keeping this object's buffers.
    void open(nosql::Txn& txn, SchemaCache& cache);
    /// Lets go of the transaction and everything that pins it.
    void close();
    Catalog& operator=(const Catalog&) = delete;

    const Schema& schema() const noexcept { return *schema_; }
    const std::shared_ptr<const Schema>& snapshot() const noexcept { return schema_; }

    const Table* findTable(std::string_view name) const noexcept { return schema_->findTable(name); }
    const Index* findIndex(std::string_view name) const noexcept { return schema_->findIndex(name); }
    const View* findView(std::string_view name) const noexcept { return schema_->findView(name); }
    const Trigger* findTrigger(std::string_view name) const noexcept
    {
        return schema_->findTrigger(name);
    }

    // --- DDL: each writes its records and edits this transaction's schema ---
    std::uint64_t newId();
    void addTable(Table t);
    void addIndex(Index ix);
    void addView(View v);
    void addTrigger(Trigger t);
    void dropTable(std::string_view name);  ///< with its indexes and triggers, and its data
    void dropIndex(std::string_view name);  ///< and its entries
    void dropView(std::string_view name);
    void dropTrigger(std::string_view name);
    /// Replaces a table's definition (ALTER TABLE): same id and data.
    void replaceTable(std::string_view oldName, Table t);
    void replaceIndex(std::string_view oldName, Index ix);
    void replaceView(std::string_view oldName, View v);
    void replaceTrigger(std::string_view oldName, Trigger t);
    void setUserVersion(std::int64_t v);
    void setApplicationId(std::int64_t v);

    // --- data ---
    const nosql::Db& rows(const Table& t);
    const nosql::Db& entries(const Index& ix);
    nosql::Txn& txn() const noexcept { return *levels_.back().txn; }
    nosql::Txn& rootTxn() const noexcept { return *levels_.front().txn; }
    bool readOnly() const;

    // --- rowids ---
    /// The rowid an insert that names none gets: one past the largest in use
    /// (SQLite's rule), or past the AUTOINCREMENT high-water mark. Throws
    /// "database or disk is full" when the rowid space is exhausted.
    std::int64_t nextRowid(const Table& t);
    /// An insert used `rowid`; keep the bookkeeping ahead of it.
    void noteRowid(const Table& t, std::int64_t rowid);
    /// A row was deleted; the largest rowid may have gone.
    void rowidRemoved(const Table& t, std::int64_t rowid);
    /// AUTOINCREMENT high-water mark (sqlite_sequence.seq); 0 when none.
    std::int64_t sequence(const Table& t);
    void setSequence(const Table& t, std::int64_t seq);

    /// Writes back pending rowid high-water marks (AUTOINCREMENT, and the
    /// hidden rowid of a table with a declared key): at commit, before a
    /// nested level opens and before DDL, so each is written once per
    /// transaction and only when it rose.
    void flush();
    /// After a successful commit as store transaction `txnid`.
    void committed(std::uint64_t txnid);

    // --- savepoints and statement transactions ---
    /// `child` was just opened on top of the current transaction.
    void pushLevel(nosql::Txn& child);
    /// The innermost level ended: committed into its parent, or aborted.
    void popLevel(bool committed);
    std::size_t depth() const noexcept { return levels_.size(); }

private:
    struct Handle
    {
        std::uint64_t serial = 0;
        nosql::Db db;
    };
    struct RowidState
    {
        bool known = false;
        std::int64_t max = 0;
        bool seqLoaded = false;
        bool seqDirty = false;
        std::int64_t seq = 0;
    };
    struct Level
    {
        nosql::Txn* txn;
        std::shared_ptr<const Schema> schema;
        std::shared_ptr<Schema> edited;
    };

    const nosql::Db& meta(bool create = false);
    Schema& edit();
    void putObject(char prefix, std::string_view name, char kind, std::string_view table,
                   std::uint64_t id, std::string_view sql);
    void eraseObject(char prefix, std::string_view name);
    void storeVersion();
    void resetCaches();
    RowidState& rowidState(const Table& t);
    std::int64_t largestRowid(const Table& t);

    SchemaCache* cache_ = nullptr;
    std::shared_ptr<const Schema> schema_;
    std::shared_ptr<Schema> edited_;
    std::vector<Level> levels_;
    std::uint64_t serial_ = 1;
    Handle meta_;
    std::vector<Handle> tableHandles_;
    std::vector<Handle> indexHandles_;
    std::vector<RowidState> rowids_;
};

/// Decodes the schema stored in `txn`'s catalog (empty when there is none).
std::shared_ptr<const Schema> loadSchema(nosql::Txn& txn);

}  // namespace sql::internal
