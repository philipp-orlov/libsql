// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// sql::remote::SqlSocket: serves the sessions of a sql::Database through librpc. The transport -- WebSocket route
// or local socket, framing, authentication hooks, limits, flow control by window, cancellation, graceful shutdown --
// is rpc::Endpoint's; what is here is the database side of a session (include/sql/wire/remote.bufs):
//   * Open gives the connection its database handle; the handle, the prepared statements and the transactions live in
//     the session state librpc keeps per connection and are torn down (open transactions rolled back, the handle
//     returned to a small pool) when it ends;
//   * a session's calls run one at a time in arrival order (rpc::EndpointOptions::serialPerSession);
//   * rows stream as sql.wire.ResultSet batches built by a per-thread BufsWriter straight into the stream, and
//     the stream's trailer carries the counters;
//   * cancelling a call interrupts the running statement (ServerContext::onCancel).

#include "sql/remote/sql_socket.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bufs/bufs.hpp"
#include "errors.hpp"
#include "rpc/server.hpp"
#include "sql/bufs.hpp"
#include "sql/wire/remote.rpc.hpp"

namespace sql::remote {

using Clock = std::chrono::steady_clock;

namespace {

/// Ends a statement whose consumer is gone: the call was cancelled, the connection closed, or the client stopped
/// reading.
struct Abort
{
};

/// Forwards to a BufsWriter and remembers how the statement ended.
struct Tee final : ResultConsumer
{
    explicit Tee(BufsWriter& w) : writer(w) {}
    void begin(const ResultShape& shape) override { writer.begin(shape); }
    bool row(const RowView& r) override { return writer.row(r); }
    void end(std::uint64_t c, std::int64_t id) override
    {
        changes = c;
        lastInsertId = id;
        writer.end(c, id);
    }
    BufsWriter& writer;
    std::uint64_t changes = 0;
    std::int64_t lastInsertId = 0;
};

BufsWriter& workerWriter()
{
    static thread_local BufsWriter writer;
    return writer;
}

}  // namespace

// ============================================================================================ SqlSocket::Impl ===

struct SqlSocket::Impl
{
    Database* db = nullptr;
    SqlSocketOptions o;
    std::string serverName;
    std::unique_ptr<rpc::Endpoint> endpoint;
    std::unique_ptr<rpc::Service> service;

    std::atomic<std::uint64_t> opened{0}, refused{0}, errors{0}, interrupts{0};
    std::atomic<std::size_t> peak{0};

    std::mutex mutex;
    std::vector<Database> idleWriters, idleReaders;
    std::unordered_map<std::string, std::size_t> perAddress;

    // Statements running now (one per session at most). The engine clears a handle's interrupt flag when a statement
    // starts, so an interrupt aimed at one that has not quite started would vanish: the watchdog therefore repeats
    // the interrupt of a statement that was told to stop, every tick, until the statement is gone. It also enforces
    // the statement timeout.
    struct RunState
    {
        Database* handle = nullptr;
        Clock::time_point started;
        std::atomic<bool> cancelled{false};
        bool timedOut = false;
    };
    std::vector<RunState*> running;
    std::thread watchdog;
    std::condition_variable watchdogWake;
    bool stopWatchdog = false;

    void startWatchdog()
    {
        running.reserve(256);
        watchdog = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stopWatchdog) {
                if (running.empty()) {
                    watchdogWake.wait(lock, [this] { return stopWatchdog || !running.empty(); });
                    continue;
                }
                watchdogWake.wait_for(lock, std::chrono::milliseconds(20));
                const auto now = Clock::now();
                for (RunState* r : running) {
                    const bool late = o.statementTimeout_.count() > 0 && now - r->started > o.statementTimeout_;
                    if (late && !r->timedOut) {
                        r->timedOut = true;
                        interrupts.fetch_add(1);
                    }
                    if (late || r->cancelled.load())
                        r->handle->interrupt();
                }
            }
        });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopWatchdog = true;
        }
        watchdogWake.notify_all();
        if (watchdog.joinable())
            watchdog.join();
    }
};

