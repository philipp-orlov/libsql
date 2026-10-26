// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// sql::remote::SqlSocket: one WebSocket route that serves sessions of a sql::Database (sql-remote.md, sections 3, 4
// and 6). The rules that keep it safe to run for months:
//   * the loop thread never touches a Database and never waits; requests are copied into pooled slots and run on
//     a worker, one session at a time, in arrival order;
//   * workers never send on the connection; they queue finished frames and ask the loop to flush them;
//   * rows are flow-controlled by credit, so a connection never queues more than a window of rows;
//   * every container is created when a session starts, sized to its limits, and only cleared afterwards;
//   * posted tasks capture one raw pointer (no allocation), with the session kept alive by a pin count.

#include "sql/remote/sql_socket.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bufs/bufs.hpp"
#include "http/thread_pool.hpp"
#include "remote/protocol.hpp"
#include "remote/ring.hpp"
#include "sql/bufs.hpp"
#include "sql/wire/remote.bufs.hpp"

namespace sql::remote {

using Clock = std::chrono::steady_clock;

namespace {

/// Ends a request without a result: the session closed, the request was interrupted, or the client stopped reading.
struct Abort
{
    enum Reason { Closed, Interrupted, Stalled } reason;
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

const char* opName(Op op)
{
    switch (op) {
        case Op::Hello: return "hello";
        case Op::Exec: return "exec";
        case Op::Prepare: return "prepare";
        case Op::ExecPrepared: return "exec_prepared";
        case Op::ClosePrepared: return "close_prepared";
        case Op::Begin: return "begin";
        case Op::Commit: return "commit";
        case Op::Rollback: return "rollback";
        case Op::Interrupt: return "interrupt";
        case Op::Credit: return "credit";
        case Op::Describe: return "describe";
        case Op::Ping: return "ping";
        case Op::State: return "state";
        case Op::Goodbye: return "goodbye";
        default: return "none";
    }
}

}  // namespace

// ============================================================================================ SqlSocket::Impl ===

namespace detail {
class Session;
}

struct SqlSocket::Impl
{
    Database* db = nullptr;
    SqlSocketOptions o;
    std::string serverName;

    // Where worker tasks run.
    std::unique_ptr<http::ThreadPool> ownPool;
    std::function<bool(std::function<void()>)> post;

    // Limits derived from the server this is attached to (see attach()).
    std::size_t window = 1 << 20;
    std::size_t maxMessage = 1 << 20;
    std::size_t routeMax = 0;

    std::atomic<bool> accepting{true};
    std::atomic<std::uint64_t> nextSession{1};
    std::atomic<std::size_t> sessionCount{0}, peak{0};
    std::atomic<std::uint64_t> opened{0}, refused{0}, requests{0}, errors{0}, interrupts{0}, stalls{0};
    std::atomic<std::uint64_t> bytesIn{0}, bytesOut{0};
    std::atomic<std::size_t> running{0}, queued{0};

    std::mutex mutex;
    std::vector<std::weak_ptr<detail::Session>> sessions;
    std::unordered_map<std::string, std::size_t> perAddress;
    std::vector<Database> idleWriters, idleReaders;

    // Worker tasks not finished yet (shutdown waits for them).
    std::atomic<std::size_t> tasks{0};
    std::mutex drainMutex;
    std::condition_variable drained;

    std::thread watchdog;
    std::atomic<bool> stopWatchdog{false};

    void taskDone()
    {
        if (tasks.fetch_sub(1) == 1) {
            std::lock_guard<std::mutex> lock(drainMutex);
            drained.notify_all();
        }
    }
};

namespace detail {

/// A claim on a session slot, released when the connection's user data goes (after it closed).
struct Reservation
{
    std::shared_ptr<SqlSocket::Impl> impl;
    std::string address;
    bool counted = false;  ///< the per-address count was incremented
    ~Reservation()
    {
        impl->sessionCount.fetch_sub(1);
        if (counted) {
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto it = impl->perAddress.find(address);
            if (it != impl->perAddress.end() && --it->second == 0)
                impl->perAddress.erase(it);
        }
    }
};

struct AcceptInfo
{
    Principal principal;
    bool authenticated = false;
    std::shared_ptr<Reservation> reservation;
};

/// One request waiting or running: the frame, kept until the request is done (parameters and SQL text are read
/// from it in place).
struct Slot
{
    http::Buffer bytes;
    std::uint32_t id = 0;
    Op op = Op::None;
    bool cancelled = false;
};

// ============================================================================================== Session ===

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(std::shared_ptr<SqlSocket::Impl> impl, std::shared_ptr<http::WebSocketConnection> ws,
            std::shared_ptr<AcceptInfo> info, http::BufferPool& pool)
        : impl_(std::move(impl)), ws_(std::move(ws)), pool_(&pool), info_(std::move(info)),
          principal_(info_->principal), id_(impl_->nextSession.fetch_add(1)), queue_(impl_->o.maxPipeline_)
    {
        outbox_.reserve(64);
        flushScratch_.reserve(64);
        statements_.reserve(impl_->o.maxStatements_ < 64 ? impl_->o.maxStatements_ : 64);
        transactions_.reserve(impl_->o.maxTransactions_);
    }

