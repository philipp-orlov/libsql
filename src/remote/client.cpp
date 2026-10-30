// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Database::connect: a Database whose work happens on a server. The connection is a librpc client (a WebSocket, or a
// local socket with shared-memory rings) and the calls are the generated SqlClient's (include/sql/wire/remote.bufs).
// Every call runs on the caller's thread and finishes before it returns; rows reach a consumer, a Result or a
// callback through the same ResultBuilder local statements use, so a remote database behaves like a local one to the
// code above it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#include "bufs/bufs.hpp"
#include "errors.hpp"
#include "http/websocket_client.hpp"
#include "internal/params.hpp"
#include "internal/stmt.hpp"
#include "remote/client.hpp"
#include "rpc/client.hpp"
#include "sql/wire/remote.rpc.hpp"
#include "sql/wire/result_view.hpp"

namespace sql::internal {

namespace r = sql::remote;
namespace wire_ = sql::wire;
namespace detail = sql::remote::detail;

namespace {

[[noreturn]] void connectionError(const std::string& what)
{
    throw Error(ErrorCode::Connection, what);
}

/// What a failed connect means to the caller.
[[noreturn]] void connectFailed(const rpc::Error& e)
{
    switch (e.code()) {
        case rpc::Code::Unauthenticated:
        case rpc::Code::PermissionDenied:
            throw Error(ErrorCode::PermissionDenied, std::string("the server refused the connection: ") + e.what());
        default: connectionError(e.what());
    }
}

void encodeValue(r::Param::Builder p, const Value& v)
{
    switch (v.type()) {
        case Type::Integer:
            p.setType(r::ParamType::Integer);
            p.setInteger(v.integer());
            break;
        case Type::Real:
            p.setType(r::ParamType::Real);
            p.setReal(v.real());
            break;
        case Type::Datetime:
            p.setType(r::ParamType::Datetime);
            p.setInteger(v.datetime().usec);
            break;
        case Type::Text: {
            p.setType(r::ParamType::Text);
            const std::string_view t = v.text();
            p.setBytes(reinterpret_cast<const std::uint8_t*>(t.data()), t.size());
            break;
        }
        case Type::Blob: {
            p.setType(r::ParamType::Blob);
            const BlobView b = v.blob();
            p.setBytes(reinterpret_cast<const std::uint8_t*>(b.data()), b.size());
            break;
        }
        default: break;
    }
}

/// The parameters of one call, as the lambdas that fill the request borrow them. Kept in one struct so each lambda
/// captures a single pointer, which std::function stores without allocating.
struct CallArgs
{
    const Value* params = nullptr;
    std::size_t paramCount = 0;
    const NamedParams* named = nullptr;
    const RemoteOptions* options = nullptr;
};

void fillParams(const CallArgs& a, r::ParamList::Builder& b)
{
    if (a.named) {
        if (a.named->empty())
            return;
        auto list = b.initList(a.named->size());
        std::size_t i = 0;
        for (const auto& [name, v] : *a.named) {
            auto p = list.init(i++);
            p.setName(name);
            encodeValue(p, v);
        }
    } else if (a.paramCount) {
        auto list = b.initList(a.paramCount);
        for (std::size_t i = 0; i < a.paramCount; ++i)
            encodeValue(list.init(i), a.params[i]);
    }
}

void fillOptions(const CallArgs& a, r::ResultOptions::Builder& o)
{
    const RemoteOptions& opt = *a.options;
    if (!opt.batchRows() && !opt.batchBytes() && !opt.blobPrefix() && opt.dictionary() && opt.narrowing())
        return;  // all defaults: nothing to say
    o.setBatchRows(static_cast<std::uint32_t>(opt.batchRows()));
    o.setBatchBytes(static_cast<std::uint32_t>(opt.batchBytes()));
    o.setBlobPrefix(static_cast<std::uint32_t>(opt.blobPrefix()));
    o.setNoDictionary(!opt.dictionary());
    o.setNoNarrowing(!opt.narrowing());
}

/// What the closing trailer of a statement said.
struct Ended
{
    std::uint64_t changes = 0;
    std::int64_t lastInsertId = 0, totalChanges = 0, handleChanges = 0, handleLastInsertId = 0;
    bool inTransaction = false;
    std::string sql;
    bool strict = false, withoutRowid = false;
    bool known = false;
};

}  // namespace

// ================================================================================================= session ===

class RemoteSession : public std::enable_shared_from_this<RemoteSession>
{
public:
    RemoteSession(std::string url, RemoteOptions options) : options_(std::move(options)), url_(std::move(url)), path_(url_)
    {
        row_.reserve(32);
    }
    ~RemoteSession() { shutdown(); }

