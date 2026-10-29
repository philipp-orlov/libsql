// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "web_server.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "managed.hpp"
#ifdef SQL_TOOL_REPORTS
#include "reports.hpp"
#endif
#include "sql/remote/sql_socket.hpp"

#if defined(SQL_TOOL_REPORTS) && defined(SQL_EMBEDDED_FONTS)
// The report fonts (fonts/*.ttf, gzipped), embedded the same way as the page.
extern "C" {
extern const char _binary_LiberationSans_Regular_ttf_gz_start[];
extern const char _binary_LiberationSans_Regular_ttf_gz_end[];
extern const char _binary_LiberationSans_Bold_ttf_gz_start[];
extern const char _binary_LiberationSans_Bold_ttf_gz_end[];
}
#endif

#ifdef SQL_EMBEDDED_INDEX_HTML
// Defined by objcopy (see cmake/EmbedIndexHtml.cmake) from the gzipped public/index.html; they mark where
// the linker put the bytes, they are not real arrays.
extern "C" {
extern const char _binary_index_html_gz_start[];
extern const char _binary_index_html_gz_end[];
}
#include <zlib.h>
#endif

namespace sqlweb {
namespace {

#ifdef SQL_EMBEDDED_INDEX_HTML
// For the rare client that does not accept gzip; inflated once, on first use.
const std::string& embeddedIndexPlain()
{
    static const std::string plain = [] {
        z_stream stream{};
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
            throw std::runtime_error("cannot initialize zlib");
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(_binary_index_html_gz_start));
        stream.avail_in =
            static_cast<uInt>(_binary_index_html_gz_end - _binary_index_html_gz_start);
        std::string out;
        char chunk[64 * 1024];
        int result;
        do {
            stream.next_out = reinterpret_cast<Bytef*>(chunk);
            stream.avail_out = sizeof(chunk);
            result = inflate(&stream, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END) {
                inflateEnd(&stream);
                throw std::runtime_error("the embedded index.html is not valid gzip");
            }
            out.append(chunk, sizeof(chunk) - stream.avail_out);
        } while (result != Z_STREAM_END);
        inflateEnd(&stream);
        return out;
    }();
    return plain;
}
#endif

bool isLoopback(const std::string& host)
{
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

http::ServerConfig makeConfig(const WebOptions& options)
{
    http::ServerConfig config;
    config.host = options.host;
    config.port = options.port;
    config.ioThreads = 2;
    // One worker, so that the one Database handle is only ever used from one thread.
    config.workerThreads = 1;
#ifdef SQL_TOOL_REPORTS
    config.maxBodyBytes = ReportStore::kMaxBytes + 4096;  // report definitions are the only thing put here
#else
    config.maxBodyBytes = 64 * 1024;  // there is nothing to post: the page and the socket
#endif
    config.serverHeader = "sql";
    // A page from another site must not reach an API that listens on a private address (DNS rebinding).
    if (isLoopback(options.host)) {
        config.allowedHosts = {"127.0.0.1", "localhost", "[::1]"};
        // The same for WebSockets, which browsers do not confine by origin: only this page may open one.
        const std::string port = std::to_string(options.port);
        const std::string scheme = options.tlsCertificate.empty() ? "http://" : "https://";
        config.allowedOrigins = {scheme + "127.0.0.1:" + port, scheme + "localhost:" + port, scheme + "[::1]:" + port};
    }
    if (!options.tlsCertificate.empty()) {
#ifdef HTTP_WITH_SSL
        config.httpEnabled = false;
        config.tls = http::TlsConfig{options.port, options.tlsCertificate, options.tlsKey};
#else
        throw std::runtime_error("this build has no TLS (libhttp was built without HTTP_WITH_SSL)");
#endif
    }
    return config;
}

}  // namespace

bool hasReports() noexcept
{
#ifdef SQL_TOOL_REPORTS
    return true;
#else
    return false;
#endif
}

bool hasEmbeddedIndex() noexcept
{
#ifdef SQL_EMBEDDED_INDEX_HTML
    return true;
#else
    return false;
#endif
}

struct WebServer::Impl
{
    Impl(sql::Database& db, WebOptions o)
        : options(std::move(o)),
          server(makeConfig(options))
#ifdef SQL_TOOL_REPORTS
          ,
          reports(reportsDirectory(options, db))
#endif
    {
        registerSocket(db);
#ifdef SQL_TOOL_REPORTS
        registerReports();
#endif
        registerFrontEnd();
    }

#ifdef SQL_TOOL_REPORTS
    static std::filesystem::path reportsDirectory(const WebOptions& options, sql::Database& db)
    {
        if (!options.reportsDir.empty())
            return options.reportsDir;
        const std::string path = db.path().string();
        return path.empty() || path == ":memory:" ? std::filesystem::path("reports") : std::filesystem::path(path + ".reports");
    }

