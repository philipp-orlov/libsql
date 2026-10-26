// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// WebServer: the SQL WebSocket route, its token and origin checks, and front end delivery, over a real loopback socket.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sql_test.hpp"
#include "web_server.hpp"

namespace {

struct Response
{
    int status = 0;
    std::string head;
    std::string body;

    /// A header's value, or empty.
    std::string header(const std::string& name) const
    {
        std::string lower = head;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string key = "\r\n" + name + ":";
        const std::size_t at = lower.find(key);
        if (at == std::string::npos)
            return {};
        const std::size_t from = at + key.size();
        const std::size_t to = head.find("\r\n", from);
        std::string value = head.substr(from, to - from);
        const std::size_t first = value.find_first_not_of(' ');
        return first == std::string::npos ? std::string() : value.substr(first);
    }
};

std::uint16_t freePort()
{
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("cannot bind a probe socket");
    socklen_t size = sizeof(address);
    ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size);
    ::close(probe);
    return ntohs(address.sin_port);
}

int connectTo(std::uint16_t port)
{
    for (int attempt = 0; attempt < 2000; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            timeval timeout{10, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("the server did not start");
}

struct Running
{
    tst::Scratch scratch{"web-server"};
    sql::Database db = scratch.open();
    sqlweb::WebOptions options;
    std::unique_ptr<sqlweb::WebServer> server;
    std::thread thread;
    std::atomic<bool> stop{false};

    explicit Running(sqlweb::WebOptions o = {}) : options(std::move(o))
    {
        options.host = "127.0.0.1";
        options.port = freePort();
        db.exec(
            "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t(v) VALUES ('a'), ('b')");
        server = std::make_unique<sqlweb::WebServer>(db, options);
        thread = std::thread([this] { server->run([this] { return stop.load(); }); });
        ::close(connectTo(options.port));  // returns once it listens
    }

    ~Running()
    {
        stop = true;
        thread.join();
    }

    /// One request on its own connection; `extra` is raw header lines, each ending in CRLF.
    Response send(const std::string& method, const std::string& path, const std::string& body = {},
                  const std::string& extra = {}, const std::string& host = "127.0.0.1") const
    {
        const int fd = connectTo(options.port);
        std::string request =
            method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
        request +=
            "Content-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
        ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
        std::string raw;
        char chunk[4096];
        for (ssize_t n; (n = ::recv(fd, chunk, sizeof(chunk), 0)) > 0;)
            raw.append(chunk, static_cast<std::size_t>(n));
        ::close(fd);

        Response response;
        const std::size_t split = raw.find("\r\n\r\n");
        if (raw.size() < 12 || split == std::string::npos)
            return response;
        response.status = std::stoi(raw.substr(9, 3));
        response.head = raw.substr(0, split + 2);
        response.body = raw.substr(split + 4);
        return response;
    }

    std::string url() const { return "ws://127.0.0.1:" + std::to_string(options.port) + options.socketPath; }

    Response get(const std::string& path, const std::string& extra = {}) const
    {
        return send("GET", path, {}, extra);
    }

    Response post(const std::string& path, const std::string& body) const
    {
        return send("POST", path, body, "Content-Type: application/json\r\n");
    }
};

}  // namespace

TEST(the_database_is_served_over_a_websocket)
{
    Running s;
    sql::Database remote = sql::Database::connect(s.url());
    CHECK_EQ(tst::only(remote.exec("SELECT count(*) FROM t")), std::string("2"));
    // Changes made in a transaction are shared: every session runs on the one handle.
    remote.exec("BEGIN");
    remote.exec("INSERT INTO t(v) VALUES ('over the socket')");
    CHECK(remote.inTransaction());
    sql::Database other = sql::Database::connect(s.url());
    CHECK_EQ(tst::only(other.exec("SELECT count(*) FROM t")), std::string("3"));
    other.exec("COMMIT");
    CHECK_EQ(tst::only(s.db.exec("SELECT count(*) FROM t")), std::string("3"));
}

TEST(a_failing_statement_is_an_error_not_a_dropped_connection)
{
    Running s;
    sql::Database remote = sql::Database::connect(s.url());
    CHECK_THROWS(remote.exec("SELECT * FROM missing"), sql::ErrorCode::NoSuchTable);
    CHECK_EQ(tst::only(remote.exec("SELECT 1")), std::string("1"));
}

TEST(a_token_protects_the_database)
{
    sqlweb::WebOptions options;
    options.token = "s3cret";
    Running s(options);
    const std::string url = s.url();
    CHECK_THROWS(sql::Database::connect(url), sql::ErrorCode::PermissionDenied);
    CHECK_THROWS(sql::Database::connect(url, sql::RemoteOptions().token("nope")), sql::ErrorCode::PermissionDenied);
    CHECK_THROWS(sql::Database::connect(url, sql::RemoteOptions().authorization("Bearer nope")), sql::ErrorCode::PermissionDenied);
    sql::Database viaHeader = sql::Database::connect(url, sql::RemoteOptions().authorization("Bearer s3cret"));
    CHECK_EQ(tst::only(viaHeader.exec("SELECT 1")), std::string("1"));
    sql::Database viaToken = sql::Database::connect(url, sql::RemoteOptions().token("s3cret"));
    CHECK_EQ(tst::only(viaToken.exec("SELECT 2")), std::string("2"));
    // The page is public (it has to be, to ask for the token); nothing else is served.
    CHECK_EQ(s.get("/api/info").status, 404);
}

TEST(a_read_only_server_refuses_writes)
{
    sqlweb::WebOptions options;
    options.readOnly = true;
    Running s(options);
    sql::Database remote = sql::Database::connect(s.url());
    CHECK_EQ(tst::only(remote.exec("SELECT count(*) FROM t")), std::string("2"));
    CHECK_THROWS(remote.exec("INSERT INTO t(v) VALUES ('x')"), sql::ErrorCode::ReadOnly);
}

TEST(the_route_can_be_moved)
{
    sqlweb::WebOptions options;
    options.socketPath = "/db";
    Running s(options);
    const std::string where = "ws://127.0.0.1:" + std::to_string(s.options.port);
    CHECK_THROWS(sql::Database::connect(where + "/api/v1/ws/sql"), sql::ErrorCode::Connection);
    sql::Database moved = sql::Database::connect(where + "/db");
    CHECK_EQ(tst::only(moved.exec("SELECT 1")), std::string("1"));
}

TEST(there_is_no_json_api)
{
    Running s;
    for (const char* path : {"/api/info", "/api/schema", "/api/dump", "/api/pragmas", "/api/sql", "/api/browse"})
        CHECK_EQ(s.get(path).status, 404);
    CHECK_EQ(s.post("/api/sql", R"({"sql": "DELETE FROM t"})").status, 404);
    CHECK_EQ(tst::only(s.db.exec("SELECT count(*) FROM t")), std::string("2"));
}

TEST(a_page_of_another_origin_cannot_open_the_websocket)
{
    Running s;
    const std::string url = s.url();
    // A browser sends an Origin; only this server's own is let through.
    CHECK_THROWS(sql::Database::connect(url, sql::RemoteOptions().header("Origin", "https://evil.example")), sql::ErrorCode::PermissionDenied);
    sql::Database same = sql::Database::connect(
        url, sql::RemoteOptions().header("Origin", "http://127.0.0.1:" + std::to_string(s.options.port)));
    CHECK_EQ(tst::only(same.exec("SELECT 1")), std::string("1"));
}

TEST(a_request_for_another_host_name_is_refused)
{
    Running s;
    CHECK_EQ(s.send("GET", "/", {}, {}, "evil.example").status, 421);
    CHECK_EQ(s.send("GET", "/", {}, {}, "localhost:1234").status, s.get("/").status);
}

TEST(an_unknown_route_is_404)
{
    Running s;
    CHECK_EQ(s.get("/api/nothing").status, 404);
}

TEST(the_embedded_page_is_served_with_and_without_gzip)
{
    if (!sqlweb::hasEmbeddedIndex())
        return;
    Running s;
    const Response gzipped = s.get("/", "Accept-Encoding: gzip\r\n");
    CHECK_EQ(gzipped.status, 200);
    CHECK_EQ(gzipped.header("content-encoding"), std::string("gzip"));
    CHECK(gzipped.header("content-type").find("text/html") == 0);
    CHECK_EQ(gzipped.header("cache-control"), std::string("no-cache"));
    CHECK_EQ(gzipped.header("x-frame-options"), std::string("DENY"));

    const Response plain = s.get("/index.html");
    CHECK_EQ(plain.status, 200);
    CHECK_EQ(plain.header("content-encoding"), std::string());
    CHECK(plain.body.find("<html") != std::string::npos);
}

TEST(a_public_directory_replaces_the_embedded_page)
{
    tst::Scratch site("web-public");
    const auto dir = site.file("public");
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "index.html") << "<html>from disk</html>";
    std::ofstream(dir / "extra.txt") << "extra";

    sqlweb::WebOptions options;
    options.publicDir = dir.string();
    options.publicDirOverridden = true;
    Running s(options);

    const Response index = s.get("/", "Accept-Encoding: gzip\r\n");
    CHECK_EQ(index.status, 200);
    CHECK_EQ(index.body, std::string("<html>from disk</html>"));
    CHECK_EQ(index.header("content-encoding"), std::string());
    CHECK_EQ(index.header("cache-control"), std::string("no-cache"));

    const Response extra = s.get("/extra.txt");
    CHECK_EQ(extra.status, 200);
    CHECK_EQ(extra.body, std::string("extra"));
    CHECK_EQ(s.get("/missing.txt").status, 404);
}

TEST(a_missing_public_directory_still_serves_the_database)
{
    sqlweb::WebOptions options;
    options.publicDir = "/nonexistent/sql-public";
    options.publicDirOverridden = true;
    Running s(options);
    CHECK_EQ(s.get("/").status, 404);
    CHECK_EQ(tst::only(sql::Database::connect(s.url()).exec("SELECT count(*) FROM t")), std::string("2"));
}

int main()
{
    return tst::runAll("web_server");
}