namespace {

/// What the principal of a session is, carried in rpc::Principal::data.
struct PrincipalData
{
    Principal principal;
};

/// Everything a session owns. Torn down when the last reference goes (librpc drops it on a worker).
struct SessionState
{
    SqlSocket::Impl* impl = nullptr;
    bool opened = false;
    bool readOnly = false;
    bool countedAddress = false;
    std::string address;
    Principal principal;
    Database own;
    Database* handle = nullptr;
    std::vector<std::pair<std::uint32_t, Statement>> statements;
    std::vector<std::pair<std::uint32_t, Transaction>> transactions;
    // Reused for every request.
    Params params;
    NamedParams named;
    bool usesNamed = false;
    std::vector<std::string> parameterNames;
    SqlSocket::Impl::RunState run;  // the statement this session is running, for the watchdog

    ~SessionState()
    {
        statements.clear();
        transactions.clear();  // rolls back whatever is open
        if (handle && handle == &own) {
            bool healthy = true;
            try {
                if (own.inTransaction())
                    own.exec("ROLLBACK");
            } catch (...) {
                healthy = false;
            }
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto& idle = readOnly ? impl->idleReaders : impl->idleWriters;
            if (healthy && idle.size() < impl->o.idleHandles_)
                idle.push_back(std::move(own));
            else
                own = Database();
        } else if (handle) {
            try {
                if (handle->inTransaction())
                    handle->exec("ROLLBACK");
            } catch (...) {
            }
        }
        if (countedAddress) {
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto it = impl->perAddress.find(address);
            if (it != impl->perAddress.end() && --it->second == 0)
                impl->perAddress.erase(it);
        }
    }
};

std::string hostOf(std::string_view address)
{
    const std::size_t colon = address.rfind(':');
    return std::string(colon == std::string_view::npos ? address : address.substr(0, colon));
}

class Service final : public SqlService
{
public:
    explicit Service(SqlSocket::Impl& impl) : impl_(impl) {}

    // ------------------------------------------------------------------------------------------- session ---

    void Open(rpc::ServerContext& ctx, bool readOnly, std::string_view /*client*/, std::uint32_t /*hints*/,
              Opened::Builder& result) override
    {
        if (ctx.sessionState())
            throw rpc::Error(rpc::Code::FailedPrecondition, "the session is already open");
        auto s = std::make_shared<SessionState>();
        s->impl = &impl_;
        if (const auto data = std::static_pointer_cast<PrincipalData>(ctx.principal().data))
            s->principal = data->principal;
        else
            s->principal.name = ctx.principal().name;
        s->readOnly = readOnly || impl_.o.readOnly_ || s->principal.readOnly;
        if (impl_.o.maxSessionsPerAddress_ != 0) {
            s->address = hostOf(ctx.remoteAddress());
            std::lock_guard<std::mutex> lock(impl_.mutex);
            if (++impl_.perAddress[s->address] > impl_.o.maxSessionsPerAddress_) {
                if (--impl_.perAddress[s->address] == 0)
                    impl_.perAddress.erase(s->address);
                impl_.refused.fetch_add(1);
                throw rpc::Error(rpc::Code::ResourceExhausted, "too many sessions from this address");
            }
            s->countedAddress = true;
        }
        acquireHandle(*s);
        s->opened = true;
        ctx.setSessionState(s);
        impl_.opened.fetch_add(1);
        const std::size_t now = impl_.endpoint->stats().sessions;
        std::size_t peak = impl_.peak.load();
        while (now > peak && !impl_.peak.compare_exchange_weak(peak, now)) {
        }
        result.setSession(ctx.sessionId());
        result.setReadOnly(s->readOnly);
        result.setServer(impl_.serverName);
    }

