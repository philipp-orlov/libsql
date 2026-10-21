// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The public surface: opening stores, connection state, transactions (the
// C++ API's and SQL's BEGIN/COMMIT/SAVEPOINT), prepared statements, and owned
// results.

#include <new>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <random>

#include "internal/catalog.hpp"
#include "internal/lexer.hpp"
#include "internal/parser.hpp"
#include "internal/raise.hpp"
#include "internal/stmt.hpp"
#include "internal/util.hpp"

#ifdef _WIN32
#include <process.h>
#define LIBSQL_GETPID _getpid
#else
#include <unistd.h>
#define LIBSQL_GETPID getpid
#endif

namespace sql {

namespace internal {

// ---------------------------------------------------------------- stores --

struct StoreHandle
{
    std::string name;
    std::filesystem::path path;
    std::string displayPath;  ///< what PRAGMA database_list shows
    nosql::Env env;
    SchemaCache schema;
    bool readOnly = false;
    bool scratch = false;  ///< temp/:memory: store: deleted on close

    ~StoreHandle()
    {
        if (!scratch)
            return;
        env.close();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

struct OpenOptions
{
    std::uint64_t maxSize = 64ull << 30;
    nosql::Durability durability = nosql::Durability::Safe;
    bool cacheReads = true;
    std::chrono::milliseconds revalidate{60000};
    std::size_t statementCache = 32;
    std::size_t arenaBlock = 4096;
    /// libnosql settings passed through when set (kUnset or negative: its own
    /// default).
    static constexpr std::uint64_t kUnset = ~std::uint64_t(0);
    std::chrono::milliseconds syncInterval{-1};
    std::uint64_t pageSize = kUnset;
    std::uint64_t initialSize = kUnset;
    std::uint64_t growthStep = kUnset;
    std::uint64_t bufferCache = kUnset;
    std::uint64_t dirtyLimit = kUnset;
};

std::filesystem::path scratchPath()
{
    static std::atomic<std::uint64_t> counter{0};
    static const std::uint64_t salt = std::random_device{}();
    const std::string name = "libsql-" + std::to_string(LIBSQL_GETPID()) + "-" + std::to_string(salt % 1000000) +
                             "-" + std::to_string(counter.fetch_add(1)) + ".db";
    return std::filesystem::temp_directory_path() / name;
}

std::unique_ptr<StoreHandle> openScratch(const std::string& name, const OpenOptions& o)
{
    auto h = std::make_unique<StoreHandle>();
    h->name = name;
    h->path = scratchPath();
    h->scratch = true;
    h->env = nosql::Env::configure()
                 .maxSize(o.maxSize)
                 .maxDbs(512)
                 .createIfMissing(true)
                 .errorIfExists(true)
                 .preallocate(false)
                 .sync(nosql::Durability::None)
                 .open(h->path);
    return h;
}

std::unique_ptr<StoreHandle> openFile(const std::string& name, const std::filesystem::path& path,
                                      const OpenOptions& o, bool readOnly, bool create)
{
    const std::string p = path.string();
    if (p.empty() || p == ":memory:") {
        auto h = openScratch(name, o);
        h->displayPath = "";
        return h;
    }
    auto h = std::make_unique<StoreHandle>();
    h->name = name;
    h->path = path;
    h->displayPath = std::filesystem::absolute(path).string();
    h->readOnly = readOnly;
    nosql::Env::Options eo = nosql::Env::configure()
                                 .maxSize(o.maxSize)
                                 .maxDbs(512)
                                 .readOnly(readOnly)
                                 .createIfMissing(create)
                                 .sync(o.durability)
                                 .cacheReadChecksums(o.cacheReads)
                                 .revalidateAfter(o.revalidate);
    if (o.syncInterval.count() >= 0)
        eo.syncInterval(o.syncInterval);
    if (o.pageSize != OpenOptions::kUnset)
        eo.pageSize(std::size_t(o.pageSize));
    if (o.initialSize != OpenOptions::kUnset)
        eo.initialSize(o.initialSize);
    if (o.growthStep != OpenOptions::kUnset)
        eo.growthStep(o.growthStep);
    if (o.bufferCache != OpenOptions::kUnset)
        eo.bufferCache(std::size_t(o.bufferCache));
    if (o.dirtyLimit != OpenOptions::kUnset)
        eo.dirtyLimit(o.dirtyLimit);
    h->env = eo.open(path);
    return h;
}

// --------------------------------------------------------- batches -------

struct StatementMeta
{
    bool mutating = false;
    bool control = false;
    bool connection = false;  ///< ATTACH, DETACH, VACUUM
    bool writesPragma = false;
};

/// A parsed batch and its compiled programs, planned lazily statement by
/// statement and re-planned when the schema they were planned against moves.
struct Batch
{
    std::string text;
    std::vector<std::string> parameterNames;
    std::vector<StatementMeta> meta;
    std::vector<std::unique_ptr<StatementNode>> fresh;  ///< unplanned trees, consumed by the first plan
    std::vector<std::unique_ptr<Program>> programs;
    bool anyControl = false;
    bool anyConnection = false;
    bool needsWrite = false;
    std::size_t parameters = 0;

    std::size_t arenaBlock = 4096;  ///< for the statements' parse arenas

    /// Parses with this batch's arena block size, restoring the thread's.
    template <class F>
    auto parsing(F&& f)
    {
        struct Restore
        {
            std::size_t saved = arenaBlockSize();
            ~Restore() { setArenaBlockSize(saved); }
        } restore;
        setArenaBlockSize(arenaBlock);
        return f();
    }

    Batch(std::string_view sql, std::size_t block) : text(sql), arenaBlock(block)
    {
        ParsedBatch parsed = parsing([&] { return parse(text); });
        parameterNames = std::move(parsed.parameterNames);
        parameters = parameterNames.size();
        for (StatementNode& node : parsed.statements) {
            StatementMeta m;
            m.mutating = mutates(node);
            m.control = controlsTransaction(node);
            m.connection = std::holds_alternative<AttachStmt>(node.body) ||
                           std::holds_alternative<DetachStmt>(node.body) ||
                           std::holds_alternative<VacuumStmt>(node.body);
            if (const auto* pr = std::get_if<PragmaStmt>(&node.body)) {
                m.writesPragma = pr->value && (equalsNoCase(pr->name, "user_version") ||
                                               equalsNoCase(pr->name, "application_id"));
            }
            anyControl = anyControl || m.control;
            anyConnection = anyConnection || m.connection;
            needsWrite = needsWrite || m.mutating || m.writesPragma;
            meta.push_back(m);
            fresh.push_back(std::make_unique<StatementNode>(std::move(node)));
        }
        programs.resize(meta.size());
    }

    std::unique_ptr<StatementNode> freshNode(std::size_t k)
    {
        if (fresh[k])
            return std::move(fresh[k]);
        ParsedBatch again = parsing([&] { return parse(text); });
        return std::make_unique<StatementNode>(std::move(again.statements[k]));
    }
};

// ----------------------------------------------------------- connection --

/// A small most-recently-used cache of parsed and planned texts.
/// Recently run one-off texts, most recent first, so `exec` of the same text
/// in a loop parses and plans once. Entries are shared: a batch that is
/// running stays alive when something it runs evicts it or clears the cache
/// (DETACH).
struct BatchCache
{
    using Entry = std::shared_ptr<Batch>;
    std::list<Entry> entries;
    std::unordered_map<std::string_view, std::list<Entry>::iterator> index;
    std::size_t capacity = 32;
    std::size_t arenaBlock = 4096;

    Entry get(std::string_view text)
    {
        if (const auto it = index.find(text); it != index.end()) {
            if (it->second != entries.begin())
                entries.splice(entries.begin(), entries, it->second);
            return entries.front();
        }
        auto fresh = std::make_shared<Batch>(text, arenaBlock);
        if (capacity == 0)
            return fresh;
        entries.push_front(fresh);
        index.emplace(std::string_view(fresh->text), entries.begin());
        while (entries.size() > capacity) {
            index.erase(std::string_view(entries.back()->text));
            entries.pop_back();
        }
        return fresh;
    }
    void clear()
    {
        index.clear();
        entries.clear();
    }
    void configure(const OpenOptions& o)
    {
        capacity = o.statementCache;
        arenaBlock = o.arenaBlock;
    }
};

struct TxnImpl;

struct DatabaseImpl
{
    std::vector<std::unique_ptr<StoreHandle>> stores;  ///< 0 main, 1 temp (may be null), 2.. attached
    bool readOnly = false;
    OpenOptions options;
    ConnectionState conn;
    FuncContext fctx;
    std::shared_ptr<TxnImpl> sqlTxn;  ///< opened by BEGIN or SAVEPOINT
    std::atomic<bool> interrupted{false};
    /// Recently run one-off texts, so `exec` in a loop plans once. Used by
    /// Database::exec only; transactions have their own.
    BatchCache cache;
    std::shared_ptr<Batch> cached(std::string_view text) { return cache.get(text); }
    /// Finished transactions waiting to be reused.
    std::mutex poolMutex;
    std::vector<std::shared_ptr<TxnImpl>> pool;
    static constexpr std::size_t kPoolSize = 4;
};

/// A nesting level inside a transaction: a savepoint, or one statement.
struct Level
{
    std::string name;
    bool savepoint = false;
    std::vector<nosql::Txn> txns;  ///< per store; invalid where not nested
    std::int64_t deferredFk = 0;
};

/// One transaction. These are pooled per Database and reused, so that a
/// statement in its own transaction allocates nothing for the transaction's
/// bookkeeping once the pool is warm. Each carries its own function scratch
/// and text cache, because transactions of one Database may run on
/// different threads.
struct TxnImpl
{
    std::shared_ptr<DatabaseImpl> db;
    bool readOnly = false;  ///< may never write (beginRead, reader handles)
    bool sql = false;       ///< opened by SQL BEGIN/SAVEPOINT
    bool bySavepoint = false;
    bool started = false;   ///< store transactions open
    bool writer = false;    ///< the main store's transaction writes
    bool doomed = false;    ///< a ROLLBACK conflict or a broken write ended it
    std::vector<nosql::Txn> roots;
    std::vector<std::unique_ptr<Catalog>> catalogPool;  ///< kept across uses
    std::vector<Catalog*> catalogs;                     ///< open ones, by store
    std::vector<std::unique_ptr<Level>> levels;
    std::vector<std::unique_ptr<Level>> levelPool;  ///< ended levels, for reuse
    Session session;
    FuncContext fctx;
    BatchCache cache;

    /// Ends whatever is open, children first: a parent cannot end while a
    /// child is open.
    void release()
    {
        while (!levels.empty()) {
            for (auto it = levels.back()->txns.rbegin(); it != levels.back()->txns.rend(); ++it)
                it->abort();
            levels.pop_back();
        }
        for (Catalog*& c : catalogs) {
            if (c)
                c->close();
            c = nullptr;
        }
        for (nosql::Txn& t : roots)
            t.abort();
        started = false;
        writer = false;
        session.beginAtomic = nullptr;
    }

    ~TxnImpl() { release(); }
};

int Session::storeIndex(std::string_view name) const
{
    for (std::size_t i = 0; i < stores.size(); ++i) {
        if (equalsNoCase(stores[i].name, name))
            return int(i);
    }
    if (equalsNoCase(name, "temp"))
        return 1;
    return -1;
}

namespace {

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

void refreshSession(TxnImpl& t)
{
    DatabaseImpl& db = *t.db;
    Session& s = t.session;
    s.conn = &db.conn;
    s.writable = t.writer;
    s.interrupt = &db.interrupted;
    s.explicitTxn = t.sql || !t.levels.empty();
    s.stores.resize(db.stores.size());
    for (std::size_t i = 0; i < db.stores.size(); ++i) {
        SessionStore& ss = s.stores[i];
        ss.name = i == 0 ? std::string_view("main")
                  : i == 1 ? std::string_view("temp")
                           : (db.stores[i] ? std::string_view(db.stores[i]->name) : std::string_view());
        ss.path = db.stores[i] ? std::string_view(db.stores[i]->displayPath) : std::string_view();
        ss.env = db.stores[i] ? &db.stores[i]->env : nullptr;
        ss.txn = i < t.roots.size() && t.roots[i].valid() ? &t.roots[i] : nullptr;
        ss.catalog = i < t.catalogs.size() ? t.catalogs[i] : nullptr;
    }
}

/// The writer slot of store `h`, waited for as long as busy_timeout allows.
nosql::Txn writerOf(DatabaseImpl& db, StoreHandle& h)
{
    const std::int64_t ms = db.conn.busyTimeout.load(std::memory_order_relaxed);
    try {
        return ms > 0 ? h.env.writeTxn(std::chrono::milliseconds(ms)) : h.env.writeTxn();
    } catch (const nosql::Error& e) {
        if (e.code() == nosql::ErrorCode::Busy)
            throw Error(ErrorCode::Busy, "database is locked");
        throw;
    }
}

void openStore(TxnImpl& t, std::size_t i)
{
    DatabaseImpl& db = *t.db;
    if (t.roots.size() < db.stores.size())
        t.roots.resize(db.stores.size());
    if (t.catalogs.size() < db.stores.size())
        t.catalogs.resize(db.stores.size(), nullptr);
    if (t.catalogPool.size() < db.stores.size())
        t.catalogPool.resize(db.stores.size());
    StoreHandle& h = *db.stores[i];
    const bool write = i == 1 ? true : (t.writer && !h.readOnly);
    t.roots[i] = write ? writerOf(db, h) : h.env.readTxn();
    if (!t.catalogPool[i])
        t.catalogPool[i] = std::make_unique<Catalog>();
    t.catalogPool[i]->open(t.roots[i], h.schema);
    t.catalogs[i] = t.catalogPool[i].get();
}

void start(TxnImpl& t, bool write)
{
    DatabaseImpl& db = *t.db;
    t.writer = write;
    t.roots.resize(db.stores.size());
    t.catalogs.assign(db.stores.size(), nullptr);
    for (std::size_t i = 0; i < db.stores.size(); ++i) {
        if (db.stores[i])
            openStore(t, i);
    }
    t.started = true;
    refreshSession(t);
    TxnImpl* self = &t;
    t.session.openTemp = [self](Session&) {
        DatabaseImpl& d = *self->db;
        if (!d.stores[1])
            d.stores[1] = openScratch("temp", d.options);
        openStore(*self, 1);
        refreshSession(*self);
    };
}

/// Opens nested store transactions for a new level.
void pushLevel(TxnImpl& t, std::string name, bool savepoint)
{
    std::unique_ptr<Level> level;
    if (!t.levelPool.empty()) {
        level = std::move(t.levelPool.back());
        t.levelPool.pop_back();
    } else {
        level = std::make_unique<Level>();
    }
    level->name = std::move(name);
    level->savepoint = savepoint;
    level->deferredFk = t.session.deferredFkViolations;
    level->txns.resize(t.roots.size());
    if (t.started) {
        for (std::size_t i = 0; i < t.roots.size(); ++i) {
            if (!t.catalogs[i] || t.roots[i].isReadOnly())
                continue;
            // Rowid marks so far belong to the parent: an abort of this
            // level must not lose them (Catalog::flush).
            t.catalogs[i]->flush();
            level->txns[i] = t.catalogs[i]->txn().nested();
            t.catalogs[i]->pushLevel(level->txns[i]);
        }
    }
    t.levels.push_back(std::move(level));
    refreshSession(t);
}

void popLevel(TxnImpl& t, bool commit)
{
    std::unique_ptr<Level> level = std::move(t.levels.back());
    t.levels.pop_back();
    for (std::size_t i = level->txns.size(); i-- > 0;) {
        nosql::Txn& child = level->txns[i];
        if (!child.valid())
            continue;
        if (commit)
            child.commit();
        else
            child.abort();
        if (i < t.catalogs.size() && t.catalogs[i])
            t.catalogs[i]->popLevel(commit);
    }
    if (!commit)
        t.session.deferredFkViolations = level->deferredFk;
    refreshSession(t);
    t.levelPool.push_back(std::move(level));
}

/// Turns a reading transaction into a writing one (SQL BEGIN DEFERRED), if no
/// other writer has committed since its snapshot; otherwise Busy.
void upgrade(TxnImpl& t)
{
    DatabaseImpl& db = *t.db;
    if (t.readOnly || db.readOnly)
        throw Error(ErrorCode::ReadOnly, "attempt to write a readonly database");
    std::vector<std::unique_ptr<Level>> markers = std::move(t.levels);
    t.levels.clear();
    if (!t.started) {
        start(t, true);
    } else if (!t.writer) {
        const std::uint64_t snapshot = t.roots[0].id();
        nosql::Txn w = writerOf(db, *db.stores[0]);
        if (w.id() - 1 != snapshot) {
            w.abort();
            t.levels = std::move(markers);
            throw Error(ErrorCode::Busy, "database is locked");
        }
        t.catalogs[0]->close();
        t.roots[0].abort();
        t.roots[0] = std::move(w);
        t.catalogs[0]->open(t.roots[0], db.stores[0]->schema);
        t.writer = true;
        for (std::size_t i = 2; i < db.stores.size(); ++i) {
            if (!db.stores[i] || db.stores[i]->readOnly || !t.catalogs[i])
                continue;
            t.catalogs[i]->close();
            t.roots[i].abort();
            openStore(t, i);
        }
        refreshSession(t);
    }
    // Savepoints made while reading become real nested transactions now.
    for (auto& m : markers)
        pushLevel(t, m->name, m->savepoint);
}

void commit(TxnImpl& t)
{
    if (t.session.deferredFkViolations > 0)
        throw Error(ErrorCode::ConstraintViolation, Constraint::ForeignKey, "FOREIGN KEY constraint failed");
    while (!t.levels.empty())
        popLevel(t, true);
    if (!t.started)
        return;
    for (std::size_t i = 0; i < t.roots.size(); ++i) {
        nosql::Txn& txn = t.roots[i];
        if (!txn.valid())
            continue;
        if (txn.isReadOnly()) {
            txn.abort();
            continue;
        }
        if (t.catalogs[i])
            t.catalogs[i]->flush();
        const std::uint64_t id = txn.id();
        txn.commit();
        if (t.catalogs[i])
            t.catalogs[i]->committed(id);
    }
    t.release();
}

void ensureStarted(TxnImpl& t, bool write)
{
    if (!t.started && !(write && !t.readOnly && !t.levels.empty()))
        start(t, write && !t.readOnly);
    if (write && !t.writer)
        upgrade(t);
}

// ---------------------------------------------------------------- run ----

void ensurePlanned(Batch& b, std::size_t k, Session& s)
{
    if (b.programs[k] && planCurrent(*b.programs[k], s))
        return;
    auto fresh = std::make_unique<Program>();
    fresh->text = b.text;
    fresh->node = b.freshNode(k);
    planStatement(*fresh, s, nullptr);
    b.programs[k] = std::move(fresh);
}

void checkWritable(DatabaseImpl& db, const StatementMeta& m, const TxnImpl* t)
{
    if (!(m.mutating || m.writesPragma))
        return;
    if (db.readOnly || (t && t->readOnly) || db.conn.queryOnly)
        throw Error(ErrorCode::ReadOnly, "attempt to write a readonly database");
}

/// Runs statement k of `b` in `t`. With `atomic`, a writing statement runs in
/// a nested level of its own so that a failure undoes it alone; DML opens
/// that level only when it has to (see Session::beginAtomic).
Result runOne(Batch& b, std::size_t k, TxnImpl& t, const std::vector<Value>& params, bool atomic,
              const RowVisitor* each = nullptr, Result* into = nullptr)
{
    DatabaseImpl& db = *t.db;
    const StatementMeta& m = b.meta[k];
    checkWritable(db, m, &t);
    ensureStarted(t, m.mutating || m.writesPragma);
    refreshSession(t);
    ensurePlanned(b, k, t.session);
    Program& p = *b.programs[k];
    db.interrupted.store(false, std::memory_order_relaxed);
    Result fresh;
    ResultBuilder out(into ? *into : fresh);
    out.each = each;
    const bool guarded = atomic && (m.mutating || m.writesPragma) && t.writer;
    bool nested = false;
    if (guarded) {
        t.session.beginAtomic = [&t, &nested] {
            if (!nested) {
                pushLevel(t, std::string(), false);
                nested = true;
            }
        };
        const StatementBody& body = p.node->body;
        const bool dml = std::holds_alternative<InsertStmt>(body) || std::holds_alternative<UpdateStmt>(body) ||
                         std::holds_alternative<DeleteStmt>(body);
        if (!dml)
            t.session.ensureAtomic();
    }
    // A row callback may not run the statement it is called from: the plan
    // and its scratch are in use.
    if (p.active)
        throw Error(ErrorCode::InvalidArgument, "statement is already running");
    struct Reset
    {
        Session& s;
        Program& p;
        ~Reset()
        {
            s.beginAtomic = nullptr;
            p.active = false;
        }
    } reset{t.session, p};
    p.active = true;
    try {
        runProgram(p, t.session, params, out, t.fctx);
    } catch (const RollbackError&) {
        if (nested)
            popLevel(t, false);
        t.doomed = true;
        throw;
    } catch (const FailError&) {
        if (nested)
            popLevel(t, true);
        throw;
    } catch (const Error&) {
        if (nested)
            popLevel(t, false);
        throw;
    } catch (...) {
        // A storage failure: undone if the statement had its own level.
        // Without one (a single-row write that skipped it) the store may hold
        // part of the row, so the transaction must not commit.
        if (nested)
            popLevel(t, false);
        else if (guarded)
            t.doomed = true;
        throw;
    }
    if (nested)
        popLevel(t, true);
    return fresh;
}

std::shared_ptr<TxnImpl> newTxn(const std::shared_ptr<DatabaseImpl>& db, bool readOnly)
{
    std::shared_ptr<TxnImpl> t;
    {
        std::lock_guard<std::mutex> lk(db->poolMutex);
        if (!db->pool.empty()) {
            t = std::move(db->pool.back());
            db->pool.pop_back();
        }
    }
    if (!t) {
        t = std::make_shared<TxnImpl>();
        t->cache.configure(db->options);
    }
    t->db = db;
    t->readOnly = readOnly;
    t->sql = false;
    t->bySavepoint = false;
    t->doomed = false;
    t->session.deferredFkViolations = 0;
    t->session.immediateFkViolations = 0;
    return t;
}

/// Makes `t` the SQL transaction of `db`. Its link back does not own `db`, so closing the
/// handle with BEGIN open frees both and rolls the transaction back.
void adoptSqlTxn(DatabaseImpl& db, std::shared_ptr<TxnImpl>&& t)
{
    t->db = std::shared_ptr<DatabaseImpl>(std::shared_ptr<DatabaseImpl>(), &db);
    db.sqlTxn = std::move(t);
}

/// Ends `t` (aborting whatever it still holds) and returns it to the pool.
void park(std::shared_ptr<TxnImpl>&& owner)
{
    std::shared_ptr<TxnImpl> t = std::move(owner);  // the caller's handle is gone either way
    if (!t)
        return;
    t->release();
    std::shared_ptr<DatabaseImpl> db = std::move(t->db);  // no cycle while pooled
    if (!db || t.use_count() != 1)
        return;
    std::lock_guard<std::mutex> lk(db->poolMutex);
    if (db->pool.size() < DatabaseImpl::kPoolSize)
        db->pool.push_back(std::move(t));
}

/// Owns a transaction for the length of a call and parks it at the end.
struct Parked
{
    std::shared_ptr<TxnImpl> t;
    ~Parked() { park(std::move(t)); }
};

/// SAVEPOINT / RELEASE / ROLLBACK TO inside a transaction.
void savepointControl(TxnImpl& t, const TransactionStmt& st, bool& finished)
{
    finished = false;
    switch (st.kind) {
        case TransactionStmt::Kind::Savepoint: pushLevel(t, st.name, true); return;
        case TransactionStmt::Kind::Release:
        case TransactionStmt::Kind::RollbackTo: {
            std::size_t at = t.levels.size();
            bool found = false;
            while (at-- > 0) {
                if (t.levels[at]->savepoint && equalsNoCase(t.levels[at]->name, st.name)) {
                    found = true;
                    break;
                }
            }
            if (!found)
                throw Error(ErrorCode::Runtime, "no such savepoint: " + st.name);
            if (st.kind == TransactionStmt::Kind::Release) {
                if (at == 0 && t.bySavepoint) {
                    commit(t);
                    finished = true;
                    return;
                }
                while (t.levels.size() > at)
                    popLevel(t, true);
                return;
            }
            const std::string name = t.levels[at]->name;
            while (t.levels.size() > at)
                popLevel(t, false);
            pushLevel(t, name, true);
            return;
        }
        default: break;
    }
}

Value evalConstant(DatabaseImpl& db, Expr& e, const std::vector<Value>& params)
{
    if (e.kind == ExprKind::Column && e.table.empty())
        return Value(e.name);
    Program p;
    Session s;
    s.conn = &db.conn;
    Planner pl(p, s);
    Scope scope;
    ExprPtr holder(&e);
    try {
        pl.resolve(holder, scope, kNoSubquery);
    } catch (...) {
        (void)holder.release();
        throw;
    }
    Expr* resolved = holder.release();
    p.frame.resize(p.frameSize);
    Ctx c;
    c.slots = p.frame.data();
    c.params = &params;
    c.session = &s;
    c.fctx = &db.fctx;
    return eval(*resolved, c);
}

void copyStore(nosql::Env& from, const std::filesystem::path& to)
{
    if (std::filesystem::exists(to))
        throw Error(ErrorCode::Runtime, "output file already exists");
    nosql::Env out = nosql::Env::configure().maxDbs(512).createIfMissing(true).open(to);
    nosql::Txn src = from.readTxn();
    nosql::Txn dst = out.writeTxn();
    auto copy = [](const nosql::Db& a, const nosql::Db& b) {
        nosql::Cursor cur = a.cursor();
        for (bool ok = cur.first(); ok; ok = cur.next())
            b.put(cur.key(), cur.value(), nosql::PutMode::Append);
    };
    copy(src.mainDb(), dst.mainDb());
    for (const std::string& name : src.listDbs())
        copy(src.db(nosql::Slice(name)), dst.db(nosql::Slice(name), nosql::DbFlags::Create));
    dst.commit();
    src.abort();
    out.sync(true);
}

/// ATTACH, DETACH, VACUUM: only outside a transaction.
Result connectionStatement(DatabaseImpl& db, StatementNode& node, const std::vector<Value>& params)
{
    if (auto* a = std::get_if<AttachStmt>(&node.body)) {
        const Value file = evalConstant(db, *a->file, params);
        const Value name = evalConstant(db, *a->schema, params);
        std::string fname, sname;
        appendText(file, fname);
        appendText(name, sname);
        if (sname.empty())
            throw Error(ErrorCode::Runtime, "invalid database name");
        if (equalsNoCase(sname, "main") || equalsNoCase(sname, "temp"))
            throw Error(ErrorCode::Runtime, "database " + sname + " is already in use");
        for (std::size_t i = 2; i < db.stores.size(); ++i) {
            if (db.stores[i] && equalsNoCase(db.stores[i]->name, sname))
                throw Error(ErrorCode::Runtime, "database " + sname + " is already in use");
        }
        if (db.stores.size() >= 12)
            throw Error(ErrorCode::Runtime, "too many attached databases - max 10");
        db.stores.push_back(openFile(sname, fname, db.options, db.readOnly, !db.readOnly));
        return Result();
    }
    if (auto* d = std::get_if<DetachStmt>(&node.body)) {
        const Value name = evalConstant(db, *d->schema, params);
        std::string sname;
        appendText(name, sname);
        if (equalsNoCase(sname, "main") || equalsNoCase(sname, "temp"))
            throw Error(ErrorCode::Runtime, "cannot detach database " + sname);
        for (std::size_t i = 2; i < db.stores.size(); ++i) {
            if (db.stores[i] && equalsNoCase(db.stores[i]->name, sname)) {
                db.stores.erase(db.stores.begin() + std::ptrdiff_t(i));
                db.cache.clear();
                return Result();
            }
        }
        throw Error(ErrorCode::Runtime, "no such database: " + sname);
    }
    auto& v = std::get<VacuumStmt>(node.body);
    std::size_t store = 0;
    if (!v.schema.empty()) {
        bool found = false;
        for (std::size_t i = 0; i < db.stores.size(); ++i) {
            const std::string n = i == 0 ? "main" : (i == 1 ? "temp" : (db.stores[i] ? db.stores[i]->name : ""));
            if (equalsNoCase(n, v.schema) && db.stores[i]) {
                store = i;
                found = true;
            }
        }
        if (!found)
            throw Error(ErrorCode::Runtime, "unknown database " + v.schema);
    }
    if (v.into) {
        const Value file = evalConstant(db, *v.into, params);
        std::string fname;
        appendText(file, fname);
        copyStore(db.stores[store]->env, fname);
    }
    // In place: libnosql already reuses freed pages, and the store may be
    // open in other handles, so there is nothing to rebuild (divergence V17).
    return Result();
}

/// Runs a whole batch the way Database::exec and Statement::exec do.
Result execBatch(const std::shared_ptr<DatabaseImpl>& dbp, Batch& b,
                 const std::vector<Value>& params, const RowVisitor* each = nullptr)
{
    DatabaseImpl& db = *dbp;
    Result last;
    const std::size_t n = b.meta.size();
    if (!db.sqlTxn && !b.anyControl && !b.anyConnection) {
        // One transaction for the whole batch (libsql's contract, V4).
        for (std::size_t k = 0; k < n; ++k)
            checkWritable(db, b.meta[k], nullptr);
        Parked held{newTxn(dbp, db.readOnly)};
        std::shared_ptr<TxnImpl>& t = held.t;
        start(*t, b.needsWrite);
        for (std::size_t k = 0; k < n; ++k)
            last = runOne(b, k, *t, params, false, each);
        commit(*t);
        return last;
    }
    for (std::size_t k = 0; k < n; ++k) {
        const StatementMeta& m = b.meta[k];
        if (m.control) {
            const std::unique_ptr<StatementNode> node = b.freshNode(k);
            const auto& st = std::get<TransactionStmt>(node->body);
            switch (st.kind) {
                case TransactionStmt::Kind::Begin: {
                    if (db.sqlTxn)
                        throw Error(ErrorCode::Runtime, "cannot start a transaction within a transaction");
                    auto t = newTxn(dbp, db.readOnly);
                    t->sql = true;
                    if (st.mode != 0) {
                        if (db.readOnly)
                            throw Error(ErrorCode::ReadOnly, "attempt to write a readonly database");
                        start(*t, true);
                    }
                    adoptSqlTxn(db, std::move(t));
                    break;
                }
                case TransactionStmt::Kind::Commit:
                    if (!db.sqlTxn)
                        throw Error(ErrorCode::Runtime, "cannot commit - no transaction is active");
                    commit(*db.sqlTxn);
                    park(std::move(db.sqlTxn));
                    break;
                case TransactionStmt::Kind::Rollback:
                    if (!db.sqlTxn)
                        throw Error(ErrorCode::Runtime, "cannot rollback - no transaction is active");
                    park(std::move(db.sqlTxn));
                    break;
                case TransactionStmt::Kind::Savepoint: {
                    if (!db.sqlTxn) {
                        auto t = newTxn(dbp, db.readOnly);
                        t->sql = true;
                        t->bySavepoint = true;
                        adoptSqlTxn(db, std::move(t));
                    }
                    bool done = false;
                    savepointControl(*db.sqlTxn, st, done);
                    break;
                }
                case TransactionStmt::Kind::Release:
                case TransactionStmt::Kind::RollbackTo: {
                    if (!db.sqlTxn)
                        throw Error(ErrorCode::Runtime, "no such savepoint: " + st.name);
                    bool done = false;
                    savepointControl(*db.sqlTxn, st, done);
                    if (done)
                        park(std::move(db.sqlTxn));
                    break;
                }
            }
            last = Result();
            continue;
        }
        if (m.connection) {
            std::unique_ptr<StatementNode> node = b.freshNode(k);
            if (db.sqlTxn)
                throw Error(ErrorCode::Runtime, std::holds_alternative<VacuumStmt>(node->body)
                                                    ? "cannot VACUUM from within a transaction"
                                                    : "cannot ATTACH database within transaction");
            last = connectionStatement(db, *node, params);
            continue;
        }
        if (db.sqlTxn) {
            std::shared_ptr<TxnImpl> t = db.sqlTxn;
            try {
                last = runOne(b, k, *t, params, true, each);
            } catch (...) {
                if (t->doomed)
                    park(std::move(db.sqlTxn));
                throw;
            }
            continue;
        }
        Parked held{newTxn(dbp, db.readOnly)};
        start(*held.t, m.mutating || m.writesPragma);
        last = runOne(b, k, *held.t, params, false, each);
        commit(*held.t);
    }
    return last;
}

/// One-off text run with positional values must supply every parameter it
/// uses; a prepared statement instead leaves unbound ones NULL, as SQLite does.
void checkCount(const Batch& b, const std::vector<Value>& params)
{
    if (params.size() < b.parameters)
        throw Error(ErrorCode::InvalidArgument, "statement wants " + std::to_string(b.parameters) +
                                                    " parameters, " + std::to_string(params.size()) +
                                                    " were bound");
}

/// `name` spelled with its prefix (":id") matches exactly; spelled without
/// one ("id") it matches :id, @id or $id.
bool parameterMatches(const std::string& spelled, std::string_view name) noexcept
{
    if (spelled == name)
        return true;
    if (name.empty() || name[0] == ':' || name[0] == '@' || name[0] == '$' || name[0] == '?')
        return false;
    return spelled.size() == name.size() + 1 && spelled[0] != '?' &&
           std::string_view(spelled).substr(1) == name;
}

std::vector<Value> bindNamed(const Batch& b, const NamedParams& named)
{
    std::vector<Value> out(b.parameters);
    for (const auto& [name, v] : named) {
        bool found = false;
        for (std::size_t i = 0; i < b.parameterNames.size(); ++i) {
            if (parameterMatches(b.parameterNames[i], name)) {
                out[i] = v;
                found = true;
            }
        }
        if (!found)
            throw Error(ErrorCode::InvalidArgument, "no parameter named " + name);
    }
    return out;
}

}  // namespace

struct StatementImpl
{
    std::shared_ptr<DatabaseImpl> db;
    std::unique_ptr<Batch> batch;
    Params bound;
};

}  // namespace internal

// ------------------------------------------------------------ accessors ---

namespace {

int columnIndex(const ColumnNames* names, std::size_t size, std::string_view column) noexcept
{
    if (!names)
        return -1;
    for (std::size_t i = 0; i < names->names.size() && i < size; ++i) {
        if (internal::equalsNoCase(names->names[i], column))
            return int(i);
    }
    for (std::size_t i = 0; i < names->qualified.size() && i < size; ++i) {
        if (!names->qualified[i].empty() && internal::equalsNoCase(names->qualified[i], column))
            return int(i);
    }
    return -1;
}

}  // namespace

Values::Values(const Value* first, std::size_t count) : size_(count)
{
    data_ = count <= kInline ? reinterpret_cast<Value*>(inline_)
                             : static_cast<Value*>(::operator new(count * sizeof(Value)));
    std::size_t built = 0;
    try {
        for (; built < count; ++built)
            new (data_ + built) Value(first[built]);
    } catch (...) {
        size_ = built;
        release();
        throw;
    }
}

void Values::fill(Value* first, std::size_t count, bool move, Value* cells)
{
    release();
    const bool borrow = count > kInline && cells;
    Value* data = count <= kInline ? reinterpret_cast<Value*>(inline_)
                  : borrow         ? cells
                                   : static_cast<Value*>(::operator new(count * sizeof(Value)));
    std::size_t built = 0;
    try {
        for (; built < count; ++built) {
            if (move) {
                first[built].own();
                new (data + built) Value(std::move(first[built]));
            } else {
                new (data + built) Value(first[built]);
            }
        }
    } catch (...) {
        for (std::size_t i = 0; i < built; ++i)
            data[i].~Value();
        if (count > kInline && !borrow)
            ::operator delete(data);
        throw;
    }
    data_ = data;
    size_ = count;
    borrowed_ = borrow;
}

void Values::refill(const Value* first)
{
    for (std::size_t i = 0; i < size_; ++i)
        data_[i] = first[i];
}

Values::Values(std::vector<Value>&& values) : size_(values.size())
{
    data_ = size_ <= kInline ? reinterpret_cast<Value*>(inline_)
                             : static_cast<Value*>(::operator new(size_ * sizeof(Value)));
    for (std::size_t i = 0; i < size_; ++i)
        new (data_ + i) Value(std::move(values[i]));
}

Values& Values::operator=(const Values& other)
{
    if (this != &other) {
        Values copy(other);
        release();
        take(copy);
    }
    return *this;
}

Values& Values::operator=(Values&& other) noexcept
{
    if (this != &other) {
        release();
        take(other);
    }
    return *this;
}

void Values::take(Values& other) noexcept
{
    size_ = other.size_;
    borrowed_ = other.borrowed_;
    other.borrowed_ = false;
    if (other.data_ && !other.inlined()) {
        data_ = other.data_;
    } else {
        data_ = reinterpret_cast<Value*>(inline_);
        for (std::size_t i = 0; i < size_; ++i) {
            new (data_ + i) Value(std::move(other.data_[i]));
            other.data_[i].~Value();
        }
        if (!other.data_)
            data_ = nullptr;
    }
    other.data_ = nullptr;
    other.size_ = 0;
}

void Values::release() noexcept
{
    for (std::size_t i = 0; i < size_; ++i)
        data_[i].~Value();
    if (data_ && !inlined() && !borrowed_)
        ::operator delete(data_);
    data_ = nullptr;
    size_ = 0;
    borrowed_ = false;
}

Result::Result(const Result& other)
    : names_(other.names_), rows_(other.rows_), changes_(other.changes_), lastInsertId_(other.lastInsertId_)
{
}

Result& Result::operator=(const Result& other)
{
    if (this != &other) {
        Result copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Result::Result(Result&& other) noexcept
    : names_(std::move(other.names_)),
      chunks_(std::move(other.chunks_)),
      chunkSize_(other.chunkSize_),
      chunkFree_(other.chunkFree_),
      rows_(std::move(other.rows_)),
      spare_(std::move(other.spare_)),
      changes_(other.changes_),
      lastInsertId_(other.lastInsertId_)
{
    other.rows_.clear();
    other.spare_.clear();
    other.chunks_.clear();
    other.chunkSize_ = other.chunkFree_ = 0;
}

Result& Result::operator=(Result&& other) noexcept
{
    if (this != &other) {
        // Rows first: they destroy the values that live in our chunks.
        rows_.clear();
        spare_.clear();
        names_ = std::move(other.names_);
        chunks_ = std::move(other.chunks_);
        chunkSize_ = other.chunkSize_;
        chunkFree_ = other.chunkFree_;
        rows_ = std::move(other.rows_);
        spare_ = std::move(other.spare_);
        changes_ = other.changes_;
        lastInsertId_ = other.lastInsertId_;
        other.rows_.clear();
        other.spare_.clear();
        other.chunks_.clear();
        other.chunkSize_ = other.chunkFree_ = 0;
    }
    return *this;
}

Result::~Result()
{
    // The rows destroy the values in the chunks before the chunks go.
    rows_.clear();
}

Value* Result::cells(std::size_t count)
{
    if (chunkFree_ < count) {
        // Chunks double from 64 values up to 4096; a wider row gets its own.
        const std::size_t next = chunkSize_ == 0 ? 64 : std::min<std::size_t>(chunkSize_ * 2, 4096);
        const std::size_t n = std::max(next, count);
        chunks_.emplace_back(new unsigned char[n * sizeof(Value)]);
        chunkSize_ = n;
        chunkFree_ = n;
    }
    Value* at = reinterpret_cast<Value*>(chunks_.back().get()) + (chunkSize_ - chunkFree_);
    chunkFree_ -= count;
    return at;
}

const Value& RowView::at(std::size_t i) const
{
    if (i >= size_)
        throw Error(ErrorCode::InvalidArgument, "column " + std::to_string(i) + " is out of range");
    return values_[i];
}

const Value& RowView::at(std::string_view column) const
{
    const int i = columnIndex(names_->get(), size_, column);
    if (i < 0)
        throw Error(ErrorCode::NoSuchColumn, "no such result column: " + std::string(column));
    return values_[std::size_t(i)];
}

const std::vector<std::string>& RowView::columns() const
{
    static const std::vector<std::string> kNone;
    return *names_ ? (*names_)->names : kNone;
}

Row RowView::row() const
{
    return Row(*names_, Values(values_, size_));
}

const Value& Row::at(std::size_t i) const
{
    if (i >= values_.size())
        throw Error(ErrorCode::InvalidArgument, "column " + std::to_string(i) + " is out of range");
    return values_[i];
}

bool Row::has(std::string_view column) const noexcept
{
    return columnIndex(names_.get(), values_.size(), column) >= 0;
}

const Value& Row::at(std::string_view column) const
{
    const int i = columnIndex(names_.get(), values_.size(), column);
    if (i < 0)
        throw Error(ErrorCode::NoSuchColumn, "no such result column: " + std::string(column));
    return values_[std::size_t(i)];
}

const std::vector<std::string>& Result::columns() const
{
    static const std::vector<std::string> kNone;
    return names_ ? names_->names : kNone;
}

const std::vector<std::string>& Result::qualifiedColumns() const
{
    static const std::vector<std::string> kNone;
    return names_ ? names_->qualified : kNone;
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
    internal::park(std::move(impl_));
}

Result Transaction::exec(std::string_view text, const NamedParams& params)
{
    internal::TxnImpl& t = internal::live(impl_);
    return exec(text, internal::bindNamed(*t.cache.get(text), params));
}

Result Transaction::exec(std::string_view text, const Params& params)
{
    internal::TxnImpl& t = internal::live(impl_);
    const std::shared_ptr<internal::Batch> held = t.cache.get(text);
    internal::Batch& b = *held;
    internal::checkCount(b, params);
    Result last;
    for (std::size_t k = 0; k < b.meta.size(); ++k) {
        const internal::StatementMeta& m = b.meta[k];
        if (m.connection)
            throw Error(ErrorCode::Runtime, "cannot ATTACH database within transaction");
        if (m.control) {
            const std::unique_ptr<internal::StatementNode> node = b.freshNode(k);
            const auto& st = std::get<internal::TransactionStmt>(node->body);
            if (st.kind == internal::TransactionStmt::Kind::Begin)
                throw Error(ErrorCode::Runtime, "cannot start a transaction within a transaction");
            if (st.kind == internal::TransactionStmt::Kind::Commit ||
                st.kind == internal::TransactionStmt::Kind::Rollback)
                throw Error(ErrorCode::BadTransaction, "use Transaction::commit() or rollback()");
            bool done = false;
            internal::savepointControl(t, st, done);
            last = Result();
            continue;
        }
        try {
            last = internal::runOne(b, k, t, params, true);
        } catch (...) {
            if (t.doomed)
                internal::park(std::move(impl_));
            throw;
        }
    }
    return last;
}

void Transaction::commit()
{
    internal::TxnImpl& t = internal::live(impl_);
    internal::commit(t);
    internal::park(std::move(impl_));
}

void Transaction::rollback()
{
    internal::park(std::move(impl_));
}

nosql::Txn& Transaction::txn()
{
    internal::TxnImpl& t = internal::live(impl_);
    if (!t.levels.empty()) {
        for (auto it = t.levels.rbegin(); it != t.levels.rend(); ++it) {
            if (!(*it)->txns.empty() && (*it)->txns[0].valid())
                return (*it)->txns[0];
        }
    }
    return t.roots[0];
}

// ----------------------------------------------------- prepared statement --

namespace {

internal::StatementImpl& live(const std::shared_ptr<internal::StatementImpl>& impl)
{
    if (!impl)
        throw Error(ErrorCode::InvalidArgument, "the statement is not prepared");
    return *impl;
}

/// Binds every parameter into the statement's own cells, which keep their
/// buffers; a borrowing value stays a borrow, as bind(index, value) keeps it.
void bindInPlace(internal::StatementImpl& s, std::initializer_list<Value> params)
{
    if (params.size() != s.bound.size())
        throw Error(ErrorCode::InvalidArgument,
                    "statement takes " + std::to_string(s.bound.size()) + " parameters, " +
                        std::to_string(params.size()) + " were given");
    Value* cell = s.bound.data();
    for (const Value& v : params) {
        if (v.borrowed())
            cell->borrow(v);
        else
            *cell = v;
        ++cell;
    }
}

}  // namespace

Statement::Statement(Statement&&) noexcept = default;
Statement& Statement::operator=(Statement&&) noexcept = default;
Statement::~Statement() = default;

const std::string& Statement::sql() const
{
    return live(impl_).batch->text;
}

std::size_t Statement::parameters() const
{
    return live(impl_).bound.size();
}

std::size_t Statement::parameterIndex(std::string_view name) const
{
    const internal::Batch& b = *live(impl_).batch;
    for (std::size_t i = 0; i < b.parameterNames.size(); ++i) {
        if (internal::parameterMatches(b.parameterNames[i], name))
            return i + 1;
    }
    return 0;
}

std::string Statement::parameterName(std::size_t index) const
{
    const internal::Batch& b = *live(impl_).batch;
    if (index < 1 || index > b.parameterNames.size())
        return {};
    return b.parameterNames[index - 1];
}

bool Statement::writes() const
{
    return live(impl_).batch->needsWrite;
}

std::vector<std::string> Statement::columnNames()
{
    internal::StatementImpl& s = live(impl_);
    internal::Batch& b = *s.batch;
    if (b.meta.empty() || b.meta.back().control || b.meta.back().connection)
        return {};
    const std::size_t k = b.meta.size() - 1;
    internal::Parked held{internal::newTxn(s.db, true)};
    internal::start(*held.t, false);
    internal::ensurePlanned(b, k, held.t->session);
    internal::Program& p = *b.programs[k];
    if (p.select)
        return std::get<internal::SelectStmt>(p.node->body).forJson ? std::vector<std::string>{"json"}
                                                                     : p.select->names;
    return {};
}

Statement& Statement::bind(std::size_t index, Value value)
{
    internal::StatementImpl& s = live(impl_);
    if (index < 1 || index > s.bound.size())
        throw Error(ErrorCode::InvalidArgument, "no parameter " + std::to_string(index) + "; the statement has " +
                                                    std::to_string(s.bound.size()));
    s.bound[index - 1] = std::move(value);
    return *this;
}

Statement& Statement::bind(std::string_view name, Value value)
{
    const std::size_t i = parameterIndex(name);
    if (i == 0)
        throw Error(ErrorCode::InvalidArgument, "no parameter named " + std::string(name));
    return bind(i, std::move(value));
}

Statement& Statement::bind(const Params& params)
{
    internal::StatementImpl& s = live(impl_);
    if (params.size() != s.bound.size())
        throw Error(ErrorCode::InvalidArgument, "statement takes " + std::to_string(s.bound.size()) +
                                                    " parameters, " + std::to_string(params.size()) +
                                                    " were given");
    s.bound = params;
    return *this;
}

Statement& Statement::bind(const NamedParams& params)
{
    for (const auto& [name, v] : params)
        bind(std::string_view(name), v);
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
    return internal::execBatch(s.db, *s.batch, s.bound);
}

Result Statement::exec(Transaction& txn)
{
    return run(txn, nullptr);
}

Result Statement::exec(std::initializer_list<Value> params)
{
    bindInPlace(live(impl_), params);
    return exec();
}

Result Statement::exec(Transaction& txn, std::initializer_list<Value> params)
{
    bindInPlace(live(impl_), params);
    return exec(txn);
}

Result& Statement::execInto(Transaction& txn, Result& out)
{
    run(txn, nullptr, &out);
    return out;
}

std::uint64_t Statement::each(const RowCallback& fn)
{
    return eachRow(nullptr, RowVisitor(fn));
}

std::uint64_t Statement::each(Transaction& txn, const RowCallback& fn)
{
    return eachRow(&txn, RowVisitor(fn));
}

std::uint64_t Statement::eachRow(Transaction* txn, RowVisitor fn)
{
    if (txn)
        return run(*txn, &fn).changes();
    internal::StatementImpl& s = live(impl_);
    return internal::execBatch(s.db, *s.batch, s.bound, &fn).changes();
}

Result Statement::run(Transaction& txn, const RowVisitor* each, Result* into)
{
    internal::StatementImpl& s = live(impl_);
    internal::TxnImpl& t = internal::live(txn.impl_);
    internal::Batch& b = *s.batch;
    if (b.anyControl || b.anyConnection)
        throw Error(ErrorCode::BadTransaction, "transaction control inside a Transaction; use its methods");
    Result last;
    for (std::size_t k = 0; k < b.meta.size(); ++k) {
        try {
            last = internal::runOne(b, k, t, s.bound, true, each, into);
        } catch (...) {
            if (t.doomed)
                internal::park(std::move(txn.impl_));
            throw;
        }
    }
    return last;
}

// ------------------------------------------------------------- database ---

Database Database::Options::open(const std::filesystem::path& path) const
{
    auto impl = std::make_shared<internal::DatabaseImpl>();
    impl->options.maxSize = maxSize_;
    impl->options.durability = durabilitySet_
                                   ? durability_
                                   : (durable_ ? nosql::Durability::Safe : nosql::Durability::None);
    impl->options.cacheReads = cacheReads_;
    impl->options.revalidate = revalidateAfter_;
    impl->options.syncInterval = syncInterval_;
    impl->options.pageSize = pageSize_;
    impl->options.initialSize = initialSize_;
    impl->options.growthStep = growthStep_;
    impl->options.bufferCache = bufferCache_;
    impl->options.dirtyLimit = dirtyLimit_;
    impl->conn.busyTimeout = busyTimeout_.count();
    struct Preset
    {
        std::uint64_t statements, scratch, sort, arena;
    };
    static constexpr Preset kPresets[] = {
        {8, 64ull << 10, 8ull << 20, 1024},      // Compact
        {32, 2ull << 20, 64ull << 20, 4096},     // Balanced
        {128, 16ull << 20, 256ull << 20, 16384}, // Throughput
    };
    const Preset& preset = kPresets[static_cast<int>(profile_)];
    auto pick = [](std::uint64_t set, std::uint64_t fallback) { return set == kUnset ? fallback : set; };
    impl->conn.sortMemory = pick(sortMemory_, preset.sort);
    impl->conn.scratchRetain = pick(scratchRetain_, preset.scratch);
    impl->options.statementCache = std::size_t(pick(statementCache_, preset.statements));
    impl->options.arenaBlock = std::size_t(pick(arenaBlock_, preset.arena));
    impl->cache.configure(impl->options);
    impl->readOnly = readOnly_;
    impl->stores.push_back(internal::openFile("main", path, impl->options, readOnly_, create_));
    impl->stores.emplace_back();  // temp, opened on first use
    Database db;
    db.impl_ = std::move(impl);
    return db;
}

Database::~Database() = default;

const std::filesystem::path& Database::path() const
{
    return internal::open(impl_).stores[0]->path;
}

Result Database::exec(std::string_view text, const Params& params)
{
    internal::DatabaseImpl& db = internal::open(impl_);
    const std::shared_ptr<internal::Batch> b = db.cached(text);
    internal::checkCount(*b, params);
    return internal::execBatch(impl_, *b, params);
}

Result Database::exec(std::string_view text, const NamedParams& params)
{
    internal::DatabaseImpl& db = internal::open(impl_);
    const std::shared_ptr<internal::Batch> b = db.cached(text);
    return internal::execBatch(impl_, *b, internal::bindNamed(*b, params));
}

Statement Database::prepare(std::string_view text)
{
    internal::DatabaseImpl& db = internal::open(impl_);
    auto stmt = std::make_shared<internal::StatementImpl>();
    stmt->db = impl_;
    stmt->batch = std::make_unique<internal::Batch>(text, db.options.arenaBlock);
    for (const internal::StatementMeta& m : stmt->batch->meta) {
        if ((m.mutating || m.writesPragma) && db.readOnly)
            throw Error(ErrorCode::ReadOnly, "attempt to write a readonly database");
    }
    stmt->bound.assign(stmt->batch->parameters, Value());
    return Statement(std::move(stmt));
}

Transaction Database::begin()
{
    internal::DatabaseImpl& db = internal::open(impl_);
    if (db.sqlTxn)
        throw Error(ErrorCode::BadTransaction, "a transaction opened with BEGIN is active");
    auto t = internal::newTxn(impl_, db.readOnly);
    internal::start(*t, !db.readOnly);
    return Transaction(std::move(t));
}

Transaction Database::beginRead() const
{
    internal::DatabaseImpl& db = internal::open(impl_);
    if (db.sqlTxn)
        throw Error(ErrorCode::BadTransaction, "a transaction opened with BEGIN is active");
    auto t = internal::newTxn(impl_, true);
    internal::start(*t, false);
    return Transaction(std::move(t));
}

Database Database::openReader() const
{
    internal::DatabaseImpl& source = internal::open(impl_);
    auto impl = std::make_shared<internal::DatabaseImpl>();
    impl->options = source.options;
    impl->conn.sortMemory = source.conn.sortMemory.load(std::memory_order_relaxed);
    impl->conn.scratchRetain = source.conn.scratchRetain.load(std::memory_order_relaxed);
    impl->cache.configure(impl->options);
    impl->readOnly = true;
    auto main = std::make_unique<internal::StoreHandle>();
    main->name = "main";
    main->path = source.stores[0]->path;
    main->displayPath = source.stores[0]->displayPath;
    main->env = source.stores[0]->env.share();
    main->readOnly = true;
    impl->stores.push_back(std::move(main));
    impl->stores.emplace_back();
    Database db;
    db.impl_ = std::move(impl);
    return db;
}

std::int64_t Database::changes() const
{
    return internal::open(impl_).conn.changes;
}

std::int64_t Database::totalChanges() const
{
    return internal::open(impl_).conn.totalChanges;
}

std::int64_t Database::lastInsertRowid() const
{
    return internal::open(impl_).conn.lastInsertRowid;
}

bool Database::inTransaction() const
{
    return internal::open(impl_).sqlTxn != nullptr;
}

void Database::interrupt()
{
    internal::open(impl_).interrupted.store(true, std::memory_order_relaxed);
}

namespace {

template <class Fn>
auto withSchema(const std::shared_ptr<internal::DatabaseImpl>& impl, Fn&& fn)
{
    internal::DatabaseImpl& db = internal::open(impl);
    nosql::Txn t = db.stores[0]->env.readTxn();
    internal::Catalog cat(t, db.stores[0]->schema);
    auto r = fn(cat);
    t.abort();
    return r;
}

ColumnInfo columnInfo(const internal::Table& t, std::size_t i)
{
    const internal::Column& c = t.columns[i];
    ColumnInfo info;
    info.name = c.name;
    if (t.strict) {
        switch (c.strictType) {
            case internal::StrictType::Any: info.type = Type::Null; break;
            case internal::StrictType::Integer: info.type = Type::Integer; break;
            case internal::StrictType::Real: info.type = Type::Real; break;
            case internal::StrictType::Text: info.type = Type::Text; break;
            case internal::StrictType::Blob: info.type = Type::Blob; break;
            case internal::StrictType::Datetime: info.type = Type::Datetime; break;
        }
    }
    info.declaredType = c.declType;
    info.affinity = internal::toString(c.affinity);
    info.collation = internal::toString(c.collation);
    info.primaryKey = c.primaryKey;
    info.notNull = c.notNull;
    info.hasDefault = c.hasDefault;
    if (c.hasDefault && c.defaultConstant)
        info.defaultValue = c.defaultValue;
    info.defaultSql = c.defaultSql;
    info.generated = c.generated;
    for (const internal::Index* ix : t.indexes) {
        if (ix->unique && ix->parts.size() == 1 && ix->parts[0].column == int(i) && ix->whereSql.empty())
            info.unique = true;
    }
    if (t.primaryKey.size() == 1 && t.primaryKey[0] == int(i))
        info.unique = true;
    return info;
}

}  // namespace

std::vector<std::string> Database::tables() const
{
    return withSchema(impl_, [](internal::Catalog& cat) {
        std::vector<std::string> out;
        for (const auto& t : cat.schema().tables) {
            if (t->name.size() >= 7 && internal::equalsNoCase(t->name.substr(0, 7), "sqlite_"))
                continue;
            out.push_back(t->name);
        }
        return out;
    });
}

TableInfo Database::table(std::string_view name) const
{
    const std::string wanted(name);
    return withSchema(impl_, [&](internal::Catalog& cat) {
        const internal::Table* t = cat.findTable(wanted);
        if (!t)
            throw Error(ErrorCode::NoSuchTable, "no such table: " + wanted);
        TableInfo info;
        info.name = t->name;
        for (std::size_t i = 0; i < t->columns.size(); ++i)
            info.columns.push_back(columnInfo(*t, i));
        info.primaryKey = t->primaryKey;
        info.strict = t->strict;
        info.withoutRowid = t->withoutRowid;
        info.sql = t->sql;
        return info;
    });
}

std::vector<IndexInfo> Database::indexes(std::string_view table) const
{
    const std::string wanted(table);
    return withSchema(impl_, [&](internal::Catalog& cat) {
        std::vector<IndexInfo> out;
        for (const auto& ix : cat.schema().indexes) {
            if (!wanted.empty() && !internal::equalsNoCase(ix->table, wanted))
                continue;
            const internal::Table* t = cat.findTable(ix->table);
            IndexInfo info;
            info.name = ix->name;
            info.table = ix->table;
            info.unique = ix->unique;
            info.partial = !ix->whereSql.empty();
            info.sql = ix->sql;
            for (const internal::IndexPart& p : ix->parts) {
                if (p.column >= 0)
                    info.columns.push_back(t->columns[std::size_t(p.column)].name);
                else if (p.column == -2)
                    info.columns.push_back("rowid");
                else
                    info.columns.push_back(p.exprSql);
            }
            out.push_back(std::move(info));
        }
        return out;
    });
}

nosql::Env& Database::env() const
{
    return internal::open(impl_).stores[0]->env;
}

void Database::close()
{
    impl_.reset();
}

const char* version() noexcept
{
    return "0.2.0";
}

bool complete(std::string_view text)
{
    return internal::isCompleteStatement(text);
}

}  // namespace sql
