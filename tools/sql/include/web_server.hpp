// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Serves the browser front end, and the database over a WebSocket route (sql::remote::SqlSocket).
//
// The front end is a single index.html linked into the binary (see cmake/EmbedIndexHtml.cmake). A public
// directory given on the command line replaces it, and its other files are served beside it.
//
// The front end does everything through that route (SQL in the bufs protocol of sql-remote.md, nothing else): there is
// no JSON API. All requests that touch the database run, one at a time, on one worker thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "sql/sql.hpp"

namespace sqlweb {

struct WebOptions
{
    /// The database can be changed through this server and there is no login (but a token, below), so it listens on
    /// this machine only unless told otherwise.
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string publicDir = "./public";
    /// False: serve the embedded index.html when the binary has one.
    bool publicDirOverridden = false;
    /// Tells the front end to disable editing; the session is read-only, so the database refuses writes too.
    bool readOnly = false;
    /// Where the WebSocket route is served. The front end looks for it here, so change it only for other clients.
    std::string socketPath = "/api/v1/ws/sql";
    /// Not empty: the database is also served to programs on this machine on a Unix socket (owner only), through
    /// shared memory: `sql unix://<path>` or Database::connect("unix://<path>"). The token, if any, is still needed.
    std::string unixSocket;
    /// When not empty the route needs it ("Authorization: Bearer <token>" in the handshake, or the token in the
    /// first message, which is how the page presents it). The page itself is public.
    std::string token;
    /// Where report definitions are kept (one JSON file each; /api/v1/reports). Empty: `<database file>.reports`.
    std::string reportsDir;
    /// Both set: serve https and wss on `port` instead of http and ws (needs a libhttp built with OpenSSL).
    std::string tlsCertificate;
    std::string tlsKey;
};

/// True when this build has the report features (SQL_WITH_REPORTS, which needs SQL_WITH_REMOTE): the report files
/// (/api/v1/reports), the embedded report font and the /reports page. Without them those routes do not exist.
bool hasReports() noexcept;

/// True when the binary carries its own index.html.
bool hasEmbeddedIndex() noexcept;

class WebServer
{
public:
    WebServer(sql::Database& db, WebOptions options);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    /// Serves until `shouldStop` returns true (polled) or stop() is called.
    void run(std::function<bool()> shouldStop = {}, std::function<void()> onListening = {});
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sqlweb
