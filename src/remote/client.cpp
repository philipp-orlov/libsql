// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Database::connect: a Database whose work happens on a server (sql-remote.md, section 7). The session has no
// thread of its own; every call runs on the caller's thread, writes its request, and reads frames until the
// request's terminal reply. Rows reach a consumer, a Result or a callback through the same ResultBuilder local
// statements use, so a remote database behaves like a local one to the code above it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#include "bufs/bufs.hpp"
#include "http/websocket_client.hpp"
#include "internal/params.hpp"
#include "internal/stmt.hpp"
#include "remote/client.hpp"
#include "remote/protocol.hpp"
#include "sql/wire/remote.bufs.hpp"
#include "sql/wire/result_view.hpp"

namespace sql::internal {

using Clock = std::chrono::steady_clock;
namespace r = sql::remote;
namespace wire_ = sql::wire;
namespace detail = sql::remote::detail;

namespace {

/// What a terminal reply said; strings are assigned in place so a warm session reuses their buffers.
struct Terminal
{
    r::Kind kind = r::Kind::None;
    std::uint16_t code = 0;
    std::uint8_t constraint = 0;
    std::string message;
    std::uint64_t changes = 0;
    std::int64_t lastInsertId = 0;
    std::int64_t totalChanges = 0;
    bool inTransaction = false;
    std::uint64_t elapsedUs = 0;
    // Prepared
    std::uint32_t parameters = 0;
    std::vector<std::string> names, columns;
    bool writes = false;
    // Welcome
    std::uint16_t protocol = 0;
    std::uint64_t window = 0;
    std::uint32_t maxMessage = 0;
    std::uint16_t maxPipeline = 0;
    bool readOnly = false;
    // Describe Table
    std::string sql;
    bool strict = false, withoutRowid = false;
    std::int64_t handleChanges = 0, handleLastInsertId = 0;
};

[[noreturn]] void connectionError(const std::string& what)
{
    throw Error(ErrorCode::Connection, what);
}

}  // namespace

// ================================================================================================= session ===

class RemoteSession : public std::enable_shared_from_this<RemoteSession>
{
public:
    RemoteSession(std::string url, RemoteOptions options) : options_(std::move(options)), url_(std::move(url)), path_(url_)
    {
        discard_.reserve(16);
        row_.reserve(32);
    }
    ~RemoteSession() { shutdown(false); }

    void open()
    {
        connectSocket();
        hello();
    }

    // --- calls (all take the call lock; see CallGuard) ---

    Result exec(std::string_view text, const Params* params, const NamedParams* named, RemoteTxn* txn,
                ResultConsumer* consumer, Result* into)
    {
        CallGuard guard(*this);
        checkTxn(txn);
        beginCall(txn != nullptr);
        return run(r::Op::Exec, text, 0, txn ? txn->id : 0, params ? params->data() : nullptr, params ? params->size() : 0,
                   named, consumer, into);
    }

    Result execPrepared(RemoteStatement& st, RemoteTxn* txn, ResultConsumer* consumer, Result* into)
    {
        CallGuard guard(*this);
        checkTxn(txn);
        beginCall(txn != nullptr);
        if (st.epoch != epoch_)
            reprepare(st);
        return run(r::Op::ExecPrepared, {}, st.id, txn ? txn->id : 0, st.bound.data(), st.bound.size(), nullptr, consumer, into);
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
        const std::uint32_t id = send(r::Op::Begin, [&](r::Request::Builder q) {
            q.setTxn(txn->id);
            q.setReadOnly(readOnly);
        });
        awaitTerminal(id);
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
        const std::uint32_t id = send(commit ? r::Op::Commit : r::Op::Rollback, [&](r::Request::Builder q) { q.setTxn(txn.id); });
        awaitTerminal(id);
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
        fireAndForget(r::Op::Rollback, 0, txn.id);
    }

