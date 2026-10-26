// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Serves the browser front end and a WebApi over HTTP.
//
// The front end is a single index.html linked into the binary (see cmake/EmbedIndexHtml.cmake). A public
// directory given on the command line replaces it, and its other files are served beside it.
//
// All requests that touch the database run, one at a time, on one worker thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "sql/sql.hpp"

namespace sqlweb {

struct WebOptions
{
    /// The API changes data and has no login, so it listens on this machine only unless told otherwise.
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string publicDir = "./public";
    /// False: serve the embedded index.html when the binary has one.
    bool publicDirOverridden = false;
    /// Tells the front end to disable editing.
    bool readOnly = false;
    /// Also serve the database over a WebSocket route (sql::remote::SqlSocket): other programs connect with
    /// sql::Database::connect, and the front end runs its queries through it.
    bool socket = true;
    std::string socketPath = "/api/v1/ws/sql";
    /// When not empty the WebSocket route needs it ("Authorization: Bearer <token>" in the handshake, or the
    /// token in the first message). The JSON API and the page stay open: this protects the socket only.
    std::string token;
};

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
    void run(std::function<bool()> shouldStop = {});
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sqlweb
