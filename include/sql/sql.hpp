// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- a small SQL engine over a libnosql store.
//
//   * CREATE / DROP TABLE and INDEX, INSERT, SELECT (with INNER JOIN),
//     UPDATE, DELETE
//   * NULL, INTEGER, REAL, TEXT, DATETIME and BLOB values, PRIMARY KEY,
//     NOT NULL, UNIQUE and DEFAULT constraints
//   * every statement runs inside a libnosql transaction, so it inherits
//     that store's ACID guarantees and MVCC snapshot reads
//
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sql/error.hpp"
#include "sql/value.hpp"

namespace nosql {
class Env;
class Txn;
}  // namespace nosql

namespace sql {

namespace internal {
struct DatabaseImpl;
struct TxnImpl;
struct StatementImpl;
struct ResultBuilder;
}  // namespace internal

class Database;
class Transaction;
class Statement;

// -------------------------------------------------------------- schema ----

struct ColumnInfo
{
    std::string name;
    Type type = Type::Text;
    bool primaryKey = false;
    bool notNull = false;
    bool unique = false;
    bool hasDefault = false;
    Value defaultValue;
};

struct TableInfo
{
    std::string name;
    std::vector<ColumnInfo> columns;
    /// Positions in `columns` of the PRIMARY KEY, in key order; one entry for
    /// a plain key, several for a composite one. Empty when the table has an
    /// implicit integer key, which SQL still addresses under the name `rowid`.
    std::vector<int> primaryKey;
};

struct IndexInfo
{
    std::string name;
    std::string table;
    std::vector<std::string> columns;
    bool unique = false;
};

// ------------------------------------------------------------- results ----

/// One result row. Values are owned, so a row outlives the statement and the
/// transaction that produced it.
class Row
{
public:
    Row() = default;

    std::size_t size() const noexcept { return values_.size(); }
    bool empty() const noexcept { return values_.empty(); }

    const Value& at(std::size_t i) const;
    const Value& at(std::string_view column) const;
    const Value& operator[](std::size_t i) const { return at(i); }
    const Value& operator[](std::string_view column) const { return at(column); }
    bool has(std::string_view column) const noexcept;

    const std::vector<Value>& values() const noexcept { return values_; }
    std::vector<Value>::const_iterator begin() const noexcept { return values_.begin(); }
    std::vector<Value>::const_iterator end() const noexcept { return values_.end(); }

private:
    friend class Result;
    friend struct internal::ResultBuilder;
    Row(std::shared_ptr<const std::vector<std::string>> names, std::vector<Value> values)
        : names_(std::move(names)), values_(std::move(values))
    {}

    std::shared_ptr<const std::vector<std::string>> names_;
    std::vector<Value> values_;
};

/// What a statement produced: rows for SELECT, counters for everything else.
class Result
{
public:
    Result() = default;

    const std::vector<std::string>& columns() const;
    std::size_t size() const noexcept { return rows_.size(); }
    bool empty() const noexcept { return rows_.empty(); }

    const Row& at(std::size_t i) const;
    const Row& operator[](std::size_t i) const { return at(i); }
    std::vector<Row>::const_iterator begin() const noexcept { return rows_.begin(); }
    std::vector<Row>::const_iterator end() const noexcept { return rows_.end(); }

