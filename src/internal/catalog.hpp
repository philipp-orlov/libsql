// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The schema: what tables and indexes exist, where their sub-databases live,
// and where the next implicit row key comes from.
//
// The decoded schema is an immutable snapshot shared between transactions.
// A transaction that runs no DDL borrows the database's cached snapshot after
// one lookup of the catalog's version record; one that does copies the
// snapshot, edits the copy, and publishes it when it commits.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nosql/nosql.hpp"
#include "sql/sql.hpp"

namespace sql::internal {

struct Index
{
    std::string name;
    std::string table;
    std::vector<int> columns;  ///< positions in Table::columns
    bool unique = false;

    /// Filled in by the schema: the sub-database holding the entries, and the
    /// index's position in Schema::indexes.
    std::string entriesDb;
    unsigned id = 0;
};

struct Table
{
    std::string name;
    std::vector<ColumnInfo> columns;
    /// Positions of the PRIMARY KEY columns in key order. Empty when the table
    /// falls back to an implicit integer key; that key is addressable in SQL as
    /// `rowid` and occupies the slot just past the declared columns.
    std::vector<int> primaryKey;
    std::vector<Index> indexes;

    /// Filled in by the schema: the sub-database holding the rows, the catalog
    /// key of the rowid counter, and the table's position in Schema::tables.
    std::string dataDb;
    std::string rowidKey;
    unsigned id = 0;

    /// -1 when absent. Case-insensitive, and understands `rowid`.
    int find(std::string_view column) const noexcept;
    bool hasRowid() const noexcept { return primaryKey.empty(); }
    /// The slots whose values, encoded in this order and concatenated, form the
    /// key a row is stored under: the primary key columns, or the rowid slot.
    const std::vector<int>& keySlots() const noexcept { return keySlots_; }
    /// The slot an automatically assigned key lands in; only meaningful when
    /// autoKey() holds.
    int autoKeySlot() const noexcept
    {
        return primaryKey.empty() ? int(columns.size()) : primaryKey[0];
    }
    /// Declared type of a slot, `rowid` included.
    Type slotType(int slot) const noexcept
    {
        return std::size_t(slot) < columns.size() ? columns[std::size_t(slot)].type
                                                  : Type::Integer;
    }
    /// True when the key is one integer the engine may hand out itself.
    bool autoKey() const noexcept
    {
        return primaryKey.empty() ||
               (primaryKey.size() == 1 && slotType(primaryKey[0]) == Type::Integer);
    }
    std::size_t width() const noexcept { return columns.size() + (hasRowid() ? 1 : 0); }

    /// Derives the cached names and slots above; the schema calls it after
    /// every change to the table.
    void settle(unsigned id);

private:
    std::vector<int> keySlots_;
};

/// One immutable decoded schema. Tables keep their position for the life of a
/// snapshot, so ids index straight into per-transaction handle caches.
struct Schema
{
    std::uint64_t version = 0;
    std::vector<Table> tables;  ///< sorted by lowercased name
    std::size_t indexCount = 0;

    const Table* find(std::string_view name) const noexcept;
    const Index* findIndex(std::string_view name) const noexcept;
};

/// The database-wide cache of the newest decoded schema.
struct SchemaCache
{
    std::mutex mtx;
    std::shared_ptr<const Schema> schema;
    /// The store commit at which `schema` was last known to be current. Only
    /// this process writes the store, so a transaction whose snapshot is that
    /// commit can take the schema without looking at the catalog at all; any
    /// other snapshot reads the version record and decides from that.
    std::uint64_t validAtTxn = 0;
    /// Whether the catalog sub-database has been seen to exist. It is never
    /// dropped, so once true this saves a lookup per transaction.
    bool catalogSeen = false;
};

/// The schema as one transaction sees it, plus that transaction's handles to
/// the sub-databases it has touched and its cached rowid counters.
class Catalog
{
public:
    Catalog(nosql::Txn& txn, SchemaCache& cache);
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    const Table* find(std::string_view name) const noexcept { return schema_->find(name); }
    const Table& get(std::string_view name) const;  ///< throws NoSuchTable
    std::vector<std::string> tableNames() const;
    const Index* findIndex(std::string_view name) const noexcept
    {
        return schema_->findIndex(name);
    }
    std::vector<const Index*> allIndexes() const;

    void addTable(Table table);
    void dropTable(const Table& table);
    void addIndex(Index index);
    void dropIndex(const Index& index);

    /// The sub-database holding `table`'s rows, keyed by encoded primary key.
    const nosql::Db& rows(const Table& table) const;
    /// The sub-database holding `index`'s entries: encoded columns then the
    /// encoded primary key, with an empty payload.
    const nosql::Db& entries(const Index& index) const;

    /// Reserves the next implicit key for `table` and records it.
    std::int64_t nextRowid(const Table& table);
    /// Keeps the counter ahead of an explicitly supplied integer key.
    void noteRowid(const Table& table, std::int64_t used);

    /// Writes back whatever a statement left pending -- rowid counters -- so
    /// the store agrees with this object. Called after every statement.
    void flush();

    /// After this write transaction committed as store commit `txnid`: the
    /// schema it ran with -- edited or not -- is current as of that commit.
    void committed(std::uint64_t txnid);

    nosql::Txn& txn() const noexcept { return *txn_; }
    /// The snapshot this transaction plans against; a plan made against the
    /// same object stays valid.
    const std::shared_ptr<const Schema>& schema() const noexcept { return schema_; }

private:
    struct Handle
    {
        bool open = false;
        nosql::Db db;
    };
    struct Rowid
    {
        bool loaded = false;
        bool dirty = false;
        std::int64_t last = 0;
    };

    const nosql::Db& meta(bool create = false) const;
    /// The private, editable copy of the schema; made on the first DDL.
    Schema& edit();
    void storeTable(const Table& table);
    void storeVersion();
    void resetHandles();
    std::int64_t lastRowid(const Table& table);

    nosql::Txn* txn_;
    SchemaCache* cache_;
    std::shared_ptr<const Schema> schema_;
    std::shared_ptr<Schema> edited_;  ///< set once this transaction ran DDL
    mutable Handle meta_;
    mutable std::vector<Handle> tableHandles_;
    mutable std::vector<Handle> indexHandles_;
    std::vector<Rowid> rowids_;
};

}  // namespace sql::internal