    // ---- reports ----------------------------------------------------------------------------------------------

    static void json(http::HttpContext& context, int status, std::string_view body)
    {
        context.status(status).header("Cache-Control", "no-store").header("X-Content-Type-Options", "nosniff");
        context.text(body, "application/json; charset=utf-8");
    }

    static void failure(http::HttpContext& context, int status, std::string_view message)
    {
        std::string body = "{\"error\":\"";
        for (char c : message) {
            if (c == '"' || c == '\\')
                body += '\\';
            if (static_cast<unsigned char>(c) >= 0x20)
                body += c;
        }
        json(context, status, body + "\"}");
    }

    // The same token as the SQL route's; the page presents it as a bearer header.
    bool authorized(http::HttpContext& context) const
    {
        if (options.token.empty())
            return true;
        const std::string_view header = context.request().headers.get("Authorization");
        const std::string expected = "Bearer " + options.token;
        if (header.size() != expected.size())
            return false;
        unsigned char diff = 0;  // no early exit on the first difference
        for (std::size_t i = 0; i < header.size(); ++i)
            diff |= static_cast<unsigned char>(header[i] ^ expected[i]);
        return diff == 0;
    }

    void registerReports()
    {
        server.onGet("/api/v1/reports", [this](http::HttpContext& context) {
            if (!authorized(context))
                return failure(context, 401, "a token is needed");
            std::string body = "[";
            for (const ReportInfo& r : reports.list()) {
                if (body.size() > 1)
                    body += ',';
                body += "{\"name\":\"" + r.name + "\",\"size\":" + std::to_string(r.size) + ",\"modified\":" + std::to_string(r.modified) + "}";
            }
            json(context, 200, body + "]");
        });
        server.onGet("/api/v1/reports/{name}", [this](http::HttpContext& context) {
            if (!authorized(context))
                return failure(context, 401, "a token is needed");
            const std::string name(context.routeParam("name"));
            if (!ReportStore::validName(name))
                return failure(context, 400, "not a valid report name");
            const auto text = reports.read(name);
            if (!text)
                return failure(context, 404, "no such report");
            json(context, 200, *text);
        });
        server.onPut("/api/v1/reports/{name}", [this](http::HttpContext& context) {
            if (!authorized(context))
                return failure(context, 401, "a token is needed");
            if (options.readOnly)
                return failure(context, 403, "this server is read-only: reports cannot be changed");
            const std::string name(context.routeParam("name"));
            if (!ReportStore::validName(name))
                return failure(context, 400, "not a valid report name");
            if (context.request().headers.get("Content-Type").substr(0, 16) != "application/json")
                return failure(context, 415, "send the report as application/json");
            const std::string_view body = context.request().body.view();
            if (body.size() > ReportStore::kMaxBytes)
                return failure(context, 413, "the report is too large");
            if (!ReportStore::validJson(body))
                return failure(context, 400, "the report is not valid JSON");
            try {
                reports.write(name, body);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "sql: saving report %s: %s\n", name.c_str(), e.what());
                return failure(context, 500, "the report could not be saved");
            }
            json(context, 200, "{}");
        });
        server.onDelete("/api/v1/reports/{name}", [this](http::HttpContext& context) {
            if (!authorized(context))
                return failure(context, 401, "a token is needed");
            if (options.readOnly)
                return failure(context, 403, "this server is read-only: reports cannot be changed");
            const std::string name(context.routeParam("name"));
            if (!ReportStore::validName(name))
                return failure(context, 400, "not a valid report name");
            if (!reports.remove(name))
                return failure(context, 404, "no such report");
            json(context, 200, "{}");
        });
#ifdef SQL_EMBEDDED_FONTS
        // The faces reports are drawn and exported with. The names never change for a version, so they may be cached.
        auto font = [](const char* begin, const char* end) {
            return [begin, end](http::HttpContext& context) {
                context.header("Cache-Control", "public, max-age=86400").header("Content-Encoding", "gzip").header("X-Content-Type-Options", "nosniff");
                context.bytes(begin, static_cast<std::size_t>(end - begin), "font/ttf");
            };
        };
        server.onGet("/fonts/LiberationSans-Regular.ttf", font(_binary_LiberationSans_Regular_ttf_gz_start, _binary_LiberationSans_Regular_ttf_gz_end));
        server.onGet("/fonts/LiberationSans-Bold.ttf", font(_binary_LiberationSans_Bold_ttf_gz_start, _binary_LiberationSans_Bold_ttf_gz_end));
#endif
    }
#endif  // SQL_TOOL_REPORTS

