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
#include "sql/remote/sql_socket.hpp"

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
    config.maxBodyBytes = 64 * 1024;  // there is nothing to post: the page and the socket
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
    Impl(sql::Database& db, WebOptions o) : options(std::move(o)), server(makeConfig(options))
    {
        registerSocket(db);
        registerFrontEnd();
    }

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

void WebServer::run(std::function<bool()> shouldStop)
{
    impl_->server.run(std::move(shouldStop));
}

void WebServer::stop()
{
    impl_->shutdownSocket();
    impl_->server.stop();
}

}  // namespace sqlweb
