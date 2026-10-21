// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- a small SQL engine over a libnosql store.
//
//   * the SQL language of SQLite 3.45, plus libsql's FOR JSON clause
//   * NULL, INTEGER, REAL, TEXT, BLOB and (libsql's) DATETIME values
//   * every statement runs inside a libnosql transaction, so it inherits
//     that store's ACID guarantees and MVCC snapshot reads
//
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "sql/error.hpp"
#include "sql/value.hpp"

namespace nosql {
class Env;
class Txn;
enum class Durability;
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
    /// The storage class a STRICT column holds; Type::Null ("any") for ANY
    /// columns and for every column of an ordinary table, which follows
    /// SQLite's flexible typing.
    Type type = Type::Null;
    std::string declaredType;  ///< as written in CREATE TABLE
    std::string affinity;      ///< INTEGER, REAL, NUMERIC, TEXT or BLOB
    std::string collation;     ///< BINARY, NOCASE or RTRIM
    bool primaryKey = false;
    bool notNull = false;
    bool unique = false;
    bool hasDefault = false;
    Value defaultValue;        ///< when the default is a constant
    std::string defaultSql;    ///< the DEFAULT expression as written
    int generated = 0;         ///< 0 no, 1 VIRTUAL, 2 STORED
};

struct TableInfo
{
    std::string name;
    std::vector<ColumnInfo> columns;
    /// Positions in `columns` of the PRIMARY KEY, in key order; one entry for
    /// a plain key, several for a composite one. Empty when the table has an
    /// implicit integer key, which SQL still addresses under the name `rowid`.
    std::vector<int> primaryKey;
    bool strict = false;
    bool withoutRowid = false;
    std::string sql;  ///< the CREATE TABLE statement, as sqlite_schema holds it
};

struct IndexInfo
{
    std::string name;
    std::string table;
    /// Column names; an expression part appears as its SQL text.
    std::vector<std::string> columns;
    bool unique = false;
    bool partial = false;
    std::string sql;  ///< empty for indexes made by UNIQUE constraints
};

/// Result column names, shared by a Result and its Rows.
struct ColumnNames
{
    std::vector<std::string> names;
    /// `alias.column` for a result column that is a plain column reference,
    /// else empty; `row["t.c"]` finds the column through it.
    std::vector<std::string> qualified;
};

// ------------------------------------------------------------- results ----

/// The values of one row. Up to four live inside the object, so a narrow
/// result costs no allocation per row; wider rows keep theirs on the heap.
class Values
{
public:
    Values() noexcept = default;
    Values(const Value* first, std::size_t count);
    explicit Values(std::vector<Value>&& values);
    Values(const Values& other) : Values(other.data_, other.size_) {}
    Values(Values&& other) noexcept { take(other); }
    Values& operator=(const Values& other);
    Values& operator=(Values&& other) noexcept;
    ~Values() { release(); }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    const Value* data() const noexcept { return data_; }
    const Value* begin() const noexcept { return data_; }
    const Value* end() const noexcept { return data_ + size_; }
    const Value& operator[](std::size_t i) const noexcept { return data_[i]; }
    std::vector<Value> toVector() const { return std::vector<Value>(begin(), end()); }

private:
    friend struct internal::ResultBuilder;
    friend class Result;
    /// Builds the values in place from `first`, moving them when `move`. A
    /// row wider than the inline space goes into `cells` when given (memory
    /// its Result owns) instead of a heap block of its own.
    void fill(Value* first, std::size_t count, bool move, Value* cells = nullptr);
    /// Copies size() values from `first` over the ones held, each cell
    /// keeping its buffer, so a recycled row allocates nothing.
    void refill(const Value* first);

    static constexpr std::size_t kInline = 4;
    bool inlined() const noexcept { return data_ == reinterpret_cast<const Value*>(inline_); }
    void take(Values& other) noexcept;
    void release() noexcept;