    void open() { connectAndOpen(); }

    // --- calls (all take the call lock; see CallGuard) ---

    Result exec(std::string_view text, const Params* params, const NamedParams* named, RemoteTxn* txn,
                ResultConsumer* consumer, Result* into)
    {
        CallGuard guard(*this);
        checkTxn(txn);
        beginCall(txn != nullptr);
        CallArgs args;
        args.params = params ? params->data() : nullptr;
        args.paramCount = params ? params->size() : 0;
        args.named = named;
        args.options = &options_;
        return run(false, text, 0, txn ? txn->id : 0, args, consumer, into);
    }

    Result execPrepared(RemoteStatement& st, RemoteTxn* txn, ResultConsumer* consumer, Result* into)
    {
        CallGuard guard(*this);
        checkTxn(txn);
        beginCall(txn != nullptr);
        if (st.epoch != epoch_)
            reprepare(st);
        CallArgs args;
        args.params = st.bound.data();
        args.paramCount = st.bound.size();
        args.options = &options_;
        return run(true, {}, st.id, txn ? txn->id : 0, args, consumer, into);
    }

    std::shared_ptr<RemoteStatement> prepare(std::string_view text)
    {
        CallGuard guard(*this);
        beginCall(false);
        auto st = std::make_shared<RemoteStatement>();
        st->session = shared_from_this();
        st->id = nextStatement_++;
        st->info.sql = std::string(text);
        sendPrepare(*st);
        st->bound.assign(st->info.parameters, Value());
        return st;
    }

    std::shared_ptr<RemoteTxn> begin(bool readOnly)
    {
        CallGuard guard(*this);
        beginCall(false);
        auto txn = std::make_shared<RemoteTxn>();
        txn->session = shared_from_this();
        txn->id = nextTxn_++;
        record(call([&] { return sql_->Begin(txn->id, readOnly, callOptions()); }));
        txn->open = true;
        txn->epoch = epoch_;
        ++openTxns_;
        return txn;
    }

    void finishTxn(RemoteTxn& txn, bool commit)
    {
        CallGuard guard(*this);
        checkTxn(&txn);
        beginCall(true);
        txn.open = false;
        --openTxns_;
        record(call([&] { return commit ? sql_->Commit(txn.id, callOptions()) : sql_->Rollback(txn.id, callOptions()); }));
    }

    /// From destructors: the server rolls the transaction back; nothing waits for the answer.
    void abandonTxn(RemoteTxn& txn) noexcept
    {
        if (!txn.open)
            return;
        txn.open = false;
        if (txn.epoch != epoch_)
            return;  // it went with its connection
        --openTxns_;
        try {
            bufs::MessageBuilder mb;
            mb.initRoot<r::SqlRollbackRequest>().setTxn(txn.id);
            const auto sealed = mb.finish();
            rpc_.notify(r::SqlMethods::Rollback, sealed.first, sealed.second);
        } catch (...) {
        }
    }

    void closeStatement(RemoteStatement& st) noexcept
    {
        if (st.epoch != epoch_)
            return;
        try {
            bufs::MessageBuilder mb;
            mb.initRoot<r::SqlClosePreparedRequest>().setStatement(st.id);
            const auto sealed = mb.finish();
            rpc_.notify(r::SqlMethods::ClosePrepared, sealed.first, sealed.second);
        } catch (...) {
        }
    }

    std::vector<std::string> tables()
    {
        CallGuard guard(*this);
        beginCall(false);
        Result rows = describe(r::Catalog::Tables, {}, nullptr);
        std::vector<std::string> out;
        for (const Row& row : rows)
            out.emplace_back(row[0].text());
        return out;
    }

