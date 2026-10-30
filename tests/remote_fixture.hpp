// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A database served over a real socket: an http::Server with a SqlSocket attached, for the tests of
// Database::connect.
#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>
#include <functional>
#include <future>
#include <memory>
#include <thread>

#include "http/server.hpp"
#include "http/websocket_client.hpp"
#include "sql/remote/sql_socket.hpp"
#include "sql_test.hpp"

namespace remote_test {

/// A free loopback port: bound to port 0 and released (racy in theory, fine for a test).
inline std::uint16_t freePort()
{
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || ::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("no free port");
    socklen_t size = sizeof(address);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &size);
    ::close(s);
    return ntohs(address.sin_port);
}

struct Served
{
    tst::Scratch scratch{"remote"};
    sql::Database db;
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::unique_ptr<sql::remote::SqlSocket> socket;
    std::future<void> serving;
    std::uint16_t port = 0;
    std::string localPath;

    explicit Served(sql::remote::SqlSocketOptions options = sql::remote::SqlSocketOptions(),
                    std::function<void(http::ServerConfig&)> tweak = {})
        : db(sql::Database::configure().busyTimeout(std::chrono::milliseconds(500)).open(scratch.file()))
    {
        config.host = "127.0.0.1";
        config.ioThreads = 1;
        config.workerThreads = 1;
        if (tweak)
            tweak(config);
        for (int attempt = 0; attempt < 50; ++attempt) {
            config.port = freePort();
            try {
                server = std::make_unique<http::Server>(config);
                socket = std::make_unique<sql::remote::SqlSocket>(db, options);
                socket->attach(*server, "/api/v1/ws/sql");
                localPath = "/tmp/libsql-test-" + std::to_string(::getpid()) + "-" + std::to_string(config.port) + ".sock";
                socket->listenLocal(localPath);
                serving = std::async(std::launch::async, [this] { server->run(); });
                for (int i = 0; i < 200; ++i) {
                    http::WebSocketClient probe;
                    try {
                        probe.connect("ws://127.0.0.1:" + std::to_string(config.port) + "/nothing");
                    } catch (const http::WebSocketError& e) {
                        if (e.kind() == http::WebSocketError::Kind::Handshake) {
                            port = config.port;
                            return;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            } catch (...) {
            }
        }
        tst::fail(__FILE__, __LINE__, "could not start a server");
    }
    ~Served()
    {
        if (socket)
            socket->shutdown(std::chrono::milliseconds(200));
        if (server) {
            server->stop();
            serving.get();
        }
    }

    std::string url(const std::string& path = "/api/v1/ws/sql") const
    {
        return "ws://127.0.0.1:" + std::to_string(port) + path;
    }
    sql::Database connect(const sql::RemoteOptions& options = sql::RemoteOptions()) const
    {
        return sql::Database::connect(url(), options);
    }
    /// The same database through the local socket (shared-memory rings).
    std::string localUrl() const { return "unix://" + localPath; }
    sql::Database connectLocal(const sql::RemoteOptions& options = sql::RemoteOptions()) const
    {
        return sql::Database::connect(localUrl(), options);
    }
};

}  // namespace remote_test