    // ----------------------------------------------------------------------------- loop thread ---

    void onFrame(std::string_view payload, bool binary)
    {
        if (closing_.load())
            return;
        impl_->bytesIn.fetch_add(payload.size(), std::memory_order_relaxed);
        if (!binary) {
            fail(1003, ErrorCode::Protocol, 0, "text frames are not part of the protocol");
            return;
        }
        if (payload.size() < 8 || payload.size() % 8 != 0) {
            fail(1002, ErrorCode::Protocol, 0, "a frame must hold a whole libbufs message");
            return;
        }
        http::Buffer bytes(*pool_, payload.size());
        std::memcpy(bytes.data(), payload.data(), payload.size());
        bytes.setSize(payload.size());
        if (!bufs::verify<Request>(bytes.data(), bytes.size())) {
            fail(1002, ErrorCode::Protocol, 0, "not a valid request message");
            return;
        }
        const auto req = bufs::root<Request>(bytes.data());
        const std::uint32_t id = req.id();
        const Op op = req.op();
        if (id == 0) {
            fail(1002, ErrorCode::Protocol, 0, "request id 0 is not allowed");
            return;
        }
        if (!hello_) {
            if (op != Op::Hello) {
                fail(1002, ErrorCode::Protocol, id, "the first request must be hello");
                return;
            }
            hello(req);
            return;
        }
        switch (op) {
            case Op::Hello: fail(1002, ErrorCode::Protocol, id, "hello was already received"); return;
            case Op::Credit: {
                std::lock_guard<std::mutex> lock(mutex_);
                const std::uint64_t c = req.credit();
                outstanding_ -= c < outstanding_ ? c : outstanding_;
                cv_.notify_all();
                return;
            }
            case Op::Interrupt: interrupt(req.target()); return;
            case Op::Ping: {
                detail::ReplyFields f;
                f.id = id;
                f.kind = Kind::Pong;
                sendDirect(f);
                return;
            }
            case Op::Exec:
            case Op::Prepare:
            case Op::ExecPrepared:
            case Op::ClosePrepared:
            case Op::Begin:
            case Op::Commit:
            case Op::Rollback:
            case Op::Describe:
            case Op::State:
            case Op::Goodbye: break;
            default: {
                // An op a newer client may know: refuse it, keep the session.
                detail::ReplyFields f;
                f.id = id;
                f.kind = Kind::Error;
                f.code = detail::wireCode(ErrorCode::Protocol);
                f.message = "unknown request";
                sendDirect(f);
                return;
            }
        }
        if (draining_.load()) {
            refuse(id, ErrorCode::Busy, "the server is shutting down");
            return;
        }
        enum class Verdict { Queued, InUse, Full } verdict = Verdict::Queued;
        bool start = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool inUse = running_ == id;
            for (std::size_t i = 0; i < queue_.size() && !inUse; ++i)
                inUse = queue_.at(i).id == id;
            if (inUse) {
                verdict = Verdict::InUse;
            } else if (queue_.size() + (running_ ? 1 : 0) >= impl_->o.maxPipeline_) {
                verdict = Verdict::Full;
            } else {
                Slot slot;
                slot.bytes = std::move(bytes);
                slot.id = id;
                slot.op = op;
                queue_.push(std::move(slot));
                impl_->queued.fetch_add(1, std::memory_order_relaxed);
                if (!workerActive_) {
                    workerActive_ = true;
                    start = true;
                }
            }
        }
        if (verdict == Verdict::InUse) {
            fail(1002, ErrorCode::Protocol, id, "request id is already in use");
            return;
        }
        if (verdict == Verdict::Full) {
            refuse(id, ErrorCode::Busy, "too many requests in flight");
            return;
        }
        if (start && !postWorker()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                workerActive_ = false;
                while (!queue_.empty()) {
                    queue_.pop();
                    impl_->queued.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            refuse(id, ErrorCode::Busy, "server is busy");
        }
    }

    void onClose()
    {
        bool teardownNow = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
            if (running_ != 0 && handle_)
                handle_->interrupt();
            cv_.notify_all();
            if (!workerActive_) {
                workerActive_ = true;
                teardownNow = true;
            } else {
                teardownPending_ = true;
            }
        }
        if (teardownNow && !postTeardown()) {
            // The executor is gone (shutdown): tear down here, on the loop; nothing can be running.
            teardown();
        }
    }