    TableInfo table(std::string_view name)
    {
        CallGuard guard(*this);
        beginCall(false);
        Ended info;
        Result rows = describe(r::Catalog::Table_, name, &info);
        TableInfo out;
        out.name = std::string(name);
        out.sql = info.sql;
        out.strict = info.strict;
        out.withoutRowid = info.withoutRowid;
        std::vector<std::pair<std::int64_t, int>> key;
        int index = 0;
        for (const Row& row : rows) {
            ColumnInfo c;
            c.name = std::string(row[0].text());
            c.type = static_cast<Type>(row[1].integer());
            c.declaredType = std::string(row[2].text());
            c.affinity = std::string(row[3].text());
            c.collation = std::string(row[4].text());
            c.primaryKey = row[5].integer() != 0;
            c.notNull = row[6].integer() != 0;
            c.unique = row[7].integer() != 0;
            c.hasDefault = row[8].integer() != 0;
            c.defaultValue = row[9];
            c.defaultSql = std::string(row[10].text());
            c.generated = static_cast<int>(row[11].integer());
            if (row[12].integer() > 0)
                key.emplace_back(row[12].integer(), index);
            out.columns.push_back(std::move(c));
            ++index;
        }
        std::sort(key.begin(), key.end());
        for (const auto& k : key)
            out.primaryKey.push_back(k.second);
        return out;
    }

    std::vector<IndexInfo> indexes(std::string_view table)
    {
        CallGuard guard(*this);
        beginCall(false);
        Result rows = describe(r::Catalog::Indexes, table, nullptr);
        std::vector<IndexInfo> out;
        for (const Row& row : rows) {
            const std::string name(row[0].text());
            if (out.empty() || out.back().name != name) {
                IndexInfo ix;
                ix.name = name;
                ix.table = std::string(row[1].text());
                ix.unique = row[2].integer() != 0;
                ix.partial = row[3].integer() != 0;
                ix.sql = std::string(row[4].text());
                out.push_back(std::move(ix));
            }
            out.back().columns.emplace_back(row[6].text());
        }
        return out;
    }

    std::shared_ptr<RemoteSession> sibling(bool readOnly)
    {
        RemoteOptions options = options_;
        options.readOnly(readOnly || options_.isReadOnly());
        auto other = std::make_shared<RemoteSession>(url_, std::move(options));
        other->open();
        return other;
    }

    /// From another thread: asks the server to stop whatever this session is running.
    void interrupt() noexcept { rpc_.cancelAll(); }

    void shutdown() noexcept
    {
        if (closed_.exchange(true))
            return;
        rpc_.close();  // says goodbye, then closes
    }

    void close() noexcept
    {
        std::unique_lock<std::mutex> lock(callMutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            interrupt();
            lock.lock();
        }
        shutdown();
    }

    const std::filesystem::path& path() const { return path_; }
    std::int64_t changes() const { return changes_.load(); }
    std::int64_t totalChanges() const { return totalChanges_.load(); }
    std::int64_t lastInsertRowid() const { return lastInsertRowid_.load(); }
    bool inTransaction() const { return inTransaction_.load(); }

private:
    // ----------------------------------------------------------------------------------- locking ---

    struct CallGuard
    {
        explicit CallGuard(RemoteSession& s) : session(s)
        {
            if (s.owner_.load() == std::this_thread::get_id())
                throw Error(ErrorCode::InvalidArgument,
                            "a remote database cannot be used from inside its own row callback");
            lock = std::unique_lock<std::mutex>(s.callMutex_);
            s.owner_ = std::this_thread::get_id();
        }
        ~CallGuard() { session.owner_ = std::thread::id(); }
        RemoteSession& session;
        std::unique_lock<std::mutex> lock;
    };

    rpc::CallOptions callOptions() const
    {
        rpc::CallOptions c;
        c.timeout = options_.requestTimeout();
        return c;
    }

    /// Runs one rpc call and turns its failure into the sql::Error the server raised (or a connection error).
    template <class Fn>
    auto call(Fn&& fn) -> decltype(fn())
    {
        try {
            return fn();
        } catch (const rpc::Error& e) {
            throw detail::fromRpcStatus(e.status());
        }
    }

    // ----------------------------------------------------------------------------------- connecting ---

    void connectAndOpen()
    {
        const std::string_view url = url_;
        const bool local = url.rfind("unix://", 0) == 0 || url.rfind("ipc://", 0) == 0;
        if (!local) {
            if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0)
                throw Error(ErrorCode::InvalidArgument, "a remote database URL starts with ws://, wss:// or unix://");
            try {
                http::parseWebSocketUrl(url);
            } catch (const http::WebSocketError& e) {
                throw Error(ErrorCode::InvalidArgument, e.what());
            }
        }
        rpc::ClientOptions co;
        co.token = options_.token();
        if (!options_.authorization().empty())
            co.headers.emplace_back("Authorization", options_.authorization());
        for (const auto& h : options_.headers())
            co.headers.push_back(h);
        co.peer = options_.clientName().empty() ? std::string("libsql/") + version() + " C++" : options_.clientName();
        co.connectTimeout = options_.connectTimeout();
        co.pingInterval = options_.pingInterval();
        co.window = options_.window();
        co.maxMessage = options_.maxMessage();
        co.caFile = options_.caFile();
        co.insecureSkipVerify = options_.isInsecure();
        try {
            rpc_ = rpc::Client::connect(url_, co);
        } catch (const rpc::Error& e) {
            connectFailed(e);
        }
        sql_ = std::make_unique<r::SqlClient>(rpc_);
        try {
            call([&] { return sql_->Open(options_.isReadOnly(), co.peer, 0, callOptions()); });
        } catch (...) {
            rpc_.close();
            throw;
        }
        ++epoch_;
        inTransaction_ = false;
    }