    Value* data_ = nullptr;
    std::size_t size_ = 0;
    bool borrowed_ = false;  ///< data_ is in a Result's cell chunk, not ours to free
    alignas(Value) unsigned char inline_[kInline * sizeof(Value)];
};

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

    const Values& values() const noexcept { return values_; }
    const Value* begin() const noexcept { return values_.begin(); }
    const Value* end() const noexcept { return values_.end(); }

private:
    friend class Result;
    friend class RowView;
    friend struct internal::ResultBuilder;
    Row(const std::shared_ptr<const ColumnNames>& names, Values values)
        : names_(names), values_(std::move(values))
    {}

    std::shared_ptr<const ColumnNames> names_;
    Values values_;
};

/// A row as it streams past Statement::each: valid only during the call. Its
/// text and blob cells may borrow bytes from the database file (see
/// Value::borrowed); a copied Value, or row(), owns its bytes.
class RowView
{
public:
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    const Value* begin() const noexcept { return values_; }
    const Value* end() const noexcept { return values_ + size_; }
    const Value& operator[](std::size_t i) const noexcept { return values_[i]; }
    const Value& at(std::size_t i) const;
    /// By result column name (or `alias.column`), as Row does.
    const Value& at(std::string_view column) const;
    const Value& operator[](std::string_view column) const { return at(column); }
    const std::vector<std::string>& columns() const;
    /// An owned copy.
    Row row() const;

private:
    friend struct internal::ResultBuilder;
    RowView(const Value* values, std::size_t size, const std::shared_ptr<const ColumnNames>* names) noexcept
        : values_(values), size_(size), names_(names)
    {}
    const Value* values_;
    std::size_t size_;
    const std::shared_ptr<const ColumnNames>* names_;
};

/// Called for each row by Statement::each; return false to stop early.
using RowCallback = std::function<bool(const RowView&)>;

/// A borrowed reference to whatever Statement::each calls per row: a lambda
/// is called where it is, never copied into a std::function, so no call
/// allocates. Valid only while the callable it refers to lives.
class RowVisitor
{
public:
    template <class F,
              class = std::enable_if_t<!std::is_same_v<std::remove_const_t<F>, RowVisitor>>>
    RowVisitor(F& fn) noexcept
        : fn_(const_cast<void*>(static_cast<const void*>(&fn))),
          call_([](void* f, const RowView& row) { return bool((*static_cast<F*>(f))(row)); })
    {}
    bool operator()(const RowView& row) const { return call_(fn_, row); }

private:
    void* fn_;
    bool (*call_)(void*, const RowView&);
};

/// What a statement produced: rows for SELECT, counters for everything else.
class Result
{
public:
    Result() = default;
    Result(Result&& other) noexcept;
    Result& operator=(Result&& other) noexcept;
    /// Copies own their values.
    Result(const Result& other);
    Result& operator=(const Result& other);
    ~Result();

    const std::vector<std::string>& columns() const;
    /// Per column, `alias.column` when it is a plain column reference, else
    /// empty: the names `row["t.c"]` also answers to.
    const std::vector<std::string>& qualifiedColumns() const;
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
    /// Room for `count` values in the result's cell chunks.
    Value* cells(std::size_t count);

    std::shared_ptr<const ColumnNames> names_;
    /// Raw memory for the values of rows wider than Values' inline space: a
    /// few chunks per result instead of one heap block per row. Declared
    /// before rows_, which destroy the values in them.
    std::vector<std::unique_ptr<unsigned char[]>> chunks_;
    std::size_t chunkSize_ = 0;  ///< values the last chunk holds
    std::size_t chunkFree_ = 0;  ///< of which still unused
    std::vector<Row> rows_;
    /// Rows of an earlier run kept by Statement::execInto, refilled in place
    /// (each cell keeps its buffer) by the next run of the same statement.
    std::vector<Row> spare_;
    std::uint64_t changes_ = 0;
    std::int64_t lastInsertId_ = 0;
};

