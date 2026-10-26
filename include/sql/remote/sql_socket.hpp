// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- serving a sql::Database to remote clients over one WebSocket route of an
// http::Server, next to the application's own routes (sql-remote.md).
//
//     http::Server server(config);
//     server.onGet("/", servePage).onPost("/api/orders", createOrder);   // the application's own
//     sql::remote::SqlSocket sql(db, sql::remote::SqlSocketOptions().authorize(check));
//     sql.attach(server, "/api/v1/ws/sql");                              // the database, remotely
//     server.run(stop);
//     sql.shutdown(std::chrono::seconds(5));
//
// Clients connect with sql::Database::connect("ws://host:port/api/v1/ws/sql"). Without an
// authorize/authenticate hook the route serves the whole database to anyone who can reach it:
// keep it on a trusted network or put the hooks in.
#pragma once

#if !SQL_WITH_REMOTE
#error "libsql was built without SQL_WITH_REMOTE"
#endif

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "http/server.hpp"
#include "sql/sql.hpp"

namespace sql::remote {

namespace detail {
class Session;
struct Reservation;
struct AcceptInfo;
}  // namespace detail

/// What the handshake looked like, for the authorize hook. Valid during the call only.
struct Handshake
{
    std::string_view path;
    std::string_view remoteAddress;  ///< "host:port"
    /// Any request header by name, "" if absent.
    std::function<std::string_view(std::string_view)> header;
    /// A named part of the route pattern ("/db/{tenant}"), "" if absent.
    std::function<std::string_view(std::string_view)> routeParam;
};

/// Who is on the other end, as the application decided.
struct Principal
{
    std::string name;            ///< for logs and statistics
    bool readOnly = false;       ///< the session gets a read-only handle
    std::shared_ptr<void> data;  ///< anything the application wants back in onRequest
};

enum class Isolation
{
    /// Every session has its own handle on the store (Database::openHandle / openReader): own
    /// transactions, own prepared statements; writers take turns on the store's writer slot.
    PerSession,
    /// Every session runs on the one Database given to the SqlSocket, through the serial executor
    /// the application also uses: transactions are shared, like the sql tool's single-user model.
    Shared,
};

/// Called around every Exec / ExecPrepared on the worker, with the session's handle. The sql tool
/// uses it for its "pending until Write Changes" behaviour.
class ExecInterceptor
{
public:
    virtual ~ExecInterceptor() = default;
    /// Before the statement; may run SQL on `db` (a BEGIN, say). `hints` is Request.hints.
    virtual void before(Database& db, std::string_view sql, std::uint32_t hints) = 0;
    /// After it returned or threw (`failed`); may run SQL (a ROLLBACK).
    virtual void after(Database& db, std::string_view sql, std::uint32_t hints, bool failed) noexcept = 0;
};

class SqlSocketOptions
{
public:
    using AuthorizeResult = std::variant<Principal, int>;

