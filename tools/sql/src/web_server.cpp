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
#include "json/json.hpp"
#include "web_api.hpp"

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

using json::Json;

constexpr std::size_t kMaxBodyBytes = 16u * 1024 * 1024;

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
    config.maxBodyBytes = kMaxBodyBytes;
    config.serverHeader = "sql";
    // A page from another site must not reach an API that listens on a private address (DNS rebinding).
    if (isLoopback(options.host))
        config.allowedHosts = {"127.0.0.1", "localhost", "[::1]"};
    return config;
}

void reject(http::HttpContext& context, const std::function<void()>& done, int status,
            const std::string& message)
{
    context.status(status).header("Cache-Control", "no-store").json(Json{{"error", message}});
    done();
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
    using Query = Reply (WebApi::*)();
    using Command = Reply (WebApi::*)(const Json&);

    Impl(sql::Database& db, WebOptions o)
        : options(std::move(o)), api(db, options.readOnly), server(makeConfig(options))
    {
        registerApi();
        registerFrontEnd();
    }

    // Runs `call` on the worker and writes its reply from the connection's own loop.
    template <class Call>
    void offload(http::HttpContext& context, std::function<void()> done, Call call)
    {
        http::IoLoop& loop = context.loop();
        const bool queued = server.pool().tryEnqueue([&context, &loop, done,
                                                      call = std::move(call)]() mutable {
            Reply reply = call();
            loop.tryPost([&context, done, reply = std::move(reply)]() mutable {
                context.status(reply.status).header("Cache-Control", "no-store").json(reply.body);
                done();
            });
        });
        if (!queued)
            reject(context, done, 503, "the server is busy");
    }

    // A browser form on another site can send a POST, but not one with this content type.
    static bool isJson(http::HttpContext& context)
    {
        const std::string_view type = context.request().headers.get("Content-Type");
        return http::equalsIgnoreCase(type.substr(0, 16), "application/json");
    }

    void get(const char* path, Query query)
    {
        server.onGet(path, [this, query](http::HttpContext& context, std::function<void()> done) {
            offload(context, std::move(done), [this, query] { return (api.*query)(); });
        });
    }

    void post(const char* path, Command command)
    {
        server.onPost(path, [this, command](http::HttpContext& context,
                                            std::function<void()> done) {
            if (!isJson(context))
                return reject(context, done, 415, "the Content-Type must be application/json");
            Json body;
            std::string error;
            if (!Json::parse(context.request().body.view(), body, error))
                return reject(context, done, 400, "the request body is not valid JSON: " + error);
            offload(context, std::move(done),
                    [this, command, body = std::move(body)] { return (api.*command)(body); });
        });
    }

    void post(const char* path, Query query)
    {
        server.onPost(path, [this, query](http::HttpContext& context, std::function<void()> done) {
            if (!isJson(context))
                return reject(context, done, 415, "the Content-Type must be application/json");
            offload(context, std::move(done), [this, query] { return (api.*query)(); });
        });
    }

    void registerApi()
    {
        get("/api/info", &WebApi::info);
        get("/api/schema", &WebApi::schema);
        get("/api/dump", &WebApi::dump);
        get("/api/pragmas", &WebApi::pragmas);
        post("/api/sql", &WebApi::execute);
        post("/api/browse", &WebApi::browse);
        post("/api/rows/insert", &WebApi::insertRow);
        post("/api/rows/update", &WebApi::updateRow);
        post("/api/rows/delete", &WebApi::deleteRows);
        post("/api/pragmas", &WebApi::setPragma);
        post("/api/commit", &WebApi::commit);
        post("/api/revert", &WebApi::revert);
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
                         "sql: the public directory %s does not exist; serving the API only\n",
                         options.publicDir.c_str());
    }

    WebOptions options;
    WebApi api;
    http::Server server;
};

WebServer::WebServer(sql::Database& db, WebOptions options)
    : impl_(std::make_unique<Impl>(db, std::move(options)))
{}

WebServer::~WebServer() = default;

void WebServer::run(std::function<bool()> shouldStop)
{
    impl_->server.run(std::move(shouldStop));
}

void WebServer::stop()
{
    impl_->server.stop();
}

}  // namespace sqlweb