    /// A transaction can be used while it is open and its connection is the current one.
    void checkTxn(const RemoteTxn* txn) const
    {
        if (!txn)
            return;
        if (!txn->open)
            throw Error(ErrorCode::BadTransaction, "the transaction has already finished");
        if (txn->epoch != epoch_)
            throw Error(ErrorCode::BadTransaction, "the transaction was lost with its connection");
    }

    /// Before a call: the connection must be there. A connection the server closed while idle is replaced,
    /// unless something that lived on it (a transaction) would be lost.
    void beginCall(bool inTransaction)
    {
        if (closed_.load())
            throw Error(ErrorCode::InvalidArgument, "the database is closed");
        if (!rpc_.connected()) {
            const bool lostWork = openTxns_ > 0 || inTransaction_.load() || inTransaction;
            if (!options_.reconnects() || lostWork) {
                // What lived on the connection is gone: later calls start from a clean slate.
                ++epoch_;
                openTxns_ = 0;
                inTransaction_ = false;
                connectionError(lostWork ? "the connection was lost; the transaction was rolled back"
                                         : "the connection was lost");
            }
            openTxns_ = 0;
            connectAndOpen();
        }
    }

    // ----------------------------------------------------------------------------------- receiving ---

    /// Where the rows of a request go.
    struct RowSink
    {
        ResultBuilder* builder = nullptr;
        bool begun = false;
        bool hadColumns = false;
        bool stopped = false;  ///< the consumer asked to stop; the call was cancelled
    };

    /// One ResultSet batch into the builder.
    void feed(const rpc::Payload& payload, RowSink& sink)
    {
        for (auto& s : slots_)
            s.clear();
        auto batch = wire_::BatchView::open(payload.data(), payload.size(), &slots_);
        if (!batch)
            throw Error(ErrorCode::Protocol, "the server sent rows that do not verify");
        ResultBuilder& rb = *sink.builder;
        const std::size_t columns = batch->columnCount();
        if (!sink.begun) {
            sink.begun = true;
            const auto infos = batch->reader().columns();
            if (infos.size() > 0) {
                sink.hadColumns = true;
                adoptColumns(infos);
                rb.declTypes = &declTypes_;
                rb.columns(columns_);
            }
        }
        if (columns == 0)
            return;
        const std::size_t rows = batch->rowCount();
        if (row_.size() < columns)
            row_.resize(columns);
        for (std::size_t c = 0; c < columns; ++c)
            cols_[c] = batch->column(c);
        for (std::size_t i = 0; i < rows; ++i) {
            for (std::size_t c = 0; c < columns; ++c)
                row_[c] = cols_[c].value(i);  // a move: the cell keeps borrowing the frame
            rb.row(row_.data(), columns);
            if (rb.stopped) {
                sink.stopped = true;
                return;
            }
        }
    }

    /// The first batch's column descriptions; the shared ColumnNames is kept while they stay the same.
    template <class Infos>
    void adoptColumns(const Infos& infos)
    {
        const std::size_t n = infos.size();
        bool anyQualified = false;
        for (std::size_t i = 0; i < n && !anyQualified; ++i)
            anyQualified = infos[i].hasQualified();
        bool same = columns_ && columns_->names.size() == n && columns_->qualified.empty() == !anyQualified;
        for (std::size_t i = 0; same && i < n; ++i) {
            const auto info = infos[i];
            same = columns_->names[i] == info.name() && (!anyQualified || columns_->qualified[i] == info.qualified());
        }
        declTypes_.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            declTypes_[i].assign(infos[i].declType());
        if (!same) {
            auto fresh = std::make_shared<ColumnNames>();
            for (std::size_t i = 0; i < n; ++i) {
                fresh->names.emplace_back(infos[i].name());
                if (anyQualified)
                    fresh->qualified.emplace_back(infos[i].qualified());
            }
            columns_ = std::move(fresh);
        }
        if (cols_.size() < n)
            cols_.resize(n, wire_::ColumnView(sql::wire::Column::Reader(), 0, nullptr));
    }