    // ------------------------------------------------------------------------------------ statements ---

    void Exec(rpc::ServerContext& ctx, std::string_view text, ParamList::Reader params, std::uint32_t txn,
              ResultOptions::Reader options, std::uint32_t hints, rpc::ServerWriter<sql::wire::ResultSet>& writer,
              StatementEnd::Builder& trailer) override
    {
        execute(ctx, "exec", text, false, 0, params, txn, options, hints, writer, trailer);
    }

    void ExecPrepared(rpc::ServerContext& ctx, std::uint32_t statement, ParamList::Reader params, std::uint32_t txn,
                      ResultOptions::Reader options, rpc::ServerWriter<sql::wire::ResultSet>& writer,
                      StatementEnd::Builder& trailer) override
    {
        execute(ctx, "exec_prepared", {}, true, statement, params, txn, options, 0, writer, trailer);
    }

    void Prepare(rpc::ServerContext& ctx, std::uint32_t id, std::string_view text, Prepared::Builder& result) override
    {
        const std::shared_ptr<SessionState> hold = session(ctx);
        SessionState& s = *hold;
        const Clock::time_point started = Clock::now();
        ErrorCode outcome = ErrorCode::Ok;
        try {
            if (s.statements.size() >= impl_.o.maxStatements_)
                throw Error(ErrorCode::Busy, "too many prepared statements");
            for (auto& entry : s.statements)
                if (entry.first == id)
                    throw Error(ErrorCode::InvalidArgument, "statement id is already in use");
            Statement st = s.handle->prepare(text);
            result.setParameters(static_cast<std::uint32_t>(st.parameters()));
            s.parameterNames.clear();
            for (std::size_t i = 1; i <= st.parameters(); ++i)
                s.parameterNames.push_back(st.parameterName(i));
            auto names = result.initParameterNames(s.parameterNames.size());
            for (std::size_t i = 0; i < s.parameterNames.size(); ++i)
                names.set(i, s.parameterNames[i]);
            const std::vector<std::string> columns = st.columnNames();
            auto cols = result.initColumns(columns.size());
            for (std::size_t i = 0; i < columns.size(); ++i)
                cols.set(i, columns[i]);
            result.setWrites(st.writes());
            s.statements.emplace_back(id, std::move(st));
        } catch (const std::exception& e) {
            outcome = finishError(e);
            report(s, "prepare", text, started, outcome);
            throw detail::toRpcError(e);
        }
        report(s, "prepare", text, started, outcome);
    }