    // The database over a WebSocket route, run on the server's one worker: every session shares the one Database
    // handle, so changes pending through any of them are the same transaction.
    void registerSocket(sql::Database& db)
    {
        sql::remote::SqlSocketOptions socketOptions;
        socketOptions.isolation(sql::remote::Isolation::Shared)
            .readOnly(options.readOnly)
            .interceptor(std::make_shared<ManagedExec>())
            .executor([this](std::function<void()> task) { return server.pool().tryEnqueue(std::move(task)); })
            .serverName(std::string("sql/") + sql::version());
        if (!options.token.empty()) {
            const std::string token = options.token;
            socketOptions
                .authorize([token](const sql::remote::Handshake& h) -> sql::remote::SqlSocketOptions::AuthorizeResult {
                    const std::string_view header = h.header("Authorization");
                    if (header.empty())
                        return 0;  // a browser: the token comes in the first message
                    if (header == "Bearer " + token)
                        return sql::remote::Principal{"token", false, nullptr};
                    return 401;
                })
                .authenticate([token](std::string_view given) -> std::optional<sql::remote::Principal> {
                    if (given == token)
                        return sql::remote::Principal{"token", false, nullptr};
                    return std::nullopt;
                })
                .requireAuth();
        }
        sqlSocket = std::make_unique<sql::remote::SqlSocket>(db, std::move(socketOptions));
        sqlSocket->attach(server, options.socketPath);
        if (!options.unixSocket.empty())
            sqlSocket->listenLocal(options.unixSocket);
    }

    void shutdownSocket()
    {
        if (sqlSocket)
            sqlSocket->shutdown(std::chrono::milliseconds(1000));
    }

    // index.html keeps its name across a release, unlike the files it may load, so it is never cached.
    void serveIndex(http::HttpContext& context) const
    {
        context.header("Cache-Control", "no-cache").header("X-Frame-Options", "DENY");
        context.header("X-Content-Type-Options", "nosniff");
#ifdef SQL_EMBEDDED_INDEX_HTML
        if (!options.publicDirOverridden) {
            context.header("Vary", "Accept-Encoding");
            if (context.request().headers.get("Accept-Encoding").find("gzip") !=
                std::string_view::npos) {
                context.header("Content-Encoding", "gzip");
                context.bytes(_binary_index_html_gz_start,
                              _binary_index_html_gz_end - _binary_index_html_gz_start,
                              "text/html; charset=utf-8");
            } else {
                context.text(embeddedIndexPlain(), "text/html; charset=utf-8");
            }
            return;
        }
#endif
        std::ifstream file(options.publicDir + "/index.html", std::ios::binary);
        if (!file) {
            context.status(404).text("Not Found");
            return;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        context.text(contents.str(), "text/html; charset=utf-8");
    }

    void registerFrontEnd()
    {
        for (const char* path : {"/", "/index.html"}) {
            server.onGet(path, [this](http::HttpContext& context) { serveIndex(context); });
            server.onHead(path, [this](http::HttpContext& context) { serveIndex(context); });
        }
#ifdef SQL_TOOL_REPORTS
        // /reports is the runner alone, for customers (the same page; it looks at its address to decide).
        for (const char* path : {"/reports", "/reports/"}) {
            server.onGet(path, [this](http::HttpContext& context) { serveIndex(context); });
            server.onHead(path, [this](http::HttpContext& context) { serveIndex(context); });
        }
#endif
        const bool haveDirectory = std::filesystem::is_directory(options.publicDir);
        if (haveDirectory)
            server.onStaticFiles("/", options.publicDir);
        if (!haveDirectory && (options.publicDirOverridden || !hasEmbeddedIndex()))
            std::fprintf(stderr,
                         "sql: the public directory %s does not exist; serving the database socket only\n",
                         options.publicDir.c_str());
    }

    WebOptions options;
    http::Server server;
#ifdef SQL_TOOL_REPORTS
    ReportStore reports;
#endif
    std::unique_ptr<sql::remote::SqlSocket> sqlSocket;  // after the server: it goes first, after shutdownSocket()
};

WebServer::WebServer(sql::Database& db, WebOptions options)
    : impl_(std::make_unique<Impl>(db, std::move(options)))
{}

WebServer::~WebServer()
{
    if (impl_)
        impl_->shutdownSocket();
}

void WebServer::run(std::function<bool()> shouldStop, std::function<void()> onListening)
{
    impl_->server.run(std::move(shouldStop), std::move(onListening));
}

void WebServer::stop()
{
    impl_->shutdownSocket();
    impl_->server.stop();
}

}  // namespace sqlweb