    /// Reads a statement's stream to its end (or until the consumer stops it). Returns how it ended.
    template <class Stream>
    Ended drain(Stream& stream, RowSink& sink)
    {
        Ended ended;
        try {
            while (stream.next()) {
                feed(stream.payload(), sink);
                if (sink.stopped) {
                    // The consumer has had enough: the server is told to stop; the rest is dropped.
                    stream.cancel();
                    return ended;
                }
            }
        } catch (const rpc::Error& e) {
            throw detail::fromRpcStatus(e.status());
        }
        const auto t = stream.trailer();
        ended.known = true;
        ended.changes = t.changes();
        ended.lastInsertId = t.lastInsertId();
        ended.totalChanges = t.totalChanges();
        ended.inTransaction = t.inTransaction();
        ended.handleChanges = t.handleChanges();
        ended.handleLastInsertId = t.handleLastInsertId();
        ended.sql.assign(t.sql());
        ended.strict = t.strict();
        ended.withoutRowid = t.withoutRowid();
        return ended;
    }

    // ----------------------------------------------------------------------------------- requests ---

    Result run(bool prepared, std::string_view text, std::uint32_t statement, std::uint32_t txn, const CallArgs& args,
               ResultConsumer* consumer, Result* into)
    {
        std::optional<ResultBuilder> built;  // not movable: constructed where it lives
        if (into)
            built.emplace(*into);
        else
            built.emplace();
        ResultBuilder& rb = *built;
        rb.consumer = consumer;
        RowSink sink;
        sink.builder = &rb;
        // Each lambda captures one pointer, so std::function keeps it in place instead of allocating.
        const CallArgs* a = &args;
        const rpc::Fill<r::ParamList> params = [a](r::ParamList::Builder& b) { fillParams(*a, b); };
        const rpc::Fill<r::ResultOptions> options = [a](r::ResultOptions::Builder& o) { fillOptions(*a, o); };
        Ended ended;
        try {
            if (prepared) {
                auto stream = sql_->ExecPrepared(statement, params, txn, options, callOptions());
                ended = drain(stream, sink);
            } else {
                auto stream = sql_->Exec(text, params, txn, options, 0, callOptions());
                ended = drain(stream, sink);
            }
        } catch (const rpc::Error& e) {
            throw detail::fromRpcStatus(e.status());
        }
        if (ended.known)
            record(ended);
        rb.changed(ended.changes);
        rb.lastInsertId(ended.lastInsertId);
        // A statement without columns gives the consumer its end() and nothing before it, as locally.
        if (consumer)
            consumer->end(ended.changes, ended.lastInsertId);
        if (into)
            return Result();
        return std::move(rb.own);
    }

    void record(const Ended& t)
    {
        changes_ = t.handleChanges;
        lastInsertRowid_ = t.handleLastInsertId;
        totalChanges_ = t.totalChanges;
        inTransaction_ = t.inTransaction;
    }

    template <class Response>
    void record(const Response& counters)
    {
        changes_ = counters->changes();
        lastInsertRowid_ = counters->lastInsertId();
        totalChanges_ = counters->totalChanges();
        inTransaction_ = counters->inTransaction();
    }

    void sendPrepare(RemoteStatement& st)
    {
        auto info = call([&] { return sql_->Prepare(st.id, st.info.sql, callOptions()); });
        st.info.parameters = info->parameters();
        st.info.names.clear();
        for (auto name : info->parameterNames())
            st.info.names.emplace_back(name);
        st.info.names.resize(st.info.parameters);
        st.info.columns.clear();
        for (auto column : info->columns())
            st.info.columns.emplace_back(column);
        st.info.writes = info->writes();
        st.epoch = epoch_;
    }

    /// The connection was replaced since this statement was prepared: prepare it again under its id.
    void reprepare(RemoteStatement& st)
    {
        const std::size_t before = st.info.parameters;
        sendPrepare(st);
        if (st.info.parameters != before)
            st.bound.resize(st.info.parameters);
    }