    void ClosePrepared(rpc::ServerContext& ctx, std::uint32_t id) override
    {
        const std::shared_ptr<SessionState> hold = session(ctx);
        SessionState& s = *hold;
        for (std::size_t i = 0; i < s.statements.size(); ++i)
            if (s.statements[i].first == id) {
                s.statements.erase(s.statements.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
    }

    // ---------------------------------------------------------------------------------- transactions ---

    void Begin(rpc::ServerContext& ctx, std::uint32_t id, bool readOnly, Counters::Builder& result) override
    {
        const std::shared_ptr<SessionState> hold = session(ctx);
        SessionState& s = *hold;
        try {
            if (s.transactions.size() >= impl_.o.maxTransactions_)
                throw Error(ErrorCode::Busy, "too many open transactions");
            if (id == 0)
                throw Error(ErrorCode::InvalidArgument, "a transaction needs a non-zero id");
            for (auto& entry : s.transactions)
                if (entry.first == id)
                    throw Error(ErrorCode::InvalidArgument, "transaction id is already in use");
            s.transactions.emplace_back(id, readOnly ? s.handle->beginRead() : s.handle->begin());
        } catch (const std::exception& e) {
            finishError(e);
            throw detail::toRpcError(e);
        }
        counters(s, result);
    }

    void Commit(rpc::ServerContext& ctx, std::uint32_t id, Counters::Builder& result) override { finish(ctx, id, true, result); }
    void Rollback(rpc::ServerContext& ctx, std::uint32_t id, Counters::Builder& result) override { finish(ctx, id, false, result); }

    void State(rpc::ServerContext& ctx, Counters::Builder& result) override { counters(*session(ctx), result); }

    // ---------------------------------------------------------------------------------------- catalog ---

    void Describe(rpc::ServerContext& ctx, Catalog what, std::string_view name, ResultOptions::Reader options,
                  rpc::ServerWriter<sql::wire::ResultSet>& out, StatementEnd::Builder& trailer) override
    {
        const std::shared_ptr<SessionState> hold = session(ctx);
        SessionState& s = *hold;
        const Clock::time_point started = Clock::now();
        BufsWriter& writer = workerWriter();
        writer.reset();
        writer.options(optionsFor(ctx, options));
        writer.onBatch([&out](BufsBytes b) {
            if (!out.write(b.data(), b.size()))
                throw Abort{};
        });
        static const std::vector<std::string> kTables = {"name"};
        static const std::vector<std::string> kTable = {"name",         "type",          "declared_type", "affinity", "collation",
                                                        "primary_key",  "not_null",      "unique",        "has_default",
                                                        "default_value", "default_sql",  "generated",     "pk_position"};
        static const std::vector<std::string> kIndexes = {"index_name", "table_name", "unique", "partial", "sql", "position", "column"};
        static const std::vector<std::string> kBlank16(16);
        static const std::vector<std::string> kNone;
        std::string describedSql;
        bool strict = false, withoutRowid = false;
        ErrorCode outcome = ErrorCode::Ok;
        try {
            if (what == Catalog::Tables) {
                writer.begin(ResultShape{kTables, kNone, kBlank16});
                const std::vector<std::string> names = s.handle->tables();
                for (const std::string& n : names) {
                    Value row[1] = {Value::textRef(n)};
                    writer.row(row, 1);
                }
            } else if (what == Catalog::Table_) {
                const TableInfo info = s.handle->table(name);
                writer.begin(ResultShape{kTable, kNone, kBlank16});
                for (std::size_t i = 0; i < info.columns.size(); ++i) {
                    const ColumnInfo& c = info.columns[i];
                    std::int64_t position = 0;
                    for (std::size_t k = 0; k < info.primaryKey.size(); ++k)
                        if (static_cast<std::size_t>(info.primaryKey[k]) == i)
                            position = static_cast<std::int64_t>(k) + 1;
                    Value row[13] = {Value::textRef(c.name),
                                     Value(static_cast<std::int64_t>(c.type)),
                                     Value::textRef(c.declaredType),
                                     Value::textRef(c.affinity),
                                     Value::textRef(c.collation),
                                     Value(static_cast<std::int64_t>(c.primaryKey)),
                                     Value(static_cast<std::int64_t>(c.notNull)),
                                     Value(static_cast<std::int64_t>(c.unique)),
                                     Value(static_cast<std::int64_t>(c.hasDefault)),
                                     c.defaultValue,
                                     Value::textRef(c.defaultSql),
                                     Value(static_cast<std::int64_t>(c.generated)),
                                     Value(position)};
                    writer.row(row, 13);
                }
                describedSql = info.sql;
                strict = info.strict;
                withoutRowid = info.withoutRowid;
            } else {
                writer.begin(ResultShape{kIndexes, kNone, kBlank16});
                const std::vector<IndexInfo> indexes = s.handle->indexes(name);
                for (const IndexInfo& ix : indexes)
                    for (std::size_t k = 0; k < ix.columns.size(); ++k) {
                        Value row[7] = {Value::textRef(ix.name),
                                        Value::textRef(ix.table),
                                        Value(static_cast<std::int64_t>(ix.unique)),
                                        Value(static_cast<std::int64_t>(ix.partial)),
                                        Value::textRef(ix.sql),
                                        Value(static_cast<std::int64_t>(k)),
                                        Value::textRef(ix.columns[k])};
                        writer.row(row, 7);
                    }
            }
            writer.end(0, 0);
        } catch (const Abort&) {
            writer.onBatch({});
            throw rpc::Error(rpc::Code::Cancelled, "interrupted");
        } catch (const std::exception& e) {
            writer.onBatch({});
            outcome = finishError(e);
            report(s, "describe", name, started, outcome);
            throw detail::toRpcError(e);
        }
        writer.onBatch({});
        endOf(s, trailer, started, 0, 0);
        trailer.setSql(describedSql);
        trailer.setStrict(strict);
        trailer.setWithoutRowid(withoutRowid);
        report(s, "describe", name, started, outcome);
        compactIfBig(writer);
    }

private:
    /// The session's state, kept alive by the caller's reference for the whole call: the connection may end (and
    /// drop its own reference) while the call is still running.
    static std::shared_ptr<SessionState> session(rpc::ServerContext& ctx)
    {
        std::shared_ptr<SessionState> s = ctx.sessionState<SessionState>();
        if (!s || !s->opened)
            throw rpc::Error(rpc::Code::FailedPrecondition, "call Open first");
        return s;
    }

    void acquireHandle(SessionState& s)
    {
        // A read-only session never shares the writable handle (it would be able to write through it): it gets
        // a reader of its own, which sees what has been committed.
        if (impl_.o.isolation_ == Isolation::Shared && !s.readOnly) {
            s.handle = impl_.db;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl_.mutex);
            auto& idle = s.readOnly ? impl_.idleReaders : impl_.idleWriters;
            if (!idle.empty()) {
                s.own = std::move(idle.back());
                idle.pop_back();
            }
        }
        if (!s.own.valid())
            s.own = s.readOnly ? impl_.db->openReader() : impl_.db->openHandle();
        s.handle = &s.own;
    }

    ErrorCode finishError(const std::exception& e)
    {
        impl_.errors.fetch_add(1);
        if (const auto* error = dynamic_cast<const Error*>(&e))
            return error->code();
        return ErrorCode::Internal;
    }

    void report(SessionState& s, const char* op, std::string_view text, Clock::time_point started, ErrorCode result)
    {
        if (!impl_.o.onRequest_)
            return;
        try {
            impl_.o.onRequest_(s.principal, op, text,
                               static_cast<std::uint64_t>(
                                   std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count()),
                               result);
        } catch (...) {
        }
    }

    void counters(SessionState& s, Counters::Builder& out)
    {
        out.setChanges(s.handle->changes());
        out.setTotalChanges(s.handle->totalChanges());
        out.setLastInsertId(s.handle->lastInsertRowid());
        out.setInTransaction(s.handle->inTransaction());
    }

    void endOf(SessionState& s, StatementEnd::Builder& trailer, Clock::time_point started, std::uint64_t changes, std::int64_t lastId)
    {
        trailer.setChanges(changes);
        trailer.setLastInsertId(lastId);
        trailer.setTotalChanges(s.handle->totalChanges());
        trailer.setInTransaction(s.handle->inTransaction());
        trailer.setElapsedUs(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count()));
        trailer.setHandleChanges(s.handle->changes());
        trailer.setHandleLastInsertId(s.handle->lastInsertRowid());
    }

