// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The public surface: opening a store, running statements, and handing back
// owned results.

#include <algorithm>
#include <optional>

#include "internal/catalog.hpp"
#include "internal/executor.hpp"
#include "internal/lexer.hpp"
#include "internal/parser.hpp"

namespace sql {

namespace internal {

struct DatabaseImpl
{
    nosql::Env env;
    std::filesystem::path path;
    bool readOnly = false;
    /// The newest decoded schema, shared by every transaction that sees it.
    SchemaCache schema;
};

struct TxnImpl
{
    std::shared_ptr<DatabaseImpl> db;
    nosql::Txn txn;
    bool readOnly = false;
    /// The schema as this transaction sees it, plus its sub-database handles
    /// and rowid counters; built once when the transaction begins.
    std::optional<Catalog> catalog;
};

struct StatementImpl
{
    std::shared_ptr<DatabaseImpl> db;
    std::string text;
    std::vector<internal::Statement> parsed;  ///< the AST, not sql::Statement
    bool mutating = false;
    Params bound;
};

namespace {

/// Runs already-parsed statements and returns the last one's result.
Result runAll(Catalog& catalog, std::vector<Statement>& statements, const Params& params)
{
    Result last;
    for (Statement& s : statements)
        last = execute(catalog, s, params);
    return last;
}

/// Parses, rejects writes the caller may not make, and reports whether the
/// batch needs a write transaction. `parameters` receives the `?` count.
bool prepare(std::string_view text, bool readOnly, std::vector<Statement>& out,
             std::size_t* parameters = nullptr)
{
    out = parse(text, parameters);
    const bool mutating = std::any_of(out.begin(), out.end(), mutates);
    if (mutating && readOnly)
        throw Error(ErrorCode::ReadOnly, "database is open read-only");
    return mutating;
}

/// Runs already-parsed statements in a transaction of their own: a writer when
/// any of them mutates, a snapshot otherwise.
Result runOwn(DatabaseImpl& db, bool mutating, std::vector<Statement>& statements,
              const Params& params)
{
    nosql::Txn txn = mutating ? db.env.writeTxn() : db.env.readTxn();
    Catalog catalog(txn, db.schema);
    Result result = runAll(catalog, statements, params);
    if (mutating) {
        const std::uint64_t id = txn.id();
        txn.commit();
        catalog.committed(id);
    } else {
        txn.abort();
    }
    return result;
}

DatabaseImpl& open(const std::shared_ptr<DatabaseImpl>& impl)
{
    if (!impl)
        throw Error(ErrorCode::InvalidArgument, "the database is not open");
    return *impl;
}

TxnImpl& live(const std::shared_ptr<TxnImpl>& impl)
{
    if (!impl)
        throw Error(ErrorCode::BadTransaction, "the transaction has already finished");
    return *impl;
}

std::shared_ptr<TxnImpl> beginTxn(std::shared_ptr<DatabaseImpl> db, bool readOnly)
{
    auto txn = std::make_shared<TxnImpl>();
    txn->readOnly = readOnly;
    txn->txn = readOnly ? db->env.readTxn() : db->env.writeTxn();
    txn->catalog.emplace(txn->txn, db->schema);
    txn->db = std::move(db);
    return txn;
}

}  // namespace
}  // namespace internal

// ------------------------------------------------------------ accessors ---

const Value& Row::at(std::size_t i) const
{
    if (i >= values_.size())
        throw Error(ErrorCode::InvalidArgument,
                    "column " + std::to_string(i) + " is out of range");
    return values_[i];
}

bool Row::has(std::string_view column) const noexcept
{
    if (!names_)
        return false;
    return std::any_of(names_->begin(), names_->end(), [&](const std::string& n) {
        return internal::equalsNoCase(n, column);
    });
}

const Value& Row::at(std::string_view column) const
{
    if (names_) {
        for (std::size_t i = 0; i < names_->size() && i < values_.size(); ++i) {
            if (internal::equalsNoCase((*names_)[i], column))
                return values_[i];
        }
    }
    throw Error(ErrorCode::NoSuchColumn, "no such result column: " + std::string(column));
}

const std::vector<std::string>& Result::columns() const
{
    static const std::vector<std::string> kNone;
    return names_ ? *names_ : kNone;
}

const Row& Result::at(std::size_t i) const
{
    if (i >= rows_.size())
        throw Error(ErrorCode::InvalidArgument, "row " + std::to_string(i) + " is out of range");
    return rows_[i];
}

// ---------------------------------------------------------- transaction ---

Transaction::Transaction(Transaction&&) noexcept = default;
Transaction& Transaction::operator=(Transaction&&) noexcept = default;

Transaction::~Transaction()
{
    // Letting the nosql transaction go without a commit aborts it.
    impl_.reset();
}

Result Transaction::exec(std::string_view text, const Params& params)
{
    internal::TxnImpl& t = internal::live(impl_);
    std::vector<internal::Statement> statements;
    internal::prepare(text, t.readOnly, statements);
    return internal::runAll(*t.catalog, statements, params);
}

void Transaction::commit()
{
    internal::TxnImpl& t = internal::live(impl_);
    if (!t.readOnly) {
        const std::uint64_t id = t.txn.id();
        t.txn.commit();
        t.catalog->committed(id);
    }
    impl_.reset();
}

void Transaction::rollback()
{
    impl_.reset();
}

nosql::Txn& Transaction::txn()
{
    return internal::live(impl_).txn;
}

// ----------------------------------------------------- prepared statement --

namespace {

internal::StatementImpl& live(const std::shared_ptr<internal::StatementImpl>& impl)
{
    if (!impl)
        throw Error(ErrorCode::InvalidArgument, "the statement is not prepared");
    return *impl;
}

}  // namespace

Statement::Statement(Statement&&) noexcept = default;
Statement& Statement::operator=(Statement&&) noexcept = default;
Statement::~Statement() = default;

const std::string& Statement::sql() const
{
    return live(impl_).text;
}

std::size_t Statement::parameters() const
{
    return live(impl_).bound.size();
}

bool Statement::writes() const
{
    return live(impl_).mutating;
}

Statement& Statement::bind(std::size_t index, Value value)
{
    internal::StatementImpl& s = live(impl_);
    if (index < 1 || index > s.bound.size())
        throw Error(ErrorCode::InvalidArgument,
                    "no parameter " + std::to_string(index) + "; the statement has " +
                        std::to_string(s.bound.size()));
    s.bound[index - 1] = std::move(value);
    return *this;
}

Statement& Statement::bind(const Params& params)
{
    internal::StatementImpl& s = live(impl_);
    if (params.size() != s.bound.size())
        throw Error(ErrorCode::InvalidArgument,
                    "statement takes " + std::to_string(s.bound.size()) + " parameters, " +
                        std::to_string(params.size()) + " were given");
    s.bound = params;
    return *this;
}

Statement& Statement::clear()
{
    internal::StatementImpl& s = live(impl_);
    s.bound.assign(s.bound.size(), Value());
    return *this;
}

Result Statement::exec()
{
    internal::StatementImpl& s = live(impl_);
    return internal::runOwn(*s.db, s.mutating, s.parsed, s.bound);
}

Result Statement::exec(Transaction& txn)
{
    internal::StatementImpl& s = live(impl_);
    internal::TxnImpl& t = internal::live(txn.impl_);
    if (s.mutating && t.readOnly)
        throw Error(ErrorCode::ReadOnly, "the transaction is read-only");
    return internal::runAll(*t.catalog, s.parsed, s.bound);
}

// ------------------------------------------------------------- database ---

Database Database::Options::open(const std::filesystem::path& path) const
{
    auto impl = std::make_shared<internal::DatabaseImpl>();
    impl->env = nosql::Env::configure()
                    .maxSize(maxSize_)
                    .maxDbs(512)
                    .readOnly(readOnly_)
                    .createIfMissing(create_)
                    .sync(durable_ ? nosql::Durability::Safe : nosql::Durability::None)
                    .cacheReadChecksums(cacheReads_)
                    .revalidateAfter(revalidateAfter_)
                    .open(path);
    impl->path = path;
    impl->readOnly = readOnly_;

    Database db;
    db.impl_ = std::move(impl);
    return db;
}

Database::~Database() = default;

const std::filesystem::path& Database::path() const
{
    return internal::open(impl_).path;
}

Result Database::exec(std::string_view text, const Params& params)
{
    internal::DatabaseImpl& db = internal::open(impl_);
    std::vector<internal::Statement> statements;
    const bool write = internal::prepare(text, db.readOnly, statements);
    return internal::runOwn(db, write, statements, params);
}

Statement Database::prepare(std::string_view text)
{
    internal::DatabaseImpl& db = internal::open(impl_);
    auto stmt = std::make_shared<internal::StatementImpl>();
    stmt->db = impl_;
    stmt->text = std::string(text);
    std::size_t count = 0;
    stmt->mutating = internal::prepare(text, db.readOnly, stmt->parsed, &count);
    stmt->bound.assign(count, Value());
    return Statement(std::move(stmt));
}

Transaction Database::begin()
{
    return Transaction(internal::beginTxn(impl_, internal::open(impl_).readOnly));
}

Transaction Database::beginRead() const
{
    internal::open(impl_);
    return Transaction(internal::beginTxn(impl_, true));
}

Database Database::openReader() const
{
    internal::DatabaseImpl& source = internal::open(impl_);
    auto impl = std::make_shared<internal::DatabaseImpl>();
    impl->env = source.env.share();
    impl->path = source.path;
    impl->readOnly = true;

    Database db;
    db.impl_ = std::move(impl);
    return db;
}

std::vector<std::string> Database::tables() const
{
    internal::DatabaseImpl& db = internal::open(impl_);
    return db.env.read([&](nosql::Txn& t) { return internal::Catalog(t, db.schema).tableNames(); });
}

TableInfo Database::table(std::string_view name) const
{
    const std::string wanted(name);
    internal::DatabaseImpl& db = internal::open(impl_);
    return db.env.read([&](nosql::Txn& t) {
        internal::Catalog catalog(t, db.schema);
        const internal::Table& found = catalog.get(wanted);
        TableInfo info;
        info.name = found.name;
        info.columns = found.columns;
        info.primaryKey = found.primaryKey;
        return info;
    });
}

std::vector<IndexInfo> Database::indexes(std::string_view table) const
{
    const std::string wanted(table);
    internal::DatabaseImpl& db = internal::open(impl_);
    return db.env.read([&](nosql::Txn& t) {
        internal::Catalog catalog(t, db.schema);
        std::vector<IndexInfo> out;
        for (const internal::Index* ix : catalog.allIndexes()) {
            if (!wanted.empty() && !internal::equalsNoCase(ix->table, wanted))
                continue;
            IndexInfo info;
            info.name = ix->name;
            info.table = ix->table;
            info.unique = ix->unique;
            const internal::Table& owner = catalog.get(ix->table);
            for (const int c : ix->columns)
                info.columns.push_back(owner.columns[std::size_t(c)].name);
            out.push_back(std::move(info));
        }
        return out;
    });
}

nosql::Env& Database::env() const
{
    return internal::open(impl_).env;
}

void Database::close()
{
    impl_.reset();
}

const char* version() noexcept
{
    return "0.1.0";
}

}  // namespace sql