    void flushOutbox()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            flushScratch_.swap(outbox_);
            flushPosted_ = false;
        }
        for (http::Buffer& frame : flushScratch_) {
            impl_->bytesOut.fetch_add(frame.size(), std::memory_order_relaxed);
            if (!closing_.load())
                ws_->send(std::move(frame), true);
        }
        flushScratch_.clear();
        unpin();
    }

    /// Posted to the loop: close the connection with a code.
    void closeConnection(std::uint16_t code, const char* reason)
    {
        if (!closing_.load())
            ws_->close(code, reason);
    }

    /// Things a worker or another thread asks the loop to do. Posted as (this, action): 16 bytes, trivially
    /// copyable, so the std::function holding it does not allocate.
    enum class Action : int { SendClosing, CloseGoingAway, Goodbye, ClosePolicy };
    void runAction(int action)
    {
        switch (static_cast<Action>(action)) {
            case Action::SendClosing: {
                detail::ReplyFields f;
                f.kind = Kind::Closing;
                sendDirect(f);
                break;
            }
            case Action::CloseGoingAway: closeConnection(1001, "going away"); break;
            case Action::Goodbye: closeConnection(1000, "goodbye"); break;
            case Action::ClosePolicy: closeConnection(1008, "client stopped reading"); break;
        }
    }

    // Shutdown support (any thread).
    void beginDrain()
    {
        draining_ = true;
        postLoop(Action::SendClosing);
    }
    bool idle()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty() && running_ == 0 && outbox_.empty();
    }
    void forceClose()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ != 0 && handle_)
                handle_->interrupt();
            interruptRunning_ = true;
            cv_.notify_all();
        }
        postLoop(Action::CloseGoingAway);
    }

    /// The watchdog's check of the statement timeout.
    void checkTimeout(Clock::time_point now, std::chrono::milliseconds limit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ == 0 || !handle_ || interruptRunning_)
            return;
        if (now - runStarted_ > limit) {
            interruptRunning_ = true;
            handle_->interrupt();
            impl_->interrupts.fetch_add(1);
            cv_.notify_all();
        }
    }