    /// Rows inserted, updated or deleted by the statement.
    std::uint64_t changes() const noexcept { return changes_; }
    /// Key of the last row INSERT gave an integer primary key to; 0 otherwise.
    std::int64_t lastInsertId() const noexcept { return lastInsertId_; }

private:
    friend struct internal::ResultBuilder;
    std::shared_ptr<const std::vector<std::string>> names_;
    std::vector<Row> rows_;
    std::uint64_t changes_ = 0;
    std::int64_t lastInsertId_ = 0;
};

/// Values bound to the `?` placeholders of a statement, left to right.
using Params = std::vector<Value>;

// --------------------------------------------------------- transaction ----

/// A explicit multi-statement transaction. Move-only, and rolls back if it goes
/// out of scope without a commit.
class Transaction
{
public:
    Transaction() noexcept = default;
    Transaction(Transaction&&) noexcept;
    Transaction& operator=(Transaction&&) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    bool valid() const noexcept { return impl_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// Runs every `;`-separated statement in `text` and returns the result of
    /// the last one.
    Result exec(std::string_view text, const Params& params = {});

    void commit();
    void rollback();

    /// The libnosql transaction underneath, for reading or writing something
    /// that lives beside the tables in the same store -- a
    /// nosql::BlobStorage index, say -- under this transaction's snapshot and
    /// commit. Borrowed: valid until this transaction finishes.
    nosql::Txn& txn();

private:
    friend class Database;
    friend class Statement;
    explicit Transaction(std::shared_ptr<internal::TxnImpl> impl) noexcept : impl_(std::move(impl))
    {}
    std::shared_ptr<internal::TxnImpl> impl_;
};

// ----------------------------------------------------- prepared statement --

/// A statement parsed once and run as often as you like.
///
/// Parsing is what `prepare` gets out of the way, and the plan made on the
/// first run is kept until the schema changes, so a prepared statement keeps
/// working after the schema changes and always uses whatever indexes exist at
/// the time.
///
/// A statement belongs to the database it was prepared from, and holds it open.
/// It is move-only and not safe to run from two threads at once.
class Statement
{
public:
    Statement() noexcept = default;
    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    bool valid() const noexcept { return impl_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// The text this was prepared from.
    const std::string& sql() const;
    /// How many `?` placeholders it carries.
    std::size_t parameters() const;
    /// True when running it needs a write transaction.
    bool writes() const;

    /// Bind one parameter. `index` is 1-based, as in SQLite. Parameters that
    /// are never bound are NULL.
    Statement& bind(std::size_t index, Value value);
    /// Bind every parameter at once; the count must match `parameters()`.
    Statement& bind(const Params& params);
    /// Put every parameter back to NULL.
    Statement& clear();

    /// Run with the bindings currently in place, in a transaction of its own.
    Result exec();
    /// Run inside a transaction the caller controls.
    Result exec(Transaction& txn);
    /// Bind, then run.
    Result exec(const Params& params) { return bind(params).exec(); }
    Result exec(Transaction& txn, const Params& params) { return bind(params).exec(txn); }

private:
    friend class Database;
    explicit Statement(std::shared_ptr<internal::StatementImpl> impl) noexcept
        : impl_(std::move(impl))
    {}
    std::shared_ptr<internal::StatementImpl> impl_;
};

// ------------------------------------------------------------ database ----

/// The database: one libnosql store holding the catalog, the table data and
/// the indexes.
class Database
{
public:
    /// Fluent open builder: `Database::configure().maxSize(1 << 30).open(path)`.
    class Options
    {
    public:
        /// Hard upper bound on the store file. It grows on demand up to this.
        /// The default is 64 GiB: a ceiling for a database that owns its disk,
        /// not a budget. Set a bound from the consumer's own capacity when the
        /// volume is shared, so a runaway writer fails with MapFull instead of
        /// filling it.
        Options& maxSize(std::uint64_t bytes)
        {
            maxSize_ = bytes;
            return *this;
        }
        Options& readOnly(bool on = true)
        {
            readOnly_ = on;
            return *this;
        }
        Options& createIfMissing(bool on = true)
        {
            create_ = on;
            return *this;
        }
        /// Off trades crash durability for throughput on bulk loads; the store
        /// still opens on a consistent snapshot either way.
        Options& durable(bool on = true)
        {
            durable_ = on;
            return *this;
        }
        /// Let reads trust a page checksum the process has already verified
        /// since that page was last written, instead of re-hashing every page
        /// on every access. Roughly halves the cost of a point lookup. The
        /// trade is that damage done to the file from outside -- a media or
        /// memory fault -- is noticed on the next verification rather than the
        /// next read; every remembered check expires after `revalidateAfter`
        /// (default one minute), and writes always verify. Off by default.
        Options& cacheReadChecksums(bool on = true,
                                    std::chrono::milliseconds revalidateAfter = std::chrono::minutes(1))
        {
            cacheReads_ = on;
            revalidateAfter_ = revalidateAfter;
            return *this;
        }

        Database open(const std::filesystem::path& path) const;

    private:
        std::uint64_t maxSize_ = 64ull << 30;
        bool readOnly_ = false;
        bool create_ = true;
        bool durable_ = true;
        bool cacheReads_ = false;
        std::chrono::milliseconds revalidateAfter_{60000};
    };

    static Options configure() { return Options(); }

    Database() noexcept = default;
    Database(Database&&) noexcept = default;
    Database& operator=(Database&&) noexcept = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    /// Convenience: open with all defaults.
    explicit Database(const std::filesystem::path& path) { *this = configure().open(path); }

    bool valid() const noexcept { return impl_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    const std::filesystem::path& path() const;

    /// Runs every `;`-separated statement in `text` in one transaction and
    /// returns the result of the last one. Committed on return, rolled back if
    /// anything throws.
    Result exec(std::string_view text, const Params& params = {});

    /// Parse `text` now so that running it later costs no parsing. Worth it for
    /// anything that runs in a loop; `exec` is fine for one-offs.
    Statement prepare(std::string_view text);

    /// Open a transaction spanning several `exec` calls. Only one write
    /// transaction may be live at a time, so this blocks until any other one
    /// finishes.
    Transaction begin();
    /// A transaction that only reads. It takes a snapshot instead of the
    /// writer slot, so it neither blocks nor is blocked, and it rejects any
    /// statement that would write.
    Transaction beginRead() const;

    /// A read-only handle on the same open store, for another thread. It
    /// shares the file, mapping and lock (no second open) but owns its own
    /// schema cache and statements, so a `Database` per thread needs no
    /// external locking: readers run concurrently with each other and with the
    /// writer, each on a snapshot. `exec`, `prepare` and `begin` on it reject
    /// anything that writes with `ErrorCode::ReadOnly`. It stays usable after
    /// the `Database` it came from is closed.
    ///
    /// A single `Database` (or `Statement`) is still not safe to use from two
    /// threads at once; hand each thread its own reader.
    Database openReader() const;

    std::vector<std::string> tables() const;
    TableInfo table(std::string_view name) const;
    /// Every index, or only those on `table` when a name is given.
    std::vector<IndexInfo> indexes(std::string_view table = {}) const;

    /// The libnosql store underneath, so other libnosql layers can share the
    /// file with the tables: `nosql::BlobStorage::open(db.env(), "images")`
    /// keeps bulk payloads in a side-car archive whose index lives in this
    /// database. Borrowed: valid while this Database is open, so close it
    /// only after everything built on the Env is gone. Sub-database names
    /// used by libsql itself are reserved; pick your own.
    nosql::Env& env() const;

    void close();

private:
    std::shared_ptr<internal::DatabaseImpl> impl_;
};

/// Library version, as "major.minor.patch".
const char* version() noexcept;

}  // namespace sql
