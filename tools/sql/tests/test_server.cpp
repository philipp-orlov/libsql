// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// WebServer: routing, request checks and front end delivery, over a real loopback socket.

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

#include "sql/wire/result_view.hpp"
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

TEST(the_api_answers_json)
{
    Running s;
    const Response info = s.get("/api/info");
    CHECK_EQ(info.status, 200);
    CHECK(info.header("content-type").find("application/json") == 0);
    CHECK_EQ(info.header("cache-control"), std::string("no-store"));
    CHECK(info.body.find("\"inTransaction\":false") != std::string::npos);

    const Response schema = s.get("/api/schema");
    CHECK_EQ(schema.status, 200);
    CHECK(schema.body.find("\"name\":\"t\"") != std::string::npos);
}

TEST(a_query_goes_through_the_worker_and_back)
{
    Running s;
    const Response reply = s.post("/api/sql", R"({"sql": "SELECT v FROM t ORDER BY id"})");
    CHECK_EQ(reply.status, 200);
    CHECK(reply.body.find("\"rows\":[[\"a\"],[\"b\"]]") != std::string::npos);
}

#if SQL_WITH_BUFS
TEST(a_query_can_come_back_as_bufs)
{
    Running s;
    const Response reply = s.post("/api/sql", R"({"sql": "SELECT id, v FROM t ORDER BY id", "format": "bufs"})");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.header("content-type"), std::string("application/vnd.libsql.bufs"));
    CHECK_EQ(reply.header("x-sql-in-transaction"), std::string("false"));
    std::vector<std::uint64_t> aligned((reply.body.size() + 7) / 8);
    std::memcpy(aligned.data(), reply.body.data(), reply.body.size());
    const auto view = sql::wire::ResultView::open(aligned.data(), reply.body.size());
    CHECK_EQ(view.rowCount(), std::uint64_t(2));
    CHECK_EQ(std::string(view.value(1, 1).text()), std::string("b"));
}
#endif

TEST(the_database_is_also_served_over_a_websocket)
{
    Running s;
    const std::string url = "ws://127.0.0.1:" + std::to_string(s.options.port) + "/api/v1/ws/sql";
    sql::Database remote = sql::Database::connect(url);
    CHECK_EQ(tst::only(remote.exec("SELECT count(*) FROM t")), std::string("2"));
    // The page learns where the route is.
    const Response info = s.get("/api/info");
    CHECK(info.body.find("\"sqlSocket\":\"/api/v1/ws/sql\"") != std::string::npos);
    CHECK(info.body.find("\"sqlSocketNeedsToken\":false") != std::string::npos);
    // The socket and the JSON API are one database and one pending transaction: a change made over the socket
    // stays pending, and the JSON API's Write Changes commits it.
    remote.exec("BEGIN");
    remote.exec("INSERT INTO t(v) VALUES ('over the socket')");
    CHECK(s.post("/api/browse", R"({"table": "t"})").body.find("over the socket") != std::string::npos);
    CHECK(s.post("/api/browse", R"({"table": "t"})").body.find("\"inTransaction\":true") != std::string::npos);
    CHECK_EQ(s.post("/api/commit", "{}").status, 200);
    CHECK_EQ(tst::only(s.db.exec("SELECT count(*) FROM t")), std::string("3"));
}

TEST(a_token_protects_the_websocket_route)
{
    sqlweb::WebOptions options;
    options.token = "s3cret";
    Running s(options);
    const std::string url = "ws://127.0.0.1:" + std::to_string(s.options.port) + "/api/v1/ws/sql";
    CHECK_THROWS(sql::Database::connect(url), sql::ErrorCode::PermissionDenied);
    CHECK_THROWS(sql::Database::connect(url, sql::RemoteOptions().token("nope")), sql::ErrorCode::PermissionDenied);
    CHECK_THROWS(sql::Database::connect(url, sql::RemoteOptions().authorization("Bearer nope")), sql::ErrorCode::PermissionDenied);
    sql::Database viaHeader = sql::Database::connect(url, sql::RemoteOptions().authorization("Bearer s3cret"));
    CHECK_EQ(tst::only(viaHeader.exec("SELECT 1")), std::string("1"));
    sql::Database viaToken = sql::Database::connect(url, sql::RemoteOptions().token("s3cret"));
    CHECK_EQ(tst::only(viaToken.exec("SELECT 2")), std::string("2"));
    CHECK(s.get("/api/info").body.find("\"sqlSocketNeedsToken\":true") != std::string::npos);
}