    void compactIfBig(BufsWriter& writer)
    {
        if (writer.capacity() > impl_.o.retainBytes_)
            writer.compact();
    }

    void finish(rpc::ServerContext& ctx, std::uint32_t id, bool commit, Counters::Builder& result)
    {
        const std::shared_ptr<SessionState> hold = session(ctx);
        SessionState& s = *hold;
        const Clock::time_point started = Clock::now();
        ErrorCode outcome = ErrorCode::Ok;
        try {
            Transaction* t = findTransaction(s, id);
            try {
                commit ? t->commit() : t->rollback();
            } catch (...) {
                eraseTransaction(s, id);
                throw;
            }
            eraseTransaction(s, id);
        } catch (const std::exception& e) {
            outcome = finishError(e);
            report(s, commit ? "commit" : "rollback", {}, started, outcome);
            throw detail::toRpcError(e);
        }
        counters(s, result);
        report(s, commit ? "commit" : "rollback", {}, started, outcome);
    }

    static Transaction* findTransaction(SessionState& s, std::uint32_t id)
    {
        for (auto& entry : s.transactions)
            if (entry.first == id)
                return &entry.second;
        throw Error(ErrorCode::BadTransaction, "no such transaction: " + std::to_string(id));
    }

    static Statement* findStatement(SessionState& s, std::uint32_t id)
    {
        for (auto& entry : s.statements)
            if (entry.first == id)
                return &entry.second;
        throw Error(ErrorCode::InvalidArgument, "no such statement: " + std::to_string(id));
    }