    void closeStatement(RemoteStatement& st) noexcept
    {
        if (st.epoch == epoch_)
            fireAndForget(r::Op::ClosePrepared, st.id, 0);
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
        Terminal info;
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

    /// From another thread: asks the server to stop the request this session is waiting for.
    void interrupt() noexcept
    {
        const std::uint32_t id = current_.load();
        if (id == 0 || !ws_.isOpen())
            return;
        try {
            alignas(8) unsigned char buffer[128];
            bufs::MessageBuilder mb(buffer, sizeof buffer);
            auto q = mb.initRoot<r::Request>();
            q.setId(interruptId_.fetch_add(1) | 0x80000000u);  // ids of immediate requests are never queued
            q.setOp(r::Op::Interrupt);
            q.setTarget(id);
            const auto [data, size] = mb.finish();
            ws_.sendBinary(data, size);
        } catch (...) {
        }
    }

    void shutdown(bool wait) noexcept
    {
        if (closed_.exchange(true))
            return;
        try {
            if (ws_.isOpen()) {
                if (wait) {
                    const std::uint32_t id = send(r::Op::Goodbye, [](r::Request::Builder) {});
                    try {
                        awaitTerminalBounded(id, std::chrono::seconds(5));
                    } catch (...) {
                    }
                }
                ws_.close();
            }
        } catch (...) {
        }
    }

    void close() noexcept
    {
        std::unique_lock<std::mutex> lock(callMutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            interrupt();
            lock.lock();
        }
        shutdown(true);
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
        ~CallGuard()
        {
            session.owner_ = std::thread::id();
            session.current_ = 0;
        }
        RemoteSession& session;
        std::unique_lock<std::mutex> lock;
    };

    // ----------------------------------------------------------------------------------- connecting ---

    void connectSocket()
    {
        http::WebSocketClientOptions wo;
        wo.subprotocols = {"libsql.v1"};
        wo.connectTimeout = options_.connectTimeout();
        wo.maxMessageBytes = options_.maxMessage();
        if (!options_.authorization().empty())
            wo.headers.emplace_back("Authorization", options_.authorization());
        for (const auto& h : options_.headers())
            wo.headers.push_back(h);
        try {
            ws_.connect(url_, wo);
        } catch (const http::WebSocketError& e) {
            if (e.kind() == http::WebSocketError::Kind::Handshake && (e.status() == 401 || e.status() == 403))
                throw Error(ErrorCode::PermissionDenied, std::string("the server refused the connection: ") + e.what());
            if (e.kind() == http::WebSocketError::Kind::Usage)
                throw Error(ErrorCode::InvalidArgument, e.what());
            connectionError(e.what());
        }
    }

    void hello()
    {
        consumed_ = 0;
        const std::uint32_t id = send(r::Op::Hello, [&](r::Request::Builder q) {
            q.setProtocol(detail::kProtocolVersion);
            q.setClient(options_.clientName().empty() ? std::string("libsql/") + version() + " C++" : options_.clientName());
            if (!options_.token().empty())
                q.setToken(options_.token());
            q.setReadOnly(options_.isReadOnly());
            q.setWindow(options_.window());
            q.setMaxMessage(static_cast<std::uint32_t>(options_.maxMessage()));
        }, /*queued*/ false);
        Terminal t = awaitTerminal(id);
        if (t.kind != r::Kind::Welcome)
            throw Error(ErrorCode::Protocol, "the server did not answer hello with welcome");
        window_ = t.window ? t.window : options_.window();
        maxPipeline_ = t.maxPipeline;
        ++epoch_;
        inTransaction_ = false;
    }

    void reconnect()
    {
        ws_ = http::WebSocketClient();
        connectSocket();
        hello();
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
        flushDeferred();
        if (!ws_.isOpen() || ws_.peerClosed()) {
            const bool lostWork = openTxns_ > 0 || inTransaction_.load() || inTransaction;
            if (!options_.reconnects() || lostWork) {
                // What lived on the connection is gone: later calls start from a clean slate.
                ++epoch_;
                openTxns_ = 0;
                inTransaction_ = false;
                discard_.clear();
                connectionError(lostWork ? "the connection was lost; the transaction was rolled back"
                                         : "the connection was lost");
            }
            discard_.clear();
            openTxns_ = 0;
            reconnect();
        }
        flushCredit();
    }

    // ----------------------------------------------------------------------------------- sending ---

    template <class Fill>
    std::uint32_t send(r::Op op, Fill&& fill, bool queued = true)
    {
        (void)queued;
        std::uint32_t id = nextId_++;
        if (id == 0 || id >= 0x80000000u) {
            nextId_ = 2;
            id = 1;
        }
        auto q = builder_.initRoot<r::Request>();
        q.setId(id);
        q.setOp(op);
        fill(q);
        const auto [data, size] = builder_.finish();
        try {
            ws_.sendBinary(data, size);
        } catch (const http::WebSocketError& e) {
            connectionError(std::string("the connection was lost: ") + e.what());
        }
        return id;
    }

    void flushCredit()
    {
        if (consumed_ == 0 || !ws_.isOpen())
            return;
        const std::uint64_t c = consumed_;
        consumed_ = 0;
        send(r::Op::Credit, [c](r::Request::Builder q) { q.setCredit(c); });
    }

    void fireAndForget(r::Op op, std::uint32_t statement, std::uint32_t txn) noexcept
    {
        try {
            std::unique_lock<std::mutex> lock(callMutex_, std::try_to_lock);
            if (!lock.owns_lock() || owner_.load() == std::this_thread::get_id() || closed_.load() || !ws_.isOpen()) {
                std::lock_guard<std::mutex> g(deferredMutex_);
                deferred_.push_back({op, statement, txn, epoch_});
                return;
            }
            sendFireAndForget(op, statement, txn);
        } catch (...) {
        }
    }

    void sendFireAndForget(r::Op op, std::uint32_t statement, std::uint32_t txn)
    {
        const std::uint32_t id = send(op, [&](r::Request::Builder q) {
            q.setStatement(statement);
            q.setTxn(txn);
        });
        discard_.push_back(id);
    }

    void flushDeferred()
    {
        std::vector<Deferred> work;
        {
            std::lock_guard<std::mutex> g(deferredMutex_);
            work.swap(deferred_);
        }
        for (const Deferred& d : work)
            if (d.epoch == epoch_ && ws_.isOpen())
                sendFireAndForget(d.op, d.statement, d.txn);
    }

    // ----------------------------------------------------------------------------------- receiving ---

    /// Waits for the terminal reply of `id`, discarding the answers of fire-and-forget requests; rows are an
    /// error here (none are expected).
    Terminal awaitTerminal(std::uint32_t id)
    {
        RowSink none;
        return await(id, none, Clock::time_point::max());
    }
    void awaitTerminalBounded(std::uint32_t id, std::chrono::seconds limit)
    {
        RowSink none;
        await(id, none, Clock::now() + limit);
    }

    /// Where the rows of a request go.
    struct RowSink
    {
        ResultBuilder* builder = nullptr;
        ResultConsumer* consumer = nullptr;
        bool begun = false;
        bool hadColumns = false;
        bool stopSent = false;
        bool swallowInterrupt = false;
        bool terminal = false;  ///< the request's closing reply has been read
    };

    /// A request left without its closing reply (a consumer threw, say): the server is told to stop it and
    /// whatever it still sends for that id is read and dropped by the next calls.
    void abandon(std::uint32_t id) noexcept
    {
        try {
            current_ = id;
            interrupt();
            discard_.push_back(id);
        } catch (...) {
        }
    }

    Terminal await(std::uint32_t id, RowSink& sink, Clock::time_point hardLimit)
    {
        current_ = id;
        const bool limited = options_.requestTimeout().count() > 0;
        Clock::time_point deadline = limited ? Clock::now() + options_.requestTimeout() : Clock::time_point::max();
        deadline = std::min(deadline, hardLimit);
        bool timedOut = false;
        for (;;) {
            std::optional<http::WebSocketClient::Message> message;
            try {
                message = ws_.receive(deadline, options_.pingInterval());
            } catch (const http::WebSocketError& e) {
                if (e.kind() == http::WebSocketError::Kind::Closed)
                    connectionError("the server closed the connection (" + std::to_string(e.closeCode()) + " " +
                                    e.closeReason() + ")");
                if (e.kind() == http::WebSocketError::Kind::TooBig)
                    throw Error(ErrorCode::Protocol, "the server sent a message above the configured maximum");
                connectionError(e.what());
            }
            if (!message) {
                // The deadline passed. First ask the server to stop; if even that is not answered, give up.
                if (hardLimit != Clock::time_point::max() && Clock::now() >= hardLimit)
                    connectionError("no answer from the server");
                if (timedOut)
                    connectionError("no answer from the server after the request was interrupted");
                timedOut = true;
                sink.swallowInterrupt = false;
                interrupt();
                deadline = Clock::now() + std::chrono::seconds(5);
                continue;
            }
            const char* data = message->data.data();
            const std::size_t size = message->data.size();
            if (size < 8 || !message->binary || !bufs::verify<r::Reply>(data, size))
                throw Error(ErrorCode::Protocol, "the server sent a message that is not a reply");
            const auto reply = bufs::root<r::Reply>(data);
            const std::uint32_t replyId = reply.id();
            const r::Kind kind = reply.kind();
            if (kind == r::Kind::Closing)
                connectionError("the server is shutting down");
            if (replyId != id) {
                auto it = std::find(discard_.begin(), discard_.end(), replyId);
                if (it != discard_.end()) {
                    if (kind == r::Kind::Rows) {
                        consumed_ += detail::frameCost(size);  // dropped, but still acknowledged
                        if (consumed_ >= window_ / 2)
                            flushCredit();
                    } else {
                        discard_.erase(it);
                    }
                    continue;
                }
                throw Error(ErrorCode::Protocol, "the server sent a reply for an unknown request");
            }
            if (kind == r::Kind::Rows) {
                consumed_ += detail::frameCost(size);
                feed(data + bufs::messageSize(data), size - bufs::messageSize(data), sink, id);
                if (consumed_ >= window_ / 2)
                    flushCredit();
                continue;
            }
            Terminal t;
            readTerminal(reply, t);
            sink.terminal = true;
            if (t.kind == r::Kind::Error) {
                if (sink.swallowInterrupt && t.code == detail::wireCode(ErrorCode::Interrupted)) {
                    t.kind = r::Kind::Done;  // the consumer asked for the stop
                    t.message.clear();
                    return t;
                }
                throw Error(detail::errorCodeFrom(t.code), static_cast<Constraint>(t.constraint), t.message);
            }
            return t;
        }
    }

    static void readTerminal(const r::Reply::Reader& reply, Terminal& t)
    {
        t.kind = reply.kind();
        t.code = reply.code();
        t.constraint = reply.constraint();
        t.message.assign(reply.message());
        t.changes = reply.changes();
        t.lastInsertId = reply.lastInsertId();
        t.totalChanges = reply.totalChanges();
        t.inTransaction = reply.inTransaction();
        t.elapsedUs = reply.elapsedUs();
        t.parameters = reply.parameters();
        t.writes = reply.writes();
        t.protocol = reply.protocol();
        t.window = reply.window();
        t.maxMessage = reply.maxMessage();
        t.maxPipeline = reply.maxPipeline();
        t.readOnly = reply.readOnly();
        t.sql.assign(reply.sql());
        t.strict = reply.strict();
        t.withoutRowid = reply.withoutRowid();
        t.handleChanges = reply.handleChanges();
        t.handleLastInsertId = reply.handleLastInsertId();
        t.names.clear();
        for (auto s : reply.parameterNames())
            t.names.emplace_back(s);
        t.columns.clear();
        for (auto s : reply.columns())
            t.columns.emplace_back(s);
    }

    /// One Rows frame: its ResultSet message into the builder.
    void feed(const char* messageData, std::size_t messageSize, RowSink& sink, std::uint32_t id)
    {
        if (!sink.builder) {
            // Rows nobody asked for (a Prepare that returned some?): protocol violation.
            throw Error(ErrorCode::Protocol, "the server sent rows for a request that has none");
        }
        if (sink.stopSent)
            return;  // the consumer stopped; the rest is read and dropped
        for (auto& s : slots_)
            s.clear();
        auto batch = wire_::BatchView::open(reinterpret_cast<const std::uint8_t*>(messageData), messageSize, &slots_);
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
                sink.stopSent = true;
                sink.swallowInterrupt = true;
                const std::uint32_t target = id;
                current_ = target;
                interrupt();
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

    // ----------------------------------------------------------------------------------- requests ---

    Result run(r::Op op, std::string_view text, std::uint32_t statement, std::uint32_t txn, const Value* params,
               std::size_t count, const NamedParams* named, ResultConsumer* consumer, Result* into)
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
        sink.consumer = consumer;
        const std::uint32_t id = send(op, [&](r::Request::Builder q) {
            if (!text.empty())
                q.setSql(text);
            q.setStatement(statement);
            q.setTxn(txn);
            encodeParams(q, params, count, named);
            encodeOptions(q);
        });
        Terminal t;
        try {
            t = await(id, sink, Clock::time_point::max());
        } catch (...) {
            if (!sink.terminal && ws_.isOpen())
                abandon(id);
            current_ = 0;
            throw;
        }
        current_ = 0;
        record(t, sink.hadColumns);
        rb.changed(t.changes);
        rb.lastInsertId(t.lastInsertId);
        // A statement without columns gives the consumer its end() and nothing before it, as locally.
        if (consumer)
            consumer->end(t.changes, t.lastInsertId);
        if (into)
            return Result();
        return std::move(rb.own);
    }

    void record(const Terminal& t, bool /*hadColumns*/)
    {
        changes_ = t.handleChanges;
        lastInsertRowid_ = t.handleLastInsertId;
        totalChanges_ = t.totalChanges;
        inTransaction_ = t.inTransaction;
    }

    void encodeOptions(r::Request::Builder& q)
    {
        if (!options_.batchRows() && !options_.batchBytes() && !options_.blobPrefix() && options_.dictionary() &&
            options_.narrowing())
            return;
        auto o = q.initOptions();
        o.setBatchRows(static_cast<std::uint32_t>(options_.batchRows()));
        o.setBatchBytes(static_cast<std::uint32_t>(options_.batchBytes()));
        o.setBlobPrefix(static_cast<std::uint32_t>(options_.blobPrefix()));
        o.setNoDictionary(!options_.dictionary());
        o.setNoNarrowing(!options_.narrowing());
    }

    static void encodeValue(r::Param::Builder p, const Value& v)
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

    static void encodeParams(r::Request::Builder& q, const Value* params, std::size_t count, const NamedParams* named)
    {
        if (named) {
            if (named->empty())
                return;
            auto list = q.initParams(named->size());
            std::size_t i = 0;
            for (const auto& [name, v] : *named) {
                auto p = list.init(i++);
                p.setName(name);
                encodeValue(p, v);
            }
            return;
        }
        if (count == 0)
            return;
        auto list = q.initParams(count);
        for (std::size_t i = 0; i < count; ++i)
            encodeValue(list.init(i), params[i]);
    }

    void sendPrepare(RemoteStatement& st)
    {
        const std::uint32_t id = send(r::Op::Prepare, [&](r::Request::Builder q) {
            q.setStatement(st.id);
            q.setSql(st.info.sql);
        });
        Terminal t = awaitTerminal(id);
        if (t.kind != r::Kind::Prepared)
            throw Error(ErrorCode::Protocol, "the server did not answer prepare with prepared");
        st.info.parameters = t.parameters;
        st.info.names = std::move(t.names);
        st.info.names.resize(t.parameters);
        st.info.columns = std::move(t.columns);
        st.info.writes = t.writes;
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

    /// Describe: the rows as a Result; `terminal` (if given) gets the closing reply.
    Result describe(r::Catalog what, std::string_view name, Terminal* terminal)
    {
        ResultBuilder rb;
        RowSink sink;
        sink.builder = &rb;
        const std::uint32_t id = send(r::Op::Describe, [&](r::Request::Builder q) {
            q.setWhat(what);
            if (!name.empty())
                q.setName(name);
        });
        Terminal t;
        try {
            t = await(id, sink, Clock::time_point::max());
        } catch (...) {
            if (!sink.terminal && ws_.isOpen())
                abandon(id);
            current_ = 0;
            throw;
        }
        current_ = 0;
        record(t, true);
        if (terminal)
            *terminal = std::move(t);
        return std::move(rb.own);
    }

    // ----------------------------------------------------------------------------------- state ---

    struct Deferred
    {
        r::Op op;
        std::uint32_t statement, txn;
        std::uint64_t epoch;
    };

    RemoteOptions options_;
    std::string url_;
    std::filesystem::path path_;
    http::WebSocketClient ws_;
    bufs::MessageBuilder builder_;
    std::mutex callMutex_;
    std::atomic<std::thread::id> owner_{};
    std::atomic<bool> closed_{false};
    std::atomic<std::uint32_t> current_{0};
    std::atomic<std::uint32_t> interruptId_{1};
    std::uint32_t nextId_ = 1, nextStatement_ = 1, nextTxn_ = 1;
    std::uint64_t window_ = 1 << 20, consumed_ = 0;
    std::uint16_t maxPipeline_ = 16;
    std::uint64_t epoch_ = 0;
    std::atomic<int> openTxns_{0};
    std::atomic<std::int64_t> changes_{0}, totalChanges_{0}, lastInsertRowid_{0};
    std::atomic<bool> inTransaction_{false};
    std::vector<std::uint32_t> discard_;
    std::mutex deferredMutex_;
    std::vector<Deferred> deferred_;
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