TEST(the_websocket_route_can_be_left_out)
{
    sqlweb::WebOptions options;
    options.socket = false;
    Running s(options);
    const std::string url = "ws://127.0.0.1:" + std::to_string(s.options.port) + "/api/v1/ws/sql";
    CHECK_THROWS(sql::Database::connect(url), sql::ErrorCode::Connection);
    CHECK(s.get("/api/info").body.find("\"sqlSocket\":null") != std::string::npos);
}

TEST(a_page_of_another_origin_cannot_open_the_websocket)
{
    Running s;
    const std::string url = "ws://127.0.0.1:" + std::to_string(s.options.port) + "/api/v1/ws/sql";
    // A browser sends an Origin; only this server's own is let through.
    sql::Database other;
    CHECK_THROWS(sql::Database::connect(url, sql::RemoteOptions().header("Origin", "https://evil.example")), sql::ErrorCode::PermissionDenied);
    sql::Database same = sql::Database::connect(
        url, sql::RemoteOptions().header("Origin", "http://127.0.0.1:" + std::to_string(s.options.port)));
    CHECK_EQ(tst::only(same.exec("SELECT 1")), std::string("1"));
}

TEST(an_edit_over_http_is_pending_until_commit)
{
    Running s;
    CHECK_EQ(s.post("/api/rows/update", R"({"table": "t", "key": [1], "set": {"v": "z"}})").status,
             200);
    const Response page = s.post("/api/browse", R"({"table": "t"})");
    CHECK(page.body.find("[[1,\"z\"],[2,\"b\"]]") != std::string::npos);
    CHECK(page.body.find("\"inTransaction\":true") != std::string::npos);

    CHECK_EQ(s.post("/api/revert", "{}").status, 200);
    const Response again = s.post("/api/browse", R"({"table": "t"})");
    CHECK(again.body.find("[[1,\"a\"],[2,\"b\"]]") != std::string::npos);

    CHECK_EQ(s.post("/api/rows/update", R"({"table": "t", "key": [1], "set": {"v": "z"}})").status,
             200);
    CHECK_EQ(s.post("/api/commit", "{}").status, 200);
    CHECK_EQ(tst::only(s.db.exec("SELECT v FROM t WHERE id = 1")), std::string("z"));
}

TEST(a_failing_request_is_a_4xx_with_a_message)
{
    Running s;
    const Response reply = s.post("/api/sql", R"({"sql": "SELECT * FROM missing"})");
    CHECK_EQ(reply.status, 400);
    CHECK(reply.body.find("\"error\"") != std::string::npos);
    CHECK_EQ(s.post("/api/browse", R"({"table": "missing"})").status, 400);
}

TEST(a_post_must_be_json)
{
    Running s;
    const std::string body = R"({"sql": "DELETE FROM t"})";
    // What a form on another site can send.
    CHECK_EQ(s.send("POST", "/api/sql", body, "Content-Type: text/plain\r\n").status, 415);
    CHECK_EQ(s.send("POST", "/api/sql", body, "Content-Type: application/x-www-form-urlencoded\r\n")
                 .status,
             415);
    CHECK_EQ(s.send("POST", "/api/sql", body).status, 415);
    CHECK_EQ(s.send("POST", "/api/commit", "{}").status, 415);
    CHECK_EQ(s.post("/api/sql", "{not json").status, 400);
    CHECK_EQ(tst::only(s.db.exec("SELECT count(*) FROM t")), std::string("2"));
}

TEST(a_json_content_type_may_carry_a_charset)
{
    Running s;
    const Response reply = s.send("POST", "/api/sql", R"({"sql": "SELECT 1"})",
                                  "Content-Type: Application/JSON; charset=utf-8\r\n");
    CHECK_EQ(reply.status, 200);
}

TEST(a_request_for_another_host_name_is_refused)
{
    Running s;
    CHECK_EQ(s.send("GET", "/api/info", {}, {}, "evil.example").status, 421);
    CHECK_EQ(s.send("GET", "/api/info", {}, {}, "localhost:1234").status, 200);
}

TEST(an_unknown_route_is_404_and_a_wrong_method_is_405)
{
    Running s;
    CHECK_EQ(s.get("/api/nothing").status, 404);
    CHECK_EQ(s.send("PUT", "/api/sql", "{}", "Content-Type: application/json\r\n").status, 405);
    CHECK_EQ(s.get("/api/sql").status, 405);
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
    CHECK_EQ(s.get("/api/info").status, 200);
    CHECK_EQ(s.get("/missing.txt").status, 404);
}

TEST(a_missing_public_directory_still_serves_the_api)
{
    sqlweb::WebOptions options;
    options.publicDir = "/nonexistent/sql-public";
    options.publicDirOverridden = true;
    Running s(options);
    CHECK_EQ(s.get("/").status, 404);
    CHECK_EQ(s.get("/api/info").status, 200);
}

int main()
{
    return tst::runAll("web_server");
}