    /// At the handshake: a principal, or an HTTP status to refuse with (401, 403, ...). Status 0
    /// means "no decision yet": the client may still authenticate with the token in its first
    /// message. Unset: every handshake is accepted with an anonymous principal.
    SqlSocketOptions& authorize(std::function<AuthorizeResult(const Handshake&)> fn)
    {
        authorize_ = std::move(fn);
        return *this;
    }
    /// At the first message, when authorize left the session without a principal.
    SqlSocketOptions& authenticate(std::function<std::optional<Principal>(std::string_view token)> fn)
    {
        authenticate_ = std::move(fn);
        return *this;
    }
    /// A session with no principal after both hooks is refused. Default false.
    SqlSocketOptions& requireAuth(bool on = true)
    {
        requireAuth_ = on;
        return *this;
    }
    /// Every session is read-only.
    SqlSocketOptions& readOnly(bool on = true)
    {
        readOnly_ = on;
        return *this;
    }
    SqlSocketOptions& isolation(Isolation mode)
    {
        isolation_ = mode;
        return *this;
    }
    /// Runs the workers' tasks (return false when it is full). Default: an own pool of workers()
    /// threads. Isolation::Shared needs a serial executor (the sql tool passes its one worker).
    SqlSocketOptions& executor(std::function<bool(std::function<void()>)> post)
    {
        executor_ = std::move(post);
        return *this;
    }
    SqlSocketOptions& workers(unsigned n)
    {
        workers_ = n;
        return *this;
    }
    SqlSocketOptions& interceptor(std::shared_ptr<ExecInterceptor> i)
    {
        interceptor_ = std::move(i);
        return *this;
    }
    SqlSocketOptions& maxSessions(std::size_t n)
    {
        maxSessions_ = n;
        return *this;
    }
    SqlSocketOptions& maxSessionsPerAddress(std::size_t n)
    {
        maxSessionsPerAddress_ = n;
        return *this;
    }
    SqlSocketOptions& maxPipeline(std::size_t n)
    {
        maxPipeline_ = n;
        return *this;
    }
    SqlSocketOptions& maxStatements(std::size_t n)
    {
        maxStatements_ = n;
        return *this;
    }
    SqlSocketOptions& maxTransactions(std::size_t n)
    {
        maxTransactions_ = n;
        return *this;
    }
    /// Bytes of result rows a client may leave unread before the server waits (flow control).
    SqlSocketOptions& window(std::size_t bytes)
    {
        window_ = bytes;
        return *this;
    }
    SqlSocketOptions& batchBytes(std::size_t bytes)
    {
        batchBytes_ = bytes;
        return *this;
    }
    /// Largest WebSocket message either way; 0 = the http::Server's maxWebSocketMessageBytes.
    SqlSocketOptions& maxMessage(std::size_t bytes)
    {
        maxMessage_ = bytes;
        return *this;
    }
    SqlSocketOptions& idleTimeout(std::chrono::seconds t)
    {
        idleTimeout_ = t;
        return *this;
    }
    /// How long a request may wait for a client that does not read its rows before the session is cut.
    SqlSocketOptions& stallTimeout(std::chrono::seconds t)
    {
        stallTimeout_ = t;
        return *this;
    }
    /// A request running longer is interrupted. 0 = none.
    SqlSocketOptions& statementTimeout(std::chrono::milliseconds t)
    {
        statementTimeout_ = t;
        return *this;
    }
    /// Idle handles kept for the next session (PerSession).
    SqlSocketOptions& idleHandles(std::size_t n)
    {
        idleHandles_ = n;
        return *this;
    }
    /// After a request whose result grew a worker's encoder beyond this, its buffers are given back.
    SqlSocketOptions& retainBytes(std::size_t n)
    {
        retainBytes_ = n;
        return *this;
    }
    SqlSocketOptions& serverName(std::string name)
    {
        serverName_ = std::move(name);
        return *this;
    }
    /// Called for every finished request, on the worker: for audit logs and metrics.
    SqlSocketOptions& onRequest(std::function<void(const Principal&, std::string_view op, std::string_view sql,
                                                   std::uint64_t elapsedUs, ErrorCode result)> fn)
    {
        onRequest_ = std::move(fn);
        return *this;
    }

private:
    friend class SqlSocket;
    friend class detail::Session;
    friend struct detail::Reservation;
    std::function<AuthorizeResult(const Handshake&)> authorize_;
    std::function<std::optional<Principal>(std::string_view)> authenticate_;
    bool requireAuth_ = false;
    bool readOnly_ = false;
    Isolation isolation_ = Isolation::PerSession;
    std::function<bool(std::function<void()>)> executor_;
    unsigned workers_ = 4;
    std::shared_ptr<ExecInterceptor> interceptor_;
    std::size_t maxSessions_ = 256;
    std::size_t maxSessionsPerAddress_ = 0;
    std::size_t maxPipeline_ = 16;
    std::size_t maxStatements_ = 256;
    std::size_t maxTransactions_ = 4;
    std::size_t window_ = 1 << 20;
    std::size_t batchBytes_ = 256 << 10;
    std::size_t maxMessage_ = 0;
    std::chrono::seconds idleTimeout_{15 * 60};
    std::chrono::seconds stallTimeout_{60};
    std::chrono::milliseconds statementTimeout_{0};
    std::size_t idleHandles_ = 4;
    std::size_t retainBytes_ = 8 << 20;
    std::string serverName_;
    std::function<void(const Principal&, std::string_view, std::string_view, std::uint64_t, ErrorCode)> onRequest_;
};

/// Serves sql::Database sessions over one WebSocket route of an http::Server. Construct it, attach() it
/// before server.run(), and call shutdown() after run() returned (or before stop(), for a graceful
/// drain). Not copyable or movable: sessions point back at it. It must outlive the server's run().
class SqlSocket
{
public:
    SqlSocket(Database& db, SqlSocketOptions options = SqlSocketOptions());
    ~SqlSocket();  ///< calls shutdown(0) if that was not done
    SqlSocket(const SqlSocket&) = delete;
    SqlSocket& operator=(const SqlSocket&) = delete;

    /// Registers the route on `server` (sub-protocol libsql.v1, the accept hook, the message limit and the
    /// idle timeout). Call once per route; several routes, or servers, may share one SqlSocket.
    void attach(http::Server& server, std::string pattern);

    /// The pieces attach() uses, for an application that registers routes itself:
    ///   server.onWebSocket(pattern, socket.handler(&server.bufferPool()), socket.routeOptions());
    /// `pool` is where frames come from (nullptr: http::BufferPool::shared()).
    http::WebSocketHandler handler(http::BufferPool* pool = nullptr);
    http::WebSocketRouteOptions routeOptions() const;

    /// Stops accepting sessions, tells every session the server is going away, lets running requests
    /// finish for up to `grace`, interrupts the rest, closes every connection and waits for the
    /// workers. Idempotent.
    void shutdown(std::chrono::milliseconds grace);

    struct Stats
    {
        std::size_t sessions = 0, peakSessions = 0;
        std::uint64_t sessionsOpened = 0, sessionsRefused = 0;
        std::uint64_t requests = 0, errors = 0, interrupts = 0, stalls = 0;
        std::uint64_t bytesIn = 0, bytesOut = 0;
        std::size_t running = 0, queued = 0;
        std::size_t idleHandles = 0;
    };
    Stats stats() const;  ///< any thread

    struct Impl;  ///< internal

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace sql::remote