/// Values bound to the `?` placeholders of a statement, left to right.
using Params = std::vector<Value>;
/// Values bound by name: {":id", Value(4)}. A name spelled with its prefix
/// (:, @, $) matches exactly; spelled without one ("id") it matches :id, @id
/// or $id.
using NamedParams = std::vector<std::pair<std::string, Value>>;

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
    Result exec(std::string_view text, const NamedParams& params);

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
    /// How many parameters it carries: the largest parameter number used.
    std::size_t parameters() const;
    /// 1-based index of the parameter spelled `name` (":id", "@id", "$id",
    /// "?3", or "id" for whichever prefix it carries); 0 when there is none.
    std::size_t parameterIndex(std::string_view name) const;
    /// The spelling of parameter `index` (1-based); empty for a plain `?`.
    std::string parameterName(std::size_t index) const;
    /// The names of the columns the statement returns (empty for statements
    /// that return none). Plans the statement if it has not run yet.
    std::vector<std::string> columnNames();
    /// True when running it needs a write transaction.
    bool writes() const;

    /// Bind one parameter. `index` is 1-based, as in SQLite. Parameters that
    /// are never bound are NULL.
    Statement& bind(std::size_t index, Value value);
    /// Bind a parameter by name, prefix optional (see NamedParams).
    Statement& bind(std::string_view name, Value value);
    /// Bind every parameter at once; the count must match `parameters()`.
    Statement& bind(const Params& params);
    Statement& bind(const NamedParams& params);
    /// Put every parameter back to NULL.
    Statement& clear();

    /// Run with the bindings currently in place, in a transaction of its own.
    Result exec();
    /// Run inside a transaction the caller controls.
    Result exec(Transaction& txn);
    /// Bind, then run.
    Result exec(const Params& params) { return bind(params).exec(); }
    Result exec(Transaction& txn, const Params& params) { return bind(params).exec(txn); }
    Result exec(const NamedParams& params) { return bind(params).exec(); }
    /// Bind in place, then run: each value is copied into the statement's
    /// own parameter cell, which keeps its buffer, so no Params vector is
    /// built and a warm call allocates nothing. A Value::textRef or
    /// Value::blobRef stays a reference: its bytes must outlive the run.
    Result exec(std::initializer_list<Value> params);
    Result exec(Transaction& txn, std::initializer_list<Value> params);

    /// Runs inside `txn` into `out`, reusing it: the rows `out` holds from
    /// its last run of this statement are refilled in place, each cell
    /// keeping its buffer, so a warm run allocates nothing. `out` keeps the
    /// rows of its largest run until it is destroyed. Returns `out`.
    Result& execInto(Transaction& txn, Result& out);

    /// Runs like exec(), but hands each row to `fn` as it is produced instead
    /// of collecting a Result: nothing is copied or kept, so a large result
    /// costs no memory. `fn` returning false stops the statement there.
    /// Returns the number of rows changed. A lambda is called through a
    /// RowVisitor, so the call itself allocates nothing.
    std::uint64_t each(const RowCallback& fn);
    std::uint64_t each(Transaction& txn, const RowCallback& fn);
    template <class F>
    std::uint64_t each(F&& fn)
    {
        return eachRow(nullptr, RowVisitor(fn));
    }
    template <class F>
    std::uint64_t each(Transaction& txn, F&& fn)
    {
        return eachRow(&txn, RowVisitor(fn));
    }

private:
    std::uint64_t eachRow(Transaction* txn, RowVisitor fn);
    Result run(Transaction& txn, const RowVisitor* each, Result* into = nullptr);
    friend class Database;
    explicit Statement(std::shared_ptr<internal::StatementImpl> impl) noexcept
        : impl_(std::move(impl))
    {}
    std::shared_ptr<internal::StatementImpl> impl_;
};

// ------------------------------------------------------------ database ----