    static void eraseTransaction(SessionState& s, std::uint32_t id)
    {
        for (std::size_t i = 0; i < s.transactions.size(); ++i)
            if (s.transactions[i].first == id) {
                s.transactions.erase(s.transactions.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
    }

    BufsOptions optionsFor(rpc::ServerContext& ctx, ResultOptions::Reader r)
    {
        BufsOptions options;
        const std::size_t maxMessage = ctx.maxMessage();
        const std::size_t room = maxMessage > 4096 ? maxMessage - 4096 : 4096;
        const std::size_t bytes = r.batchBytes() ? r.batchBytes() : impl_.o.batchBytes_;
        options.batchBytes(bytes < room ? bytes : room);
        options.batchRows(r.batchRows());
        options.maxRows(static_cast<std::size_t>(r.maxRows()));
        options.countAllRows(r.countAllRows());
        options.blobPrefix(r.blobPrefix());
        options.dictionary(!r.noDictionary());
        options.narrowing(!r.noNarrowing());
        return options;
    }

    /// Fills the session's params (or named) from the request, borrowing text and blob bytes from it.
    static void readParams(SessionState& s, ParamList::Reader request)
    {
        s.params.clear();
        s.named.clear();
        const auto list = request.list();
        bool anyNamed = false, anyPositional = false;
        for (std::uint32_t i = 0; i < list.size(); ++i) {
            const auto p = list[i];
            Value v;
            switch (p.type()) {
                case ParamType::Integer: v = Value(static_cast<std::int64_t>(p.integer())); break;
                case ParamType::Real: v = Value(p.real()); break;
                case ParamType::Datetime: v = Value(Datetime{p.integer()}); break;
                case ParamType::Text:
                    v = Value::textRef(std::string_view(reinterpret_cast<const char*>(p.bytes().data()), p.bytes().size()));
                    break;
                case ParamType::Blob: v = Value::blobRef(p.bytes().data(), p.bytes().size()); break;
                default: break;
            }
            if (p.name().empty()) {
                anyPositional = true;
                s.params.push_back(std::move(v));
            } else {
                anyNamed = true;
                s.named.emplace_back(std::string(p.name()), std::move(v));
            }
        }
        if (anyNamed && anyPositional)
            throw Error(ErrorCode::InvalidArgument, "named and positional parameters cannot be mixed");
        s.usesNamed = anyNamed;
    }

    void execute(rpc::ServerContext& ctx, const char* op, std::string_view text, bool prepared, std::uint32_t statement,
                 ParamList::Reader params, std::uint32_t txnId, ResultOptions::Reader options, std::uint32_t hints,
                 rpc::ServerWriter<sql::wire::ResultSet>& out, StatementEnd::Builder& trailer)
    {
        const std::shared_ptr<SessionState> hold = session(ctx);
        SessionState& s = *hold;
        const Clock::time_point started = Clock::now();
        ErrorCode outcome = ErrorCode::Ok;
        BufsWriter& writer = workerWriter();
        writer.reset();
        writer.options(optionsFor(ctx, options));
        writer.onBatch([&out](BufsBytes b) {
            if (!out.write(b.data(), b.size()))
                throw Abort{};  // the call is over; nobody wants the rest
        });
        Tee tee(writer);
        bool managed = false;
        bool interceptorRan = false;
        // A cancel interrupts the statement at once and the watchdog keeps interrupting it until it is gone; the
        // statement timeout is the watchdog's too. The hook holds a raw pointer: it is cleared (under librpc's lock,
        // so it is not running any more) before this function lets go of the session state.
        Database* handle = s.handle;
        s.run.handle = handle;
        s.run.started = started;
        s.run.cancelled.store(false);
        s.run.timedOut = false;
        SessionState* const state = &s;
        ctx.onCancel([state] {
            state->run.cancelled.store(true);
            state->run.handle->interrupt();
        });
        {
            std::lock_guard<std::mutex> lock(impl_.mutex);
            impl_.running.push_back(&s.run);
        }
        impl_.watchdogWake.notify_one();
        const auto unwatch = [&] {
            std::lock_guard<std::mutex> lock(impl_.mutex);
            for (std::size_t i = 0; i < impl_.running.size(); ++i)
                if (impl_.running[i] == &s.run) {
                    impl_.running.erase(impl_.running.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
        };
        try {
            readParams(s, params);
            Transaction* txn = txnId ? findTransaction(s, txnId) : nullptr;
            managed = !txn && !prepared && impl_.o.interceptor_;
            if (managed) {
                impl_.o.interceptor_->before(*handle, text, hints);
                interceptorRan = true;
            }
            if (prepared) {
                Statement* st = findStatement(s, statement);
                st->clear();
                if (s.usesNamed) {
                    for (auto& [name, value] : s.named)
                        st->bind(std::string_view(name), std::move(value));
                } else {
                    for (std::size_t i = 0; i < s.params.size(); ++i)
                        st->bind(i + 1, std::move(s.params[i]));
                }
                txn ? st->exec(*txn, tee) : st->exec(tee);
            } else if (s.usesNamed) {
                txn ? txn->exec(text, s.named, tee) : handle->exec(text, s.named, tee);
            } else {
                txn ? txn->exec(text, s.params, tee) : handle->exec(text, s.params, tee);
            }
            writer.onBatch({});
            s.params.clear();
            s.named.clear();
            if (managed)
                impl_.o.interceptor_->after(*handle, text, hints, false);
        } catch (...) {
            writer.onBatch({});
            s.params.clear();
            s.named.clear();
            unwatch();
            ctx.onCancel({});
            if (interceptorRan)
                impl_.o.interceptor_->after(*handle, text, hints, true);
            compactIfBig(writer);
            try {
                throw;
            } catch (const Abort&) {
                impl_.interrupts.fetch_add(1);
                report(s, op, text, started, ErrorCode::Interrupted);
                throw rpc::Error(rpc::Code::Cancelled, "interrupted", detail::errorDetail(ErrorCode::Interrupted, Constraint::None));
            } catch (const rpc::Error&) {
                throw;
            } catch (const std::exception& e) {
                outcome = finishError(e);
                if (outcome == ErrorCode::Interrupted)
                    impl_.interrupts.fetch_add(1);
                report(s, op, text, started, outcome);
                throw detail::toRpcError(e);
            }
        }
        unwatch();
        ctx.onCancel({});
        endOf(s, trailer, started, tee.changes, tee.lastInsertId);
        report(s, op, text, started, outcome);
        compactIfBig(writer);
    }

    SqlSocket::Impl& impl_;
};

}  // namespace

// ================================================================================================ SqlSocket ===

namespace {

rpc::EndpointOptions endpointOptions(SqlSocket::Impl& impl)
{
    const SqlSocketOptions& o = impl.o;
    rpc::EndpointOptions e;
    e.serialPerSession().workers(o.workers_).maxSessions(o.maxSessions_).maxInflightPerSession(o.maxPipeline_ + 1);
    e.window(o.window_).idleTimeout(o.idleTimeout_).stallTimeout(o.stallTimeout_).serverName(impl.serverName);
    if (o.maxMessage_)
        e.maxMessage(o.maxMessage_);
    if (o.executor_)
        e.executor(o.executor_);
    if (o.requireAuth_)
        e.requireAuth();
    // The session state is dropped on a worker, never on the loop thread that noticed the connection end.
    e.onSessionClose([](std::uint64_t, std::shared_ptr<void>) {});
    if (o.authorize_) {
        auto authorize = o.authorize_;
        e.authorize([authorize](const rpc::Handshake& h) -> rpc::EndpointOptions::AuthorizeResult {
            Handshake handshake;
            handshake.path = h.path;
            handshake.remoteAddress = h.remoteAddress;
            handshake.local = h.local;
            handshake.pid = h.pid;
            handshake.uid = h.uid;
            handshake.gid = h.gid;
            handshake.header = h.header;
            handshake.routeParam = h.routeParam;
            SqlSocketOptions::AuthorizeResult result = authorize(handshake);
            if (const int* status = std::get_if<int>(&result))
                return *status;
            auto data = std::make_shared<PrincipalData>();
            data->principal = std::move(std::get<Principal>(result));
            rpc::Principal principal;
            principal.name = data->principal.name;
            principal.data = data;
            return principal;
        });
    }
    if (o.authenticate_) {
        auto authenticate = o.authenticate_;
        e.authenticate([authenticate](std::string_view token) -> std::optional<rpc::Principal> {
            std::optional<Principal> who = authenticate(token);
            if (!who)
                return std::nullopt;
            auto data = std::make_shared<PrincipalData>();
            data->principal = std::move(*who);
            rpc::Principal principal;
            principal.name = data->principal.name;
            principal.data = data;
            return principal;
        });
    }
    return e;
}

}  // namespace

SqlSocket::SqlSocket(Database& db, SqlSocketOptions options) : impl_(std::make_shared<Impl>())
{
    impl_->db = &db;
    impl_->o = std::move(options);
    impl_->serverName = impl_->o.serverName_.empty() ? "libsql" : impl_->o.serverName_;
    impl_->service = std::make_unique<Service>(*impl_);
    impl_->endpoint = std::make_unique<rpc::Endpoint>(endpointOptions(*impl_));
    impl_->endpoint->add(*impl_->service);
    impl_->startWatchdog();
}

SqlSocket::~SqlSocket()
{
    if (impl_)
        shutdown(std::chrono::milliseconds(0));
}

void SqlSocket::attach(http::Server& server, std::string pattern)
{
    impl_->endpoint->attach(server, std::move(pattern));
}

void SqlSocket::listenLocal(const std::string& path, unsigned mode)
{
    rpc::LocalOptions options;
    options.mode = mode;
    impl_->endpoint->listenLocal(path, options);
}

void SqlSocket::shutdown(std::chrono::milliseconds grace)
{
    impl_->endpoint->shutdown(grace);
    impl_->stop();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->idleWriters.clear();
    impl_->idleReaders.clear();
}

SqlSocket::Stats SqlSocket::stats() const
{
    const rpc::Endpoint::Stats e = impl_->endpoint->stats();
    Stats s;
    s.sessions = e.sessions;
    s.peakSessions = impl_->peak.load();
    s.sessionsOpened = impl_->opened.load();
    s.sessionsRefused = impl_->refused.load();
    s.requests = e.calls;
    s.errors = impl_->errors.load();
    s.interrupts = impl_->interrupts.load();
    s.running = e.inflight;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    s.idleHandles = impl_->idleWriters.size() + impl_->idleReaders.size();
    return s;
}

}  // namespace sql::remote