    /// Describe: the rows as a Result; `ended` (if given) gets the closing trailer.
    Result describe(r::Catalog what, std::string_view name, Ended* ended)
    {
        ResultBuilder rb;
        RowSink sink;
        sink.builder = &rb;
        CallArgs args;
        args.options = &options_;
        const CallArgs* a = &args;
        const rpc::Fill<r::ResultOptions> options = [a](r::ResultOptions::Builder& o) { fillOptions(*a, o); };
        Ended result;
        try {
            auto stream = sql_->Describe(what, name, options, callOptions());
            result = drain(stream, sink);
        } catch (const rpc::Error& e) {
            throw detail::fromRpcStatus(e.status());
        }
        if (result.known)
            record(result);
        if (ended)
            *ended = std::move(result);
        return std::move(rb.own);
    }

    // ----------------------------------------------------------------------------------- state ---

    RemoteOptions options_;
    std::string url_;
    std::filesystem::path path_;
    rpc::Client rpc_;
    std::unique_ptr<r::SqlClient> sql_;
    std::mutex callMutex_;
    std::atomic<std::thread::id> owner_{};
    std::atomic<bool> closed_{false};
    std::uint32_t nextStatement_ = 1, nextTxn_ = 1;
    std::uint64_t epoch_ = 0;
    std::atomic<int> openTxns_{0};
    std::atomic<std::int64_t> changes_{0}, totalChanges_{0}, lastInsertRowid_{0};
    std::atomic<bool> inTransaction_{false};
    // Reused for every result.
    std::vector<Value> row_;
    std::vector<wire_::ColumnView> cols_;
    std::vector<std::string> declTypes_;
    std::shared_ptr<ColumnNames> columns_;
    std::vector<std::vector<std::uint32_t>> slots_;
};

// ============================================================================================== destructors ===

RemoteTxn::~RemoteTxn()
{
    if (open && session)
        session->abandonTxn(*this);
}

RemoteStatement::~RemoteStatement()
{
    if (session)
        session->closeStatement(*this);
}

// ============================================================================================== free functions ===

std::shared_ptr<RemoteSession> remoteConnect(std::string_view url, const RemoteOptions& options)
{
    auto session = std::make_shared<RemoteSession>(std::string(url), options);
    session->open();
    return session;
}

const std::filesystem::path& remotePath(const RemoteSession& session)
{
    return session.path();
}

std::shared_ptr<RemoteSession> remoteSibling(RemoteSession& session, bool readOnly)
{
    return session.sibling(readOnly);
}

Result remoteExec(RemoteSession& session, std::string_view text, const Params* params, const NamedParams* named,
                  RemoteTxn* txn, ResultConsumer* consumer, Result* into)
{
    return session.exec(text, params, named, txn, consumer, into);
}

std::shared_ptr<RemoteStatement> remotePrepare(const std::shared_ptr<RemoteSession>& session, std::string_view text)
{
    return session->prepare(text);
}

std::shared_ptr<RemoteTxn> remoteBegin(const std::shared_ptr<RemoteSession>& session, bool readOnly)
{
    return session->begin(readOnly);
}

std::int64_t remoteChanges(const RemoteSession& session) { return session.changes(); }
std::int64_t remoteTotalChanges(const RemoteSession& session) { return session.totalChanges(); }
std::int64_t remoteLastInsertRowid(const RemoteSession& session) { return session.lastInsertRowid(); }
bool remoteInTransaction(const RemoteSession& session) { return session.inTransaction(); }
void remoteInterrupt(RemoteSession& session) noexcept { session.interrupt(); }
std::vector<std::string> remoteTables(RemoteSession& session) { return session.tables(); }
TableInfo remoteTable(RemoteSession& session, std::string_view name) { return session.table(name); }
std::vector<IndexInfo> remoteIndexes(RemoteSession& session, std::string_view table) { return session.indexes(table); }
void remoteClose(RemoteSession& session) noexcept { session.close(); }
void remoteFinishTxn(RemoteTxn& txn, bool commit) { txn.session->finishTxn(txn, commit); }
void remoteAbandonTxn(RemoteTxn& txn) noexcept
{
    if (txn.session)
        txn.session->abandonTxn(txn);
}
Result remoteExecStatement(RemoteStatement& statement, RemoteTxn* txn, ResultConsumer* consumer, Result* into)
{
    return statement.session->execPrepared(statement, txn, consumer, into);
}
void remoteCloseStatement(RemoteStatement& statement) noexcept
{
    if (statement.session)
        statement.session->closeStatement(statement);
}

}  // namespace sql::internal
