// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The schema: tables, indexes, views and triggers of one store, decoded from
// the CREATE statements kept in its catalog. Decoded once per schema version
// and shared, immutable, by every transaction that sees that version.
//
// Expressions (defaults, CHECK, generated columns, index expressions, partial
// index predicates) are kept as SQL text; whoever needs to evaluate one parses
// it into a tree of its own, so nothing here is mutated after decoding.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "internal/ast.hpp"
#include "internal/types.hpp"

namespace sql::internal {

struct Column
{
    std::string name;
    std::string declType;  ///< as written; empty when none
    Affinity affinity = Affinity::Blob;
    StrictType strictType = StrictType::Any;  ///< meaningful in STRICT tables
    Collation collation = Collation::Binary;
    bool notNull = false;
    bool declaredNotNull = false;  ///< NOT NULL as written (PRAGMA table_info)
    Conflict notNullConflict = Conflict::Default;
    bool primaryKey = false;
    std::uint8_t generated = 0;  ///< 0 none, 1 VIRTUAL, 2 STORED
    std::string generatedSql;
    bool hasDefault = false;
    std::string defaultSql;    ///< the DEFAULT expression's text
    bool defaultConstant = true;  ///< a literal, so `defaultValue` holds it
    Value defaultValue;
};

struct IndexPart
{
    int column = -1;        ///< table column, -1 for an expression, -2 for the rowid
    std::string exprSql;    ///< expression index part
    Collation collation = Collation::Binary;
    bool desc = false;
};

struct Index
{
    std::string name;
    std::string table;
    std::vector<IndexPart> parts;
    bool unique = false;
    Conflict onConflict = Conflict::Default;
    std::string whereSql;     ///< partial index predicate, or empty
    std::uint8_t origin = 0;  ///< 0 CREATE INDEX, 1 UNIQUE constraint, 2 PRIMARY KEY
    std::string sql;          ///< empty for implicit indexes
    std::uint64_t id = 0;
    std::string entriesDb;
    unsigned position = 0;    ///< in Schema::indexes, for handle caches

    bool hasExpressions() const noexcept;
};

struct ForeignKey
{
    std::vector<int> columns;  ///< child columns, positions
    std::string parentTable;
    std::vector<std::string> parentColumns;  ///< empty: the parent's primary key
    FkAction onDelete = FkAction::NoAction;
    FkAction onUpdate = FkAction::NoAction;
    bool deferred = false;
};

struct Check
{
    std::string name;
    std::string sql;
};

struct Table
{
    std::string name;
    std::string sql;
    std::uint64_t id = 0;
    std::string dataDb;
    bool strict = false;
    bool withoutRowid = false;
    bool autoincrement = false;
    std::vector<Column> columns;
    std::vector<int> primaryKey;         ///< column positions, in key order
    std::vector<bool> pkDesc;
    std::vector<Collation> pkCollation;
    Conflict pkConflict = Conflict::Default;
    int rowidAlias = -1;                 ///< the INTEGER PRIMARY KEY column, or -1
    std::vector<ForeignKey> foreignKeys;
    std::vector<Check> checks;
    std::vector<const Index*> indexes;   ///< filled in by the schema
    unsigned position = 0;

    bool hasRowid() const noexcept { return !withoutRowid; }
    /// Rows are stored under the rowid (8 raw bytes) rather than the declared key.
    bool keyedByRowid() const noexcept { return hasRowid() && (primaryKey.empty() || rowidAlias >= 0); }
    /// A rowid table clustered by a declared key keeps its rowid in the payload.
    bool payloadRowid() const noexcept { return hasRowid() && !keyedByRowid(); }
    /// Slots in a row buffer: the columns, then the rowid for rowid tables.
    std::size_t width() const noexcept { return columns.size() + (hasRowid() ? 1 : 0); }
    int rowidSlot() const noexcept { return hasRowid() ? int(columns.size()) : -1; }
    /// Case-insensitive; understands rowid/oid/_rowid_ unless a column has
    /// that name. -1 when absent.
    int find(std::string_view column) const noexcept;
    bool hasGenerated() const noexcept;
};

struct View
{
    std::string name;
    std::string sql;
    std::uint64_t id = 0;
};

struct Trigger
{
    std::string name;
    std::string table;
    std::string sql;
    TriggerTime time = TriggerTime::Before;
    TriggerEvent event = TriggerEvent::Insert;
    std::vector<std::string> updateOf;
    std::uint64_t id = 0;
};

/// A process-wide counter: every schema object, and every edit made to one,
/// takes a fresh value.
std::uint64_t nextSchemaStamp() noexcept;

/// One immutable decoded schema.
struct Schema
{
    std::uint64_t version = 0;
    /// Changes whenever the object is edited (a transaction's DDL edits its
    /// copy in place), so a plan can tell "same object" from "same content".
    std::uint64_t stamp = nextSchemaStamp();
    std::vector<std::unique_ptr<Table>> tables;
    std::vector<std::unique_ptr<Index>> indexes;
    std::vector<std::unique_ptr<View>> views;
    std::vector<std::unique_ptr<Trigger>> triggers;
    std::int64_t userVersion = 0;
    std::int64_t applicationId = 0;

    const Table* findTable(std::string_view name) const noexcept;
    const Index* findIndex(std::string_view name) const noexcept;
    const View* findView(std::string_view name) const noexcept;
    const Trigger* findTrigger(std::string_view name) const noexcept;
    /// Triggers on `table` for `event`, in creation order.
    std::vector<const Trigger*> triggersOn(std::string_view table) const;
    /// Tables whose foreign keys name `parent`.
    std::vector<const Table*> childrenOf(std::string_view parent) const;

    /// Deep copy, for editing under DDL.
    std::shared_ptr<Schema> clone() const;
    /// Rebuilds positions and the table -> index links after an edit.
    void settle();
};

/// Builds a Table from its CREATE TABLE statement, validating it the way
/// SQLite does. `sql` is the text to store. Throws on invalid definitions.
Table buildTable(const CreateTableStmt& s, std::string sql);

/// The implicit indexes a table's UNIQUE (and, for WITHOUT ROWID tables, no)
/// constraints need, named sqlite_autoindex_<table>_<n>.
std::vector<Index> implicitIndexes(const Table& t, const CreateTableStmt& s);

/// Builds an Index from CREATE INDEX against `t`.
Index buildIndex(const CreateIndexStmt& s, const Table& t, std::string sql);

/// "CREATE TABLE name..." text from a statement and its source.
std::string normalizedCreateSql(std::string_view source, const StatementNode& node,
                                std::uint32_t nameBegin, const char* prefix);

}  // namespace sql::internal