/// How much memory a Database keeps between statements so that the next
/// statement need not allocate it again. Every figure can also be set on its
/// own (Database::Options); an explicit setting wins over the profile's.
///
/// | profile    | statement cache | scratch kept | sort memory | arena block |
/// |------------|-----------------|--------------|-------------|-------------|
/// | Compact    | 8               | 64 KiB       | 8 MiB       | 1 KiB       |
/// | Balanced   | 32              | 2 MiB        | 64 MiB      | 4 KiB       |
/// | Throughput | 128             | 16 MiB       | 256 MiB     | 16 KiB      |
enum class MemoryProfile : std::uint8_t
{
    Compact,     ///< many databases per process, small devices
    Balanced,    ///< the default
    Throughput,  ///< one busy database that may use memory freely
};

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
        /// On is nosql::Durability::Safe: every commit is flushed. Off is
        /// nosql::Durability::None: commits are flushed only by
        /// nosql::Env::sync(), on close, or when libnosql decides to; a crash
        /// loses the commits since then. The store reopens on a consistent
        /// snapshot after a process crash either way; after a power loss it
        /// does so only with a libnosql that keeps the newest meta page in
        /// memory until a sync (deferred meta). An older libnosql writes meta
        /// and data unordered under None, so do not use durable(false) on a
        /// store that must survive a power cut there.
        Options& durable(bool on = true)
        {
            durable_ = on;
            return *this;
        }
        /// Any libnosql durability mode, for example nosql::Durability::Lazy
        /// with syncInterval(). Wins over durable().
        Options& durability(nosql::Durability mode)
        {
            durability_ = mode;
            durabilitySet_ = true;
            return *this;
        }
        /// libnosql's sync interval for Durability::Lazy (one second by
        /// default): the longest a commit may wait to become durable.
        Options& syncInterval(std::chrono::milliseconds interval)
        {
            syncInterval_ = interval;
            return *this;
        }
        /// libnosql's page size for a store being created; an existing store
        /// keeps its own (pinning a different one is an error). 4096 by
        /// default.
        Options& pageSize(std::size_t bytes)
        {
            pageSize_ = bytes;
            return *this;
        }
        /// Size a new store file starts at, and the least it grows by each
        /// time (libnosql's initialSize and growthStep).
        Options& initialSize(std::uint64_t bytes)
        {
            initialSize_ = bytes;
            return *this;
        }
        Options& growthStep(std::uint64_t bytes)
        {
            growthStep_ = bytes;
            return *this;
        }
        /// Payload bytes of page buffers libnosql keeps for reuse between
        /// transactions (32 MiB by default); size it to the dirty set of a
        /// typical commit.
        Options& bufferCache(std::size_t bytes)
        {
            bufferCache_ = bytes;
            return *this;
        }
        /// The most dirty page bytes one write transaction may hold (256 MiB
        /// by default); a larger one fails with nosql::ErrorCode::OutOfMemory.
        Options& dirtyLimit(std::uint64_t bytes)
        {
            dirtyLimit_ = bytes;
            return *this;
        }
        /// Let reads trust a page checksum the process has already verified
        /// since that page was last written, instead of re-hashing every page
        /// on every access. Roughly halves the cost of a point lookup. The
        /// trade is that damage done to the file from outside -- a media or
        /// memory fault -- is noticed on the next verification rather than the
        /// next read; every remembered check expires after `revalidateAfter`
        /// (default one minute), and writes always verify. On by default;
        /// `cacheReadChecksums(false)` verifies every page on every read.
        Options& cacheReadChecksums(bool on = true,
                                    std::chrono::milliseconds revalidateAfter = std::chrono::minutes(1))
        {
            cacheReads_ = on;
            revalidateAfter_ = revalidateAfter;
            return *this;
        }
        /// How long a remembered page check is trusted (see
        /// cacheReadChecksums); zero trusts it for ever.
        Options& revalidateAfter(std::chrono::milliseconds interval)
        {
            revalidateAfter_ = interval;
            return *this;
        }

        /// How long a write transaction waits for another one to finish
        /// before failing with Busy ("database is locked"); the initial value
        /// of PRAGMA busy_timeout. Zero, the default, waits indefinitely.
        Options& busyTimeout(std::chrono::milliseconds wait)
        {
            busyTimeout_ = wait;
            return *this;
        }

        /// Memory an ORDER BY may use before it writes sorted runs to a
        /// temporary file and merges them (64 MiB by default). An ORDER BY
        /// with LIMIT keeps only the rows that can make the cut and does not
        /// need this. PRAGMA temp_store = MEMORY turns spilling off.
        Options& sortMemoryLimit(std::uint64_t bytes)
        {
            sortMemory_ = bytes;
            return *this;
        }

        /// Sets the memory figures below (and sortMemoryLimit) from a
        /// profile; any of them set explicitly keeps its explicit value.
        Options& memoryProfile(MemoryProfile profile)
        {
            profile_ = profile;
            return *this;
        }
        /// One-off statement texts kept planned, per Database and per
        /// transaction, so `exec` of the same text in a loop parses and plans
        /// once. 0 turns the cache off.
        Options& statementCacheSize(std::size_t statements)
        {
            statementCache_ = statements;
            return *this;
        }
        /// Bytes of per-run working memory (group tables, DISTINCT and IN key
        /// sets, sort buffers) a statement keeps for its next run. A run that
        /// needed more frees the excess when it ends.
        Options& scratchRetain(std::uint64_t bytes)
        {
            scratchRetain_ = bytes;
            return *this;
        }
        /// Block size of the arenas that hold each parsed statement's tree.
        Options& arenaBlockSize(std::size_t bytes)
        {
            arenaBlock_ = bytes;
            return *this;
        }

        Database open(const std::filesystem::path& path) const;

    private:
        static constexpr std::uint64_t kUnset = ~std::uint64_t(0);
        MemoryProfile profile_ = MemoryProfile::Balanced;
        std::uint64_t statementCache_ = kUnset;
        std::uint64_t scratchRetain_ = kUnset;
        std::uint64_t arenaBlock_ = kUnset;
        std::uint64_t sortMemory_ = kUnset;
        std::chrono::milliseconds busyTimeout_{0};
        std::uint64_t maxSize_ = 64ull << 30;
        bool readOnly_ = false;
        bool create_ = true;
        bool durable_ = true;
        bool cacheReads_ = true;
        std::chrono::milliseconds revalidateAfter_{60000};
        bool durabilitySet_ = false;
        nosql::Durability durability_{};
        std::chrono::milliseconds syncInterval_{-1};  ///< negative: libnosql's default
        std::uint64_t pageSize_ = kUnset;
        std::uint64_t initialSize_ = kUnset;
        std::uint64_t growthStep_ = kUnset;
        std::uint64_t bufferCache_ = kUnset;
        std::uint64_t dirtyLimit_ = kUnset;
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
    /// anything throws. A batch that contains BEGIN, COMMIT, ROLLBACK,
    /// SAVEPOINT or RELEASE runs SQLite's way instead: each statement outside
    /// an explicit transaction commits on its own, and a transaction opened
    /// with BEGIN stays open across calls until COMMIT or ROLLBACK.
    Result exec(std::string_view text, const Params& params = {});
    Result exec(std::string_view text, const NamedParams& params);

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

    /// Rows changed by the most recent INSERT, UPDATE or DELETE on this handle.
    std::int64_t changes() const;
    /// Rows changed since the handle was opened, triggers included.
    std::int64_t totalChanges() const;
    /// Rowid of the most recent successful INSERT into a rowid table.
    std::int64_t lastInsertRowid() const;
    /// True while a transaction opened with SQL BEGIN or SAVEPOINT is open.
    bool inTransaction() const;
    /// Asks the statement running on this handle (from another thread) to stop
    /// with ErrorCode::Interrupted at its next row.
    void interrupt();

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

/// True when `text` ends with a complete SQL statement: a `;` that is outside
/// quotes, comments and CREATE TRIGGER bodies (sqlite3_complete).
bool complete(std::string_view text);

}  // namespace sql