private:
    // ----------------------------------------------------------------------------- tasks & pins ---

    // A posted task captures only `this` (8 trivially copyable bytes: std::function stores it inline). The session
    // stays alive while tasks are pending because pin() holds a shared_ptr to it.
    void pin()
    {
        std::lock_guard<std::mutex> lock(pinMutex_);
        if (pins_++ == 0)
            pinned_ = shared_from_this();
    }
    void unpin()
    {
        std::shared_ptr<Session> last;
        {
            std::lock_guard<std::mutex> lock(pinMutex_);
            if (--pins_ == 0)
                last = std::move(pinned_);
        }
        // `last` may be the final reference: nothing of the session is touched after this.
    }

    bool postWorker()
    {
        pin();
        impl_->tasks.fetch_add(1);
        Session* self = this;
        if (impl_->post([self] { self->runNext(); }))
            return true;
        impl_->taskDone();
        unpin();
        return false;
    }

    bool postTeardown()
    {
        pin();
        impl_->tasks.fetch_add(1);
        Session* self = this;
        if (impl_->post([self] {
                self->teardown();
                self->impl_->taskDone();
                self->unpin();
            }))
            return true;
        impl_->taskDone();
        unpin();
        return false;
    }

    void postLoop(Action action)
    {
        pin();
        Session* self = this;
        const int a = static_cast<int>(action);
        if (!ws_->post([self, a] {
                self->runAction(a);
                self->unpin();
            }))
            unpin();
    }

    // ----------------------------------------------------------------------------- loop-side replies ---

    void sendDirect(const detail::ReplyFields& fields)
    {
        http::Buffer frame = detail::makeReply(*pool_, fields);
        impl_->bytesOut.fetch_add(frame.size(), std::memory_order_relaxed);
        ws_->send(std::move(frame), true);
    }

    void refuse(std::uint32_t id, ErrorCode code, std::string_view message)
    {
        detail::ReplyFields f;
        f.id = id;
        f.kind = Kind::Error;
        f.code = detail::wireCode(code);
        f.message = message;
        sendDirect(f);
        impl_->errors.fetch_add(1);
    }

    /// An error that ends the session.
    void fail(std::uint16_t closeCode, ErrorCode code, std::uint32_t id, std::string_view message)
    {
        refuse(id, code, message);
        closeConnection(closeCode, "protocol error");
    }

    void interrupt(std::uint32_t target)
    {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ == target) {
                interruptRunning_ = true;
                if (handle_)
                    handle_->interrupt();
                cv_.notify_all();
                impl_->interrupts.fetch_add(1);
                return;
            }
            for (std::size_t i = 0; i < queue_.size(); ++i) {
                Slot& s = queue_.at(i);
                if (s.id == target && !s.cancelled) {
                    s.cancelled = true;
                    cancelled = true;
                    break;
                }
            }
        }
        if (cancelled) {
            impl_->interrupts.fetch_add(1);
            refuse(target, ErrorCode::Interrupted, "interrupted");
        }
    }

    void hello(const Request::Reader& req)
    {
        const std::uint32_t id = req.id();
        if (req.protocol() != detail::kProtocolVersion) {
            fail(1002, ErrorCode::Protocol, id, "unsupported protocol version");
            return;
        }
        bool authenticated = info_->authenticated;
        if (!authenticated && !req.token().empty() && impl_->o.authenticate_) {
            std::optional<Principal> who;
            try {
                who = impl_->o.authenticate_(req.token());
            } catch (...) {
            }
            if (who) {
                principal_ = std::move(*who);
                authenticated = true;
            }
        }
        if (impl_->o.requireAuth_ && !authenticated) {
            fail(1008, ErrorCode::PermissionDenied, id, "authentication required");
            return;
        }
        const bool readOnly = req.readOnly() || principal_.readOnly || impl_->o.readOnly_;
        try {
            acquireHandle(readOnly);
        } catch (const std::exception& e) {
            fail(1011, ErrorCode::Internal, id, e.what());
            return;
        }
        readOnly_ = readOnly;
        window_ = impl_->window;
        if (req.window() != 0 && req.window() < window_)
            window_ = req.window();
        maxMessage_ = static_cast<std::uint32_t>(impl_->maxMessage);
        if (req.maxMessage() != 0 && req.maxMessage() < maxMessage_)
            maxMessage_ = req.maxMessage();
        hello_ = true;
        detail::ReplyFields f;
        f.id = id;
        f.kind = Kind::Welcome;
        f.protocol = detail::kProtocolVersion;
        f.server = impl_->serverName;
        f.session = id_;
        f.readOnly = readOnly_;
        f.window = window_;
        f.maxMessage = maxMessage_;
        f.maxPipeline = static_cast<std::uint16_t>(impl_->o.maxPipeline_);
        sendDirect(f);
    }

    void acquireHandle(bool readOnly)
    {
        if (impl_->o.isolation_ == Isolation::Shared) {
            handle_ = impl_->db;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            auto& idle = readOnly ? impl_->idleReaders : impl_->idleWriters;
            if (!idle.empty()) {
                own_ = std::move(idle.back());
                idle.pop_back();
            }
        }
        if (!own_.valid())
            own_ = readOnly ? impl_->db->openReader() : impl_->db->openHandle();
        std::lock_guard<std::mutex> lock(mutex_);
        handle_ = &own_;
    }

    // ----------------------------------------------------------------------------- worker ---

    void runNext()
    {
        for (;;) {
            Slot slot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (closing_.load()) {
                    while (!queue_.empty()) {
                        queue_.pop();
                        impl_->queued.fetch_sub(1, std::memory_order_relaxed);
                    }
                }
                if (queue_.empty()) {
                    running_ = 0;
                    if (closing_.load() || teardownPending_) {
                        // The connection went while this worker was busy: tear down here, then give the worker back.
                        lock.unlock();
                        teardown();
                    } else {
                        workerActive_ = false;
                        cv_.notify_all();
                        lock.unlock();
                    }
                    impl_->taskDone();
                    unpin();
                    return;
                }
                slot = queue_.pop();
                impl_->queued.fetch_sub(1, std::memory_order_relaxed);
                running_ = slot.id;
                interruptRunning_ = false;
                runStarted_ = Clock::now();
            }
            if (!slot.cancelled)
                process(slot);
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = 0;
            interruptRunning_ = false;
        }
    }

    void process(Slot& slot)
    {
        impl_->running.fetch_add(1);
        const auto req = bufs::root<Request>(slot.bytes.data());
        const Clock::time_point started = Clock::now();
        detail::ReplyFields done;
        done.id = slot.id;
        done.kind = Kind::Done;
        ErrorCode result = ErrorCode::Ok;
        std::string scratch;
        bool endSession = false;
        try {
            dispatch(req, slot, done);
        } catch (const Abort& a) {
            if (a.reason == Abort::Closed) {
                impl_->running.fetch_sub(1);
                return;  // nobody is listening
            }
            done = detail::ReplyFields();
            done.id = slot.id;
            done.kind = Kind::Error;
            if (a.reason == Abort::Interrupted) {
                done.code = detail::wireCode(ErrorCode::Interrupted);
                done.message = "interrupted";
                result = ErrorCode::Interrupted;
            } else {
                done.code = detail::wireCode(ErrorCode::Runtime);
                done.message = "client stopped reading";
                result = ErrorCode::Runtime;
                endSession = true;
                impl_->stalls.fetch_add(1);
            }
        } catch (const std::exception& e) {
            done = detail::ReplyFields();
            done.id = slot.id;
            detail::errorFields(done, e, scratch);
            result = detail::errorCodeFrom(done.code);
            if (interruptRunning_ && result == ErrorCode::Interrupted)
                done.message = "interrupted";
        }
        if (done.kind == Kind::Error)
            impl_->errors.fetch_add(1);
        done.elapsedUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
        if (done.kind == Kind::Done && handle_) {
            done.totalChanges = handle_->totalChanges();
            done.inTransaction = handle_->inTransaction();
        }
        emit(detail::makeReply(*pool_, done), false);
        impl_->requests.fetch_add(1);
        impl_->running.fetch_sub(1);
        if (impl_->o.onRequest_) {
            try {
                impl_->o.onRequest_(principal_, opName(slot.op), req.sql(), done.elapsedUs, result);
            } catch (...) {
            }
        }
        if (slot.op == Op::Goodbye && done.kind == Kind::Done)
            postLoop(Action::Goodbye);
        else if (endSession)
            postLoop(Action::ClosePolicy);
        if (workerWriterTooBig())
            workerWriter().compact();
    }

    bool workerWriterTooBig() { return workerWriter().capacity() > impl_->o.retainBytes_; }

    /// Queues a finished frame for the loop. Rows frames first wait for credit.
    void emit(http::Buffer frame, bool rows)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (rows) {
            const std::size_t cost = detail::frameCost(frame.size());
            const auto deadline = Clock::now() + impl_->o.stallTimeout_;
            const auto fits = [&] {
                return closing_.load() || interruptRunning_ || outstanding_ == 0 || outstanding_ + cost <= window_;
            };
            if (!cv_.wait_until(lock, deadline, fits))
                throw Abort{Abort::Stalled};
            if (closing_.load())
                throw Abort{Abort::Closed};
            if (interruptRunning_)
                throw Abort{Abort::Interrupted};
            outstanding_ += cost;
        } else if (closing_.load()) {
            return;
        }
        outbox_.push_back(std::move(frame));
        const bool post = !flushPosted_;
        flushPosted_ = true;
        lock.unlock();
        if (post) {
            pin();
            Session* self = this;
            if (!ws_->post([self] { self->flushOutbox(); })) {
                unpin();
                std::lock_guard<std::mutex> again(mutex_);
                closing_ = true;
            }
        }
    }

    // ----------------------------------------------------------------------------- the operations ---

    struct Emitter
    {
        Session* session;
        std::uint32_t id;
        void operator()(BufsBytes batch) const
        {
            detail::ReplyFields f;
            f.id = id;
            f.kind = Kind::Rows;
            const std::size_t head = detail::replyBound(f);
            if (head + batch.size() > session->maxMessage_)
                throw Error(ErrorCode::Runtime,
                            "a row exceeds the largest message (" + std::to_string(session->maxMessage_) +
                                " bytes); raise max_message or use blob_prefix");
            session->emit(detail::makeReply(*session->pool_, f,
                                            std::string_view(reinterpret_cast<const char*>(batch.data()), batch.size())),
                          true);
        }
    };

    BufsOptions optionsFor(const Request::Reader& req)
    {
        BufsOptions options;
        const auto r = req.options();
        const std::size_t room = maxMessage_ > 4096 ? maxMessage_ - 4096 : 4096;
        std::size_t bytes = r.batchBytes() ? r.batchBytes() : impl_->o.batchBytes_;
        options.batchBytes(bytes < room ? bytes : room);
        options.batchRows(r.batchRows());
        options.maxRows(static_cast<std::size_t>(r.maxRows()));
        options.countAllRows(r.countAllRows());
        options.blobPrefix(r.blobPrefix());
        options.dictionary(!r.noDictionary());
        options.narrowing(!r.noNarrowing());
        return options;
    }

    /// Fills params_ (or named_) from the request, borrowing text and blob bytes from the slot.
    void readParams(const Request::Reader& req)
    {
        params_.clear();
        named_.clear();
        const auto list = req.params();
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
                params_.push_back(std::move(v));
            } else {
                anyNamed = true;
                named_.emplace_back(std::string(p.name()), std::move(v));
            }
        }
        if (anyNamed && anyPositional)
            throw Error(ErrorCode::InvalidArgument, "named and positional parameters cannot be mixed");
        usesNamed_ = anyNamed;
    }

    Transaction* findTransaction(std::uint32_t id)
    {
        for (auto& entry : transactions_)
            if (entry.first == id)
                return &entry.second;
        throw Error(ErrorCode::BadTransaction, "no such transaction: " + std::to_string(id));
    }

    Statement* findStatement(std::uint32_t id)
    {
        for (auto& entry : statements_)
            if (entry.first == id)
                return &entry.second;
        throw Error(ErrorCode::InvalidArgument, "no such statement: " + std::to_string(id));
    }

    void dispatch(const Request::Reader& req, Slot& slot, detail::ReplyFields& done)
    {
        if (closing_.load())
            throw Abort{Abort::Closed};
        switch (slot.op) {
            case Op::Exec:
            case Op::ExecPrepared: execute(req, slot, done, slot.op == Op::ExecPrepared); return;
            case Op::Prepare: {
                if (statements_.size() >= impl_->o.maxStatements_)
                    throw Error(ErrorCode::Busy, "too many prepared statements");
                for (auto& entry : statements_)
                    if (entry.first == req.statement())
                        throw Error(ErrorCode::InvalidArgument, "statement id is already in use");
                Statement st = handle_->prepare(req.sql());
                done.kind = Kind::Prepared;
                done.parameters = static_cast<std::uint32_t>(st.parameters());
                parameterNames_.clear();
                for (std::size_t i = 1; i <= st.parameters(); ++i)
                    parameterNames_.push_back(st.parameterName(i));
                columnNames_ = st.columnNames();
                done.parameterNames = &parameterNames_;
                done.columns = &columnNames_;
                done.writes = st.writes();
                statements_.emplace_back(req.statement(), std::move(st));
                return;
            }
            case Op::ClosePrepared:
                for (std::size_t i = 0; i < statements_.size(); ++i)
                    if (statements_[i].first == req.statement()) {
                        statements_.erase(statements_.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                return;
            case Op::Begin: {
                if (transactions_.size() >= impl_->o.maxTransactions_)
                    throw Error(ErrorCode::Busy, "too many open transactions");
                for (auto& entry : transactions_)
                    if (entry.first == req.txn())
                        throw Error(ErrorCode::InvalidArgument, "transaction id is already in use");
                if (req.txn() == 0)
                    throw Error(ErrorCode::InvalidArgument, "a transaction needs a non-zero id");
                transactions_.emplace_back(req.txn(), req.readOnly() ? handle_->beginRead() : handle_->begin());
                return;
            }
            case Op::Commit:
            case Op::Rollback: {
                Transaction* t = findTransaction(req.txn());
                try {
                    slot.op == Op::Commit ? t->commit() : t->rollback();
                } catch (...) {
                    eraseTransaction(req.txn());
                    throw;
                }
                eraseTransaction(req.txn());
                return;
            }
            case Op::Describe: describe(req, slot, done); return;
            case Op::State:
            case Op::Goodbye: return;
            default: throw Error(ErrorCode::Protocol, "unexpected request");
        }
    }

    void eraseTransaction(std::uint32_t id)
    {
        for (std::size_t i = 0; i < transactions_.size(); ++i)
            if (transactions_[i].first == id) {
                transactions_.erase(transactions_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
    }

    void execute(const Request::Reader& req, Slot& slot, detail::ReplyFields& done, bool prepared)
    {
        readParams(req);
        BufsWriter& writer = workerWriter();
        writer.reset();
        writer.options(optionsFor(req));
        Emitter emitter{this, slot.id};
        writer.onBatch([&emitter](BufsBytes b) { emitter(b); });
        Tee tee(writer);
        Transaction* txn = req.txn() ? findTransaction(req.txn()) : nullptr;
        const std::string_view text = req.sql();
        const bool managed = !txn && !prepared && impl_->o.interceptor_;
        if (managed)
            impl_->o.interceptor_->before(*handle_, text, req.hints());
        bool failed = true;
        try {
            if (prepared) {
                Statement* st = findStatement(req.statement());
                st->clear();
                if (usesNamed_) {
                    for (auto& [name, value] : named_)
                        st->bind(std::string_view(name), std::move(value));
                } else {
                    for (std::size_t i = 0; i < params_.size(); ++i)
                        st->bind(i + 1, std::move(params_[i]));
                }
                txn ? st->exec(*txn, tee) : st->exec(tee);
            } else if (usesNamed_) {
                txn ? txn->exec(text, named_, tee) : handle_->exec(text, named_, tee);
            } else {
                txn ? txn->exec(text, params_, tee) : handle_->exec(text, params_, tee);
            }
            failed = false;
        } catch (...) {
            writer.onBatch({});
            params_.clear();
            named_.clear();
            if (managed)
                impl_->o.interceptor_->after(*handle_, text, req.hints(), true);
            if (closing_.load())
                throw Abort{Abort::Closed};
            throw;
        }
        writer.onBatch({});
        params_.clear();
        named_.clear();
        if (managed)
            impl_->o.interceptor_->after(*handle_, text, req.hints(), failed);
        done.changes = tee.changes;
        done.lastInsertId = tee.lastInsertId;
    }

    void describe(const Request::Reader& req, Slot& slot, detail::ReplyFields& done)
    {
        BufsWriter& writer = workerWriter();
        writer.reset();
        writer.options(optionsFor(req));
        Emitter emitter{this, slot.id};
        writer.onBatch([&emitter](BufsBytes b) { emitter(b); });
        static const std::vector<std::string> kTables = {"name"};
        static const std::vector<std::string> kTable = {"name",        "type",         "declared_type", "affinity", "collation",
                                                        "primary_key", "not_null",     "unique",        "has_default",
                                                        "default_value", "default_sql", "generated",    "pk_position"};
        static const std::vector<std::string> kIndexes = {"index_name", "table_name", "unique", "partial", "sql", "position", "column"};
        static const std::vector<std::string> kBlank16(16);
        static const std::vector<std::string> kNone;
        try {
            if (req.what() == Catalog::Tables) {
                writer.begin(ResultShape{kTables, kNone, kBlank16});
                const std::vector<std::string> names = handle_->tables();
                for (const std::string& name : names) {
                    Value row[1] = {Value::textRef(name)};
                    writer.row(row, 1);
                }
            } else if (req.what() == Catalog::Table_) {
                const TableInfo info = handle_->table(req.name());
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
                done.sql = info.sql;
                describedSql_ = info.sql;
                done.sql = describedSql_;
                done.strict = info.strict;
                done.withoutRowid = info.withoutRowid;
            } else {
                writer.begin(ResultShape{kIndexes, kNone, kBlank16});
                const std::vector<IndexInfo> indexes = handle_->indexes(req.name());
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
        } catch (...) {
            writer.onBatch({});
            throw;
        }
        writer.onBatch({});
    }

    // ----------------------------------------------------------------------------- teardown ---

    void teardown()
    {
        std::vector<std::pair<std::uint32_t, Statement>> statements;
        std::vector<std::pair<std::uint32_t, Transaction>> transactions;
        statements.swap(statements_);
        transactions.swap(transactions_);
        transactions.clear();  // rolls back whatever is open
        statements.clear();
        Database* handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!queue_.empty())
                queue_.pop();
            outbox_.clear();
            handle = handle_;
            handle_ = nullptr;
            hello_ = false;
        }
        if (handle && handle == &own_) {
            bool healthy = true;
            try {
                if (own_.inTransaction())
                    own_.exec("ROLLBACK");
            } catch (...) {
                healthy = false;
            }
            std::lock_guard<std::mutex> lock(impl_->mutex);
            auto& idle = readOnly_ ? impl_->idleReaders : impl_->idleWriters;
            if (healthy && idle.size() < impl_->o.idleHandles_)
                idle.push_back(std::move(own_));
            else
                own_ = Database();
        } else if (handle) {
            try {
                if (handle->inTransaction())
                    handle->exec("ROLLBACK");
            } catch (...) {
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        workerActive_ = false;
        teardownPending_ = false;
        cv_.notify_all();
    }

    // ----------------------------------------------------------------------------- state ---

    std::shared_ptr<SqlSocket::Impl> impl_;
    std::shared_ptr<http::WebSocketConnection> ws_;
    http::BufferPool* pool_;
    std::shared_ptr<AcceptInfo> info_;
    Principal principal_;
    const std::uint64_t id_;

    // Loop-thread state.
    bool hello_ = false;
    bool readOnly_ = false;
    std::uint64_t window_ = 0;
    std::uint32_t maxMessage_ = 0;
    std::vector<http::Buffer> flushScratch_;

    // Shared state, guarded by mutex_.
    std::mutex mutex_;
    std::condition_variable cv_;
    Database own_;
    Database* handle_ = nullptr;
    Ring<Slot> queue_;
    std::vector<http::Buffer> outbox_;
    bool flushPosted_ = false;
    std::uint64_t outstanding_ = 0;
    std::uint32_t running_ = 0;
    bool workerActive_ = false;
    bool teardownPending_ = false;
    bool interruptRunning_ = false;
    Clock::time_point runStarted_{};
    std::atomic<bool> closing_{false};
    std::atomic<bool> draining_{false};

    // Worker-only state.
    std::vector<std::pair<std::uint32_t, Statement>> statements_;
    std::vector<std::pair<std::uint32_t, Transaction>> transactions_;
    std::vector<Value> params_;
    NamedParams named_;
    bool usesNamed_ = false;
    std::vector<std::string> parameterNames_, columnNames_;
    std::string describedSql_;

    std::mutex pinMutex_;
    int pins_ = 0;
    std::shared_ptr<Session> pinned_;
};

}  // namespace detail

// ================================================================================================ SqlSocket ===

SqlSocket::SqlSocket(Database& db, SqlSocketOptions options) : impl_(std::make_shared<Impl>())
{
    impl_->db = &db;
    impl_->o = std::move(options);
    impl_->serverName = impl_->o.serverName_.empty() ? "libsql" : impl_->o.serverName_;
    impl_->window = impl_->o.window_;
    if (impl_->o.executor_) {
        impl_->post = impl_->o.executor_;
    } else {
        impl_->ownPool = std::make_unique<http::ThreadPool>(impl_->o.workers_ ? impl_->o.workers_ : 1, 1024);
        http::ThreadPool* pool = impl_->ownPool.get();
        impl_->post = [pool](std::function<void()> task) { return pool->tryEnqueue(std::move(task)); };
    }
    if (impl_->o.statementTimeout_.count() > 0) {
        Impl* impl = impl_.get();
        impl_->watchdog = std::thread([impl] {
            while (!impl->stopWatchdog.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::vector<std::shared_ptr<detail::Session>> live;
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    for (auto& w : impl->sessions)
                        if (auto s = w.lock())
                            live.push_back(std::move(s));
                }
                const auto now = Clock::now();
                for (auto& s : live)
                    s->checkTimeout(now, impl->o.statementTimeout_);
            }
        });
    }
}

SqlSocket::~SqlSocket()
{
    shutdown(std::chrono::milliseconds(0));
}

http::WebSocketRouteOptions SqlSocket::routeOptions() const
{
    http::WebSocketRouteOptions route;
    route.subprotocols = {"libsql.v1"};
    route.maxMessageBytes = impl_->routeMax;
    route.idleTimeoutSeconds = static_cast<int>(impl_->o.idleTimeout_.count());
    std::shared_ptr<Impl> impl = impl_;
    route.accept = [impl](const http::WebSocketHandshakeRequest& request) {
        http::WebSocketAccept decision;
        if (!impl->accepting.load()) {
            decision.status = 503;
            return decision;
        }
        const std::string_view remote = request.remoteAddress;
        std::string address(remote.substr(0, remote.rfind(':')));
        auto reservation = std::make_shared<detail::Reservation>();
        reservation->impl = impl;
        reservation->address = address;
        const std::size_t count = impl->sessionCount.fetch_add(1) + 1;
        // The reservation's destructor gives the slot back on every refusal below.
        if (impl->o.maxSessions_ != 0 && count > impl->o.maxSessions_) {
            impl->refused.fetch_add(1);
            decision.status = 503;
            return decision;
        }
        if (impl->o.maxSessionsPerAddress_ != 0) {
            std::lock_guard<std::mutex> lock(impl->mutex);
            reservation->counted = true;
            if (++impl->perAddress[address] > impl->o.maxSessionsPerAddress_) {
                impl->refused.fetch_add(1);
                decision.status = 429;
                return decision;
            }
        }
        auto info = std::make_shared<detail::AcceptInfo>();
        info->reservation = std::move(reservation);
        if (impl->o.authorize_) {
            Handshake handshake;
            handshake.path = request.request.path;
            handshake.remoteAddress = remote;
            handshake.header = [&request](std::string_view name) { return request.request.headers.get(name); };
            handshake.routeParam = [&request](std::string_view name) -> std::string_view {
                for (const auto& kv : request.routeParams)
                    if (kv.first == name)
                        return kv.second;
                return {};
            };
            SqlSocketOptions::AuthorizeResult result;
            try {
                result = impl->o.authorize_(handshake);
            } catch (...) {
                decision.status = 500;
                return decision;
            }
            if (auto* principal = std::get_if<Principal>(&result)) {
                info->principal = std::move(*principal);
                info->authenticated = true;
            } else if (const int status = std::get<int>(result); status != 0) {
                impl->refused.fetch_add(1);
                decision.status = status;
                return decision;
            }
        }
        decision.subprotocol = "libsql.v1";
        decision.userData = std::move(info);
        return decision;
    };
    return route;
}

http::WebSocketHandler SqlSocket::handler(http::BufferPool* pool)
{
    std::shared_ptr<Impl> impl = impl_;
    http::BufferPool* frames = pool ? pool : &http::BufferPool::shared();
    return [impl, frames](std::shared_ptr<http::WebSocketConnection> ws) {
        auto info = std::static_pointer_cast<detail::AcceptInfo>(ws->userData());
        if (!info) {
            // Registered without routeOptions(): there is no accept hook, so make the defaults.
            info = std::make_shared<detail::AcceptInfo>();
            info->reservation = std::make_shared<detail::Reservation>();
            info->reservation->impl = impl;
            impl->sessionCount.fetch_add(1);
            ws->setHandshake(ws->subprotocol(), info);
        }
        auto session = std::make_shared<detail::Session>(impl, ws, info, *frames);
        const std::size_t now = impl->sessionCount.load();
        std::size_t peak = impl->peak.load();
        while (now > peak && !impl->peak.compare_exchange_weak(peak, now)) {
        }
        impl->opened.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto& list = impl->sessions;
            for (std::size_t i = 0; i < list.size();) {
                if (list[i].expired()) {
                    list[i] = std::move(list.back());
                    list.pop_back();
                } else {
                    ++i;
                }
            }
            list.push_back(session);
        }
        ws->onMessage([session](std::string_view data, bool binary) { session->onFrame(data, binary); });
        ws->onClose([session] { session->onClose(); });
    };
}

void SqlSocket::attach(http::Server& server, std::string pattern)
{
    const http::ServerConfig& config = server.config();
    // Limits the route has to respect: the connection closes when its queue passes its frame or byte limit,
    // so the flow-control window is held below both.
    std::size_t window = impl_->o.window_;
    const std::size_t byBytes = config.maxWebSocketQueuedBytes / 2;
    const std::size_t reserved = impl_->o.maxPipeline_ + 8;
    const std::size_t byFrames = config.maxWebSocketQueuedFrames > reserved
                                     ? (config.maxWebSocketQueuedFrames - reserved) * detail::kMinFrameCost
                                     : detail::kMinFrameCost;
    if (window > byBytes)
        window = byBytes;
    if (window > byFrames)
        window = byFrames;
    impl_->window = window;
    impl_->maxMessage = impl_->o.maxMessage_ ? impl_->o.maxMessage_ : config.maxWebSocketMessageBytes;
    impl_->routeMax = impl_->maxMessage;
    server.onWebSocket(std::move(pattern), handler(&server.bufferPool()), routeOptions());
}

void SqlSocket::shutdown(std::chrono::milliseconds grace)
{
    if (!impl_)
        return;
    Impl& impl = *impl_;
    if (impl.accepting.exchange(false)) {
        std::vector<std::shared_ptr<detail::Session>> live;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            for (auto& w : impl.sessions)
                if (auto s = w.lock())
                    live.push_back(std::move(s));
        }
        for (auto& s : live)
            s->beginDrain();
        const auto until = Clock::now() + grace;
        for (;;) {
            bool allIdle = true;
            for (auto& s : live)
                allIdle = allIdle && s->idle();
            if (allIdle || Clock::now() >= until)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (auto& s : live)
            s->forceClose();
        // The workers finish what they were told to stop.
        std::unique_lock<std::mutex> lock(impl.drainMutex);
        impl.drained.wait_for(lock, std::chrono::seconds(10), [&] { return impl.tasks.load() == 0; });
    }
    impl.stopWatchdog = true;
    if (impl.watchdog.joinable())
        impl.watchdog.join();
    if (impl.ownPool)
        impl.ownPool->shutdown();
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.idleWriters.clear();
        impl.idleReaders.clear();
    }
}

SqlSocket::Stats SqlSocket::stats() const
{
    Stats s;
    const Impl& i = *impl_;
    s.sessions = i.sessionCount.load();
    s.peakSessions = i.peak.load();
    s.sessionsOpened = i.opened.load();
    s.sessionsRefused = i.refused.load();
    s.requests = i.requests.load();
    s.errors = i.errors.load();
    s.interrupts = i.interrupts.load();
    s.stalls = i.stalls.load();
    s.bytesIn = i.bytesIn.load();
    s.bytesOut = i.bytesOut.load();
    s.running = i.running.load();
    s.queued = i.queued.load();
    {
        auto& m = const_cast<Impl&>(i);
        std::lock_guard<std::mutex> lock(m.mutex);
        s.idleHandles = m.idleWriters.size() + m.idleReaders.size();
    }
    return s;
}

}  // namespace sql::remote
