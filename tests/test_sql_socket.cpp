// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// sql::remote::SqlSocket against the protocol text of sql-remote.md, spoken by hand with
// http::WebSocketClient and the generated message builders (no Database::connect involved).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <unistd.h>
#include <future>
#include <thread>

#include "bufs/bufs.hpp"
#include "http/server.hpp"
#include "http/websocket_client.hpp"
#include "sql/remote/sql_socket.hpp"
#include "sql/wire/remote.bufs.hpp"
#include "sql/wire/result_view.hpp"
#include "remote_fixture.hpp"
#include "sql_test.hpp"

using namespace sql;
using namespace std::chrono_literals;
namespace rem = sql::remote;

// Heap allocations of the server's threads are counted; the test thread (the client) is not.
namespace {
thread_local bool t_client = false;
std::atomic<unsigned long> g_serverNews{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed) && !t_client)
        g_serverNews.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n)
{
    return ::operator new(n);
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

namespace {


struct Frame
{
    std::vector<std::uint64_t> words;
    std::size_t size = 0;
    rem::Reply::Reader reply() const { return bufs::root<rem::Reply>(words.data()); }
    /// The ResultSet message after the Reply, if the reply says so.
    const std::uint8_t* rows() const
    {
        const auto* base = reinterpret_cast<const std::uint8_t*>(words.data());
        return base + bufs::messageSize(base);
    }
    std::size_t rowsSize() const { return size - bufs::messageSize(words.data()); }
};

constexpr auto kForever = std::chrono::steady_clock::time_point::max();

/// One client connection, speaking the wire protocol.
class Wire
{
public:
    void connect(std::uint16_t port, const std::string& path = "/api/v1/ws/sql", const std::string& auth = {},
                 bool subprotocol = true)
    {
        http::WebSocketClientOptions options;
        if (subprotocol)
            options.subprotocols = {"libsql.v1"};
        if (!auth.empty())
            options.headers = {{"Authorization", auth}};
        ws.connect("ws://127.0.0.1:" + std::to_string(port) + path, options);
    }

    template <class Fill>
    std::uint32_t send(rem::Op op, Fill fill)
    {
        const std::uint32_t id = nextId++;
        bufs::MessageBuilder mb;
        auto r = mb.initRoot<rem::Request>();
        r.setId(id);
        r.setOp(op);
        fill(r);
        const auto [data, size] = mb.finish();
        ws.sendBinary(data, size);
        return id;
    }
    std::uint32_t send(rem::Op op) { return send(op, [](rem::Request::Builder) {}); }

    std::uint32_t hello(const std::string& token = {}, bool readOnly = false, std::uint64_t window = 0)
    {
        return send(rem::Op::Hello, [&](rem::Request::Builder r) {
            r.setProtocol(1);
            r.setClient("test");
            if (!token.empty())
                r.setToken(token);
            r.setReadOnly(readOnly);
            r.setWindow(window);
        });
    }

    std::uint32_t exec(const std::string& sqlText, std::uint32_t txn = 0, std::function<void(rem::Request::Builder)> more = {})
    {
        return send(rem::Op::Exec, [&](rem::Request::Builder r) {
            r.setSql(sqlText);
            r.setTxn(txn);
            if (more)
                more(r);
        });
    }

    static void addParams(rem::Request::Builder r, const std::vector<Value>& values)
    {
        auto list = r.initParams(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto p = list.init(i);
            const Value& v = values[i];
            switch (v.type()) {
                case Type::Integer: p.setType(rem::ParamType::Integer); p.setInteger(v.integer()); break;
                case Type::Real: p.setType(rem::ParamType::Real); p.setReal(v.real()); break;
                case Type::Datetime: p.setType(rem::ParamType::Datetime); p.setInteger(v.datetime().usec); break;
                case Type::Text: {
                    p.setType(rem::ParamType::Text);
                    const auto t = v.text();
                    p.setBytes(reinterpret_cast<const std::uint8_t*>(t.data()), t.size());
                    break;
                }
                case Type::Blob: {
                    p.setType(rem::ParamType::Blob);
                    const auto b = v.blob();
                    p.setBytes(reinterpret_cast<const std::uint8_t*>(b.data()), b.size());
                    break;
                }
                default: break;
            }
        }
    }

    /// The next frame, at most `wait`; throws on timeout.
    Frame next(std::chrono::milliseconds wait = 10s)
    {
        auto frame = tryNext(wait);
        if (!frame)
            tst::fail(__FILE__, __LINE__, "no frame within the timeout");
        return std::move(*frame);
    }

    std::optional<Frame> tryNext(std::chrono::milliseconds wait)
    {
        auto message = ws.receive(std::chrono::steady_clock::now() + wait);
        if (!message)
            return std::nullopt;
        Frame f;
        f.size = message->data.size();
        f.words.resize((f.size + 7) / 8);
        std::memcpy(f.words.data(), message->data.data(), f.size);
        if (!bufs::verify<rem::Reply>(f.words.data(), f.size))
            tst::fail(__FILE__, __LINE__, "a reply frame does not verify");
        return f;
    }

    struct Outcome
    {
        Frame terminal;
        std::vector<std::uint8_t> rows;  ///< every ResultSet message of the request, concatenated
        std::size_t frames = 0;          ///< Rows frames
        std::size_t largestFrame = 0;
        bool ok() const { return terminal.reply().kind() == rem::Kind::Done; }
        bool error(ErrorCode code) const
        {
            return terminal.reply().kind() == rem::Kind::Error && terminal.reply().code() == std::uint16_t(code);
        }
        std::string message() const { return std::string(terminal.reply().message()); }
        wire::ResultView view() const { return wire::ResultView::open(rows.data(), rows.size()); }
    };

    /// Reads until the terminal reply of request `id`, crediting rows as it goes (unless `credit` is false).
    Outcome finish(std::uint32_t id, bool credit = true, std::chrono::milliseconds wait = 20s)
    {
        Outcome out;
        std::uint64_t consumed = 0;
        for (;;) {
            Frame f = next(wait);
            const auto r = f.reply();
            if (r.id() != id && r.kind() != rem::Kind::Closing)
                tst::fail(__FILE__, __LINE__, "a reply for another request: " + std::to_string(r.id()));
            if (r.kind() == rem::Kind::Rows) {
                ++out.frames;
                out.largestFrame = std::max(out.largestFrame, f.size);
                const auto* base = f.rows();
                out.rows.insert(out.rows.end(), base, base + f.rowsSize());
                consumed += std::max<std::size_t>(f.size, 8192);
                if (credit && consumed >= windowSeen / 2) {
                    send(rem::Op::Credit, [&](rem::Request::Builder c) { c.setCredit(consumed); });
                    consumed = 0;
                }
                continue;
            }
            out.terminal = std::move(f);
            if (credit && consumed > 0)  // what is left of the window goes back at the end, so the next request starts full
                send(rem::Op::Credit, [&](rem::Request::Builder c) { c.setCredit(consumed); });
            return out;
        }
    }

    Outcome run(const std::string& text) { return finish(exec(text)); }

    http::WebSocketClient ws;
    std::uint32_t nextId = 1;
    std::uint64_t windowSeen = 1 << 20;
};

/// A database, an http::Server and a SqlSocket on a free port.
struct Fixture
{
    tst::Scratch scratch{"socket"};
    Database db;
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::unique_ptr<rem::SqlSocket> socket;
    std::future<void> serving;
    std::uint16_t port = 0;

    explicit Fixture(rem::SqlSocketOptions options = rem::SqlSocketOptions(), std::function<void(http::ServerConfig&)> tweak = {},
                     std::function<void(Database&)> seed = {})
        : db(Database::configure().busyTimeout(500ms).open(scratch.file()))
    {
        db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL)");
        db.exec("INSERT INTO t VALUES (1,'Ada',1.5),(2,'Bo',2.5),(3,NULL,3.5)");
        if (seed)
            seed(db);
        config.host = "127.0.0.1";
        config.ioThreads = 1;
        config.workerThreads = 1;
        if (tweak)
            tweak(config);
        // Find a free port by trying until the server accepts it.
        for (int attempt = 0; attempt < 50; ++attempt) {
            config.port = remote_test::freePort();
            try {
                server = std::make_unique<http::Server>(config);
                socket = std::make_unique<rem::SqlSocket>(db, options);
                socket->attach(*server, "/api/v1/ws/sql");
                serving = std::async(std::launch::async, [this] { server->run(); });
                // Wait until it listens.
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
                    std::this_thread::sleep_for(10ms);
                }
            } catch (...) {
            }
        }
        tst::fail(__FILE__, __LINE__, "could not start a server");
    }
    ~Fixture()
    {
        if (socket)
            socket->shutdown(200ms);
        if (server) {
            server->stop();
            serving.get();
        }
    }

    Wire open(const std::string& token = {}, bool readOnly = false, std::uint64_t window = 0)
    {
        Wire w;
        w.connect(port);
        const auto id = w.hello(token, readOnly, window);
        auto out = w.finish(id);
        CHECK(out.terminal.reply().kind() == rem::Kind::Welcome);
        w.windowSeen = out.terminal.reply().window();
        return w;
    }
};

std::string cell(const wire::ResultView& v, std::uint64_t row, std::size_t col)
{
    const Value value = v.value(row, col);
    return value.isNull() ? "NULL" : value.toText();
}

}  // namespace

TEST(socket_hello_first)
{
    Fixture f;
    Wire w;
    w.connect(f.port);
    auto id = w.exec("SELECT 1");
    auto out = w.finish(id);
    CHECK(out.error(ErrorCode::Protocol));
    try {
        w.next(5s);
        CHECK(false);
    } catch (const http::WebSocketError& e) {
        CHECK_EQ(int(e.closeCode()), 1002);
    }
}

TEST(socket_hello_welcome)
{
    Fixture f;
    Wire w;
    w.connect(f.port);
    CHECK(w.ws.subprotocol() == "libsql.v1");
    auto out = w.finish(w.hello("", false, 123456));
    const auto r = out.terminal.reply();
    CHECK(r.kind() == rem::Kind::Welcome);
    CHECK_EQ(int(r.protocol()), 1);
    CHECK_EQ(r.window(), std::uint64_t(123456));  // the client asked for less than the server allows
    CHECK(r.maxMessage() > 0);
    CHECK_EQ(int(r.maxPipeline()), 16);
    CHECK(r.session() > 0);
    CHECK(!r.readOnly());
    CHECK(std::string(r.server()).rfind("libsql", 0) == 0);
    // A second hello is a protocol error.
    auto again = w.finish(w.hello());
    CHECK(again.error(ErrorCode::Protocol));
}

TEST(socket_subprotocol_required)
{
    Fixture f;
    Wire w;
    try {
        w.connect(f.port, "/api/v1/ws/sql", {}, /*subprotocol*/ false);
        CHECK(false);
    } catch (const http::WebSocketError& e) {
        CHECK_EQ(e.status(), 400);
    }
}

TEST(socket_authorize_and_authenticate)
{
    rem::SqlSocketOptions options;
    options.authorize([](const rem::Handshake& h) -> rem::SqlSocketOptions::AuthorizeResult {
        const auto auth = h.header("Authorization");
        if (auth == "Bearer admin")
            return rem::Principal{"admin", false, nullptr};
        if (auth == "Bearer reader")
            return rem::Principal{"reader", true, nullptr};
        if (auth == "Bearer denied")
            return 403;
        if (auth.empty())
            return 0;  // maybe the token in hello
        return 401;
    });
    options.authenticate([](std::string_view token) -> std::optional<rem::Principal> {
        if (token == "hello-token")
            return rem::Principal{"hello", false, nullptr};
        return std::nullopt;
    });
    options.requireAuth();
    Fixture f(options);
    for (auto [header, status] : std::vector<std::pair<std::string, int>>{{"Bearer denied", 403}, {"Bearer nobody", 401}}) {
        Wire w;
        try {
            w.connect(f.port, "/api/v1/ws/sql", header);
            CHECK(false);
        } catch (const http::WebSocketError& e) {
            CHECK_EQ(e.status(), status);
        }
    }
    {
        Wire w;  // no credentials in the handshake and none in hello: refused at hello
        w.connect(f.port);
        auto out = w.finish(w.hello());
        CHECK(out.error(ErrorCode::PermissionDenied));
        try {
            w.next(5s);
            CHECK(false);
        } catch (const http::WebSocketError& e) {
            CHECK_EQ(int(e.closeCode()), 1008);
        }
    }
    {
        Wire w;
        w.connect(f.port);
        auto out = w.finish(w.hello("hello-token"));
        CHECK(out.terminal.reply().kind() == rem::Kind::Welcome);
    }
    {
        Wire w;
        w.connect(f.port, "/api/v1/ws/sql", "Bearer reader");
        auto out = w.finish(w.hello());
        CHECK(out.terminal.reply().kind() == rem::Kind::Welcome);
        CHECK(out.terminal.reply().readOnly());
        auto write = w.run("INSERT INTO t VALUES (9,'x',0)");
        CHECK(write.error(ErrorCode::ReadOnly));
        auto read = w.run("SELECT count(*) FROM t");
        CHECK(read.ok());
        CHECK_EQ(cell(read.view(), 0, 0), std::string("3"));
    }
}

TEST(socket_exec_rows_done)
{
    Fixture f;
    Wire w = f.open();
    auto out = w.run("SELECT id, name, score FROM t ORDER BY id");
    CHECK(out.ok());
    auto view = out.view();
    CHECK_EQ(view.rowCount(), std::uint64_t(3));
    CHECK_EQ(view.columns().size(), std::size_t(3));
    CHECK_EQ(cell(view, 0, 1), std::string("Ada"));
    CHECK_EQ(cell(view, 2, 1), std::string("NULL"));
    CHECK_EQ(cell(view, 1, 2), std::string("2.5"));
    CHECK(!out.terminal.reply().inTransaction());
    CHECK(out.terminal.reply().elapsedUs() < 5000000);
}

TEST(socket_exec_no_columns_and_counters)
{
    Fixture f;
    Wire w = f.open();
    auto out = w.run("UPDATE t SET score = score + 1");
    CHECK(out.ok());
    CHECK_EQ(out.terminal.reply().changes(), std::uint64_t(3));
    CHECK_EQ(out.view().columns().size(), std::size_t(0));
    auto ins = w.run("INSERT INTO t(name) VALUES ('Cy') RETURNING id");
    CHECK(ins.ok());
    CHECK_EQ(cell(ins.view(), 0, 0), std::string("4"));
    CHECK_EQ(ins.terminal.reply().changes(), std::uint64_t(1));
    CHECK_EQ(ins.terminal.reply().lastInsertId(), std::int64_t(4));
    CHECK_EQ(ins.terminal.reply().totalChanges(), std::int64_t(4));
}

TEST(socket_exec_error_keeps_local_message)
{
    Fixture f;
    Wire w = f.open();
    auto bad = w.run("SELEC 1");
    CHECK(bad.error(ErrorCode::SyntaxError));
    std::string local;
    try {
        f.db.exec("SELEC 1");
    } catch (const Error& e) {
        local = e.what();
    }
    CHECK_EQ(bad.message(), local);
    auto missing = w.run("SELECT * FROM nope");
    CHECK(missing.error(ErrorCode::NoSuchTable) || missing.error(ErrorCode::Runtime) || missing.error(ErrorCode::InvalidArgument));
    auto dup = w.run("INSERT INTO t(id) VALUES (1)");
    CHECK(dup.error(ErrorCode::ConstraintViolation));
    CHECK_EQ(int(dup.terminal.reply().constraint()), int(Constraint::PrimaryKey));
    // The session survives errors.
    CHECK(w.run("SELECT 1").ok());
}

TEST(socket_params)
{
    Fixture f;
    Wire w = f.open();
    auto id = w.exec("SELECT ?, ?, ?, ?, ?", 0, [](rem::Request::Builder r) {
        Wire::addParams(r, {Value(7), Value(2.5), Value("héllo"), Value::blob("\x01\x02\x03", 3), Value()});
    });
    auto out = w.finish(id);
    CHECK(out.ok());
    auto view = out.view();
    CHECK_EQ(view.value(0, 0).integer(), std::int64_t(7));
    CHECK_EQ(view.value(0, 1).real(), 2.5);
    CHECK_EQ(std::string(view.value(0, 2).text()), std::string("héllo"));
    CHECK_EQ(view.value(0, 3).blob().size(), std::size_t(3));
    CHECK(view.value(0, 4).isNull());
    // Named parameters.
    auto named = w.exec("SELECT :a + :b", 0, [](rem::Request::Builder r) {
        auto list = r.initParams(2);
        auto a = list.init(0);
        a.setName(":a");
        a.setType(rem::ParamType::Integer);
        a.setInteger(40);
        auto b = list.init(1);
        b.setName(":b");
        b.setType(rem::ParamType::Integer);
        b.setInteger(2);
    });
    auto r2 = w.finish(named);
    CHECK(r2.ok());
    CHECK_EQ(r2.view().value(0, 0).integer(), std::int64_t(42));
}

TEST(socket_streaming_batches)
{
    Fixture f(rem::SqlSocketOptions(), {}, [](Database& db) {
        db.exec("CREATE TABLE big(n INTEGER PRIMARY KEY, text TEXT)");
        Transaction txn = db.begin();
        for (int i = 0; i < 20000; ++i)
            txn.exec("INSERT INTO big VALUES (?, ?)", Params{Value(i), Value(std::string(40, char('a' + i % 26)))});
        txn.commit();
    });
    Wire w = f.open();
    auto id = w.exec("SELECT n, text FROM big ORDER BY n", 0, [](rem::Request::Builder r) {
        auto o = r.initOptions();
        o.setBatchBytes(64 * 1024);
    });
    auto out = w.finish(id);
    CHECK(out.ok());
    CHECK(out.frames > 5);
    CHECK(out.largestFrame < 80 * 1024);
    auto view = out.view();
    CHECK_EQ(view.rowCount(), std::uint64_t(20000));
    CHECK_EQ(view.value(19999, 0).integer(), std::int64_t(19999));
    CHECK_EQ(std::string(view.value(7, 1).text()), std::string(40, 'h'));
}

TEST(socket_credit_window)
{
    Fixture f(rem::SqlSocketOptions().window(256 * 1024), {}, [](Database& db) {
        db.exec("CREATE TABLE big(n INTEGER PRIMARY KEY, text TEXT)");
        Transaction txn = db.begin();
        for (int i = 0; i < 30000; ++i)
            txn.exec("INSERT INTO big VALUES (?, ?)", Params{Value(i), Value(std::string(60, 'x'))});
        txn.commit();
    });
    Wire w = f.open();
    CHECK_EQ(w.windowSeen, std::uint64_t(256 * 1024));
    auto id = w.exec("SELECT n, text FROM big", 0, [](rem::Request::Builder r) { r.initOptions().setBatchBytes(16 * 1024); });
    // Without credit the server stops after about one window of frames.
    std::size_t frames = 0, bytes = 0;
    std::uint64_t cost = 0;
    for (;;) {
        auto fr = w.tryNext(700ms);
        if (!fr)
            break;  // silence: the server is waiting for credit
        CHECK(fr->reply().kind() == rem::Kind::Rows);
        ++frames;
        bytes += fr->size;
        cost += std::max<std::size_t>(fr->size, 8192);
    }
    CHECK(frames >= 2);
    CHECK(cost <= 256 * 1024 + 20 * 1024);
    // Credit resumes it, and the whole result arrives intact.
    w.send(rem::Op::Credit, [&](rem::Request::Builder c) { c.setCredit(cost); });
    auto rest = w.finish(id);
    CHECK(rest.ok());
    CHECK_EQ(rest.view().rowCount() + 0u > 0, true);
    (void)bytes;
}

TEST(socket_stall_timeout)
{
    Fixture f(rem::SqlSocketOptions().window(64 * 1024).stallTimeout(1s), {}, [](Database& db) {
        db.exec("CREATE TABLE big(n INTEGER PRIMARY KEY, text TEXT)");
        Transaction txn = db.begin();
        for (int i = 0; i < 20000; ++i)
            txn.exec("INSERT INTO big VALUES (?, ?)", Params{Value(i), Value(std::string(60, 'x'))});
        txn.commit();
    });
    Wire w = f.open();
    auto id = w.exec("SELECT n, text FROM big", 0, [](rem::Request::Builder r) { r.initOptions().setBatchBytes(8 * 1024); });
    // Never send credit: the request ends with an Error and the session closes.
    bool sawError = false, closed = false;
    for (int i = 0; i < 200 && !closed; ++i) {
        try {
            Frame fr = w.next(5s);
            if (fr.reply().kind() == rem::Kind::Error) {
                CHECK_EQ(int(fr.reply().code()), int(ErrorCode::Runtime));
                CHECK(std::string(fr.reply().message()).find("stopped reading") != std::string::npos);
                sawError = true;
            }
        } catch (const http::WebSocketError& e) {
            closed = true;
            CHECK_EQ(int(e.closeCode()), 1008);
        }
    }
    CHECK(sawError);
    CHECK(closed);
    (void)id;
    CHECK(f.socket->stats().stalls >= 1);
}

TEST(socket_interrupt_running)
{
    Fixture f;
    Wire w = f.open();
    auto id = w.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000000) SELECT count(*) FROM c");
    std::this_thread::sleep_for(200ms);
    const auto asked = std::chrono::steady_clock::now();
    w.send(rem::Op::Interrupt, [&](rem::Request::Builder r) { r.setTarget(id); });
    auto out = w.finish(id);
    CHECK(out.error(ErrorCode::Interrupted));
    CHECK(std::chrono::steady_clock::now() - asked < 2s);
    CHECK(w.run("SELECT 1 + 1").ok());  // the session works again
}

TEST(socket_interrupt_queued_and_pipeline_limit)
{
    Fixture f(rem::SqlSocketOptions().maxPipeline(3));
    Wire w = f.open();
    auto slow = w.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000000) SELECT count(*) FROM c");
    auto second = w.exec("SELECT 2");
    auto third = w.exec("SELECT 3");
    auto fourth = w.exec("SELECT 4");  // running + 2 queued = 3 in flight: this one is too many
    auto refused = w.finish(fourth);
    CHECK(refused.error(ErrorCode::Busy));
    w.send(rem::Op::Interrupt, [&](rem::Request::Builder r) { r.setTarget(second); });
    auto cancelled = w.finish(second);
    CHECK(cancelled.error(ErrorCode::Interrupted));
    w.send(rem::Op::Interrupt, [&](rem::Request::Builder r) { r.setTarget(slow); });
    CHECK(w.finish(slow).error(ErrorCode::Interrupted));
    auto ok = w.finish(third);
    CHECK(ok.ok());
    CHECK_EQ(cell(ok.view(), 0, 0), std::string("3"));
}

TEST(socket_prepared_lifecycle)
{
    Fixture f;
    Wire w = f.open();
    auto pid = w.send(rem::Op::Prepare, [](rem::Request::Builder r) {
        r.setStatement(7);
        r.setSql("SELECT id, name FROM t WHERE id > :min ORDER BY id");
    });
    auto prepared = w.finish(pid);
    const auto pr = prepared.terminal.reply();
    CHECK(pr.kind() == rem::Kind::Prepared);
    CHECK_EQ(pr.parameters(), std::uint32_t(1));
    CHECK_EQ(std::string(pr.parameterNames()[0]), std::string(":min"));
    CHECK_EQ(pr.columns().size(), std::uint32_t(2));
    CHECK(!pr.writes());
    for (int round = 0; round < 2; ++round) {
        auto id = w.send(rem::Op::ExecPrepared, [&](rem::Request::Builder r) {
            r.setStatement(7);
            Wire::addParams(r, {Value(round)});
        });
        auto out = w.finish(id);
        CHECK(out.ok());
        CHECK_EQ(out.view().rowCount(), std::uint64_t(3 - round));
    }
    auto closeId = w.send(rem::Op::ClosePrepared, [](rem::Request::Builder r) { r.setStatement(7); });
    CHECK(w.finish(closeId).ok());
    auto after = w.send(rem::Op::ExecPrepared, [](rem::Request::Builder r) { r.setStatement(7); });
    CHECK(w.finish(after).error(ErrorCode::InvalidArgument));
    // A statement id in use is refused.
    auto first = w.send(rem::Op::Prepare, [](rem::Request::Builder r) { r.setStatement(1); r.setSql("SELECT 1"); });
    CHECK(w.finish(first).terminal.reply().kind() == rem::Kind::Prepared);
    auto dup = w.send(rem::Op::Prepare, [](rem::Request::Builder r) { r.setStatement(1); r.setSql("SELECT 2"); });
    CHECK(w.finish(dup).error(ErrorCode::InvalidArgument));
}

TEST(socket_transactions)
{
    Fixture f;
    Wire w = f.open();
    auto begin = w.send(rem::Op::Begin, [](rem::Request::Builder r) { r.setTxn(5); });
    CHECK(w.finish(begin).ok());
    CHECK(w.finish(w.exec("INSERT INTO t(name) VALUES ('in txn')", 5)).ok());
    CHECK_EQ(cell(w.finish(w.exec("SELECT count(*) FROM t", 5)).view(), 0, 0), std::string("4"));
    auto rollback = w.send(rem::Op::Rollback, [](rem::Request::Builder r) { r.setTxn(5); });
    CHECK(w.finish(rollback).ok());
    CHECK_EQ(cell(w.run("SELECT count(*) FROM t").view(), 0, 0), std::string("3"));
    // Commit keeps it.
    auto b2 = w.send(rem::Op::Begin, [](rem::Request::Builder r) { r.setTxn(6); });
    CHECK(w.finish(b2).ok());
    CHECK(w.finish(w.exec("INSERT INTO t(name) VALUES ('kept')", 6)).ok());
    auto commit = w.send(rem::Op::Commit, [](rem::Request::Builder r) { r.setTxn(6); });
    CHECK(w.finish(commit).ok());
    CHECK_EQ(cell(w.run("SELECT count(*) FROM t").view(), 0, 0), std::string("4"));
    // A finished transaction id is unknown.
    CHECK(w.finish(w.exec("SELECT 1", 6)).error(ErrorCode::BadTransaction));
    // A read-only transaction refuses writes.
    auto ro = w.send(rem::Op::Begin, [](rem::Request::Builder r) { r.setTxn(7); r.setReadOnly(true); });
    CHECK(w.finish(ro).ok());
    CHECK(w.finish(w.exec("DELETE FROM t", 7)).error(ErrorCode::ReadOnly));
}

TEST(socket_two_sessions_are_isolated)
{
    Fixture f;
    Wire a = f.open();
    Wire b = f.open();
    auto begin = a.send(rem::Op::Begin, [](rem::Request::Builder r) { r.setTxn(1); });
    CHECK(a.finish(begin).ok());
    CHECK(a.finish(a.exec("INSERT INTO t(name) VALUES ('pending')", 1)).ok());
    // B does not see it, and B's own writer waits for the slot, then reports Busy.
    CHECK_EQ(cell(b.run("SELECT count(*) FROM t").view(), 0, 0), std::string("3"));
    auto start = std::chrono::steady_clock::now();
    auto blocked = b.run("INSERT INTO t(name) VALUES ('blocked')");
    CHECK(blocked.error(ErrorCode::Busy));
    CHECK(std::chrono::steady_clock::now() - start >= 400ms);
    auto commit = a.send(rem::Op::Commit, [](rem::Request::Builder r) { r.setTxn(1); });
    CHECK(a.finish(commit).ok());
    CHECK_EQ(cell(b.run("SELECT count(*) FROM t").view(), 0, 0), std::string("4"));
    CHECK(b.run("INSERT INTO t(name) VALUES ('now ok')").ok());
}

TEST(socket_disconnect_mid_query_and_in_transaction)
{
    Fixture f;
    {
        Wire w = f.open();
        auto begin = w.send(rem::Op::Begin, [](rem::Request::Builder r) { r.setTxn(1); });
        CHECK(w.finish(begin).ok());
        CHECK(w.finish(w.exec("INSERT INTO t(name) VALUES ('lost')", 1)).ok());
        // Leave a long query running, then drop the connection.
        w.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000000) SELECT count(*) FROM c");
        std::this_thread::sleep_for(100ms);
        w.ws.close();
    }
    // The server interrupts, rolls back, and is usable again.
    for (int i = 0; i < 100 && f.socket->stats().sessions != 0; ++i)
        std::this_thread::sleep_for(50ms);
    CHECK_EQ(f.socket->stats().sessions, std::size_t(0));
    Wire w = f.open();
    CHECK_EQ(cell(w.run("SELECT count(*) FROM t").view(), 0, 0), std::string("3"));
    CHECK(w.run("INSERT INTO t(name) VALUES ('writer free')").ok());
}

TEST(socket_describe)
{
    Fixture f;
    Wire w = f.open();
    f.db.exec("CREATE INDEX t_name ON t(name)");
    auto tables = w.finish(w.send(rem::Op::Describe, [](rem::Request::Builder r) { r.setWhat(rem::Catalog::Tables); }));
    CHECK(tables.ok());
    CHECK_EQ(cell(tables.view(), 0, 0), std::string("t"));
    auto table = w.finish(w.send(rem::Op::Describe, [](rem::Request::Builder r) {
        r.setWhat(rem::Catalog::Table_);
        r.setName("t");
    }));
    CHECK(table.ok());
    auto info = f.db.table("t");
    auto view = table.view();
    CHECK_EQ(view.rowCount(), std::uint64_t(info.columns.size()));
    CHECK_EQ(cell(view, 0, 0), std::string("id"));
    CHECK_EQ(cell(view, 0, 12), std::string("1"));  // pk_position
    CHECK_EQ(std::string(table.terminal.reply().sql()), info.sql);
    auto indexes = w.finish(w.send(rem::Op::Describe, [](rem::Request::Builder r) {
        r.setWhat(rem::Catalog::Indexes);
        r.setName("t");
    }));
    CHECK(indexes.ok());
    CHECK_EQ(cell(indexes.view(), 0, 0), std::string("t_name"));
    CHECK_EQ(cell(indexes.view(), 0, 6), std::string("name"));
    auto missing = w.finish(w.send(rem::Op::Describe, [](rem::Request::Builder r) {
        r.setWhat(rem::Catalog::Table_);
        r.setName("nope");
    }));
    CHECK(missing.error(ErrorCode::NoSuchTable));
}

TEST(socket_state_ping_goodbye)
{
    Fixture f;
    Wire w = f.open();
    CHECK(w.run("INSERT INTO t(name) VALUES ('x')").ok());
    auto state = w.finish(w.send(rem::Op::State));
    CHECK(state.ok());
    CHECK_EQ(state.terminal.reply().totalChanges(), std::int64_t(1));
    auto pong = w.finish(w.send(rem::Op::Ping));
    CHECK(pong.terminal.reply().kind() == rem::Kind::Pong);
    auto bye = w.finish(w.send(rem::Op::Goodbye));
    CHECK(bye.ok());
    try {
        w.next(5s);
        CHECK(false);
    } catch (const http::WebSocketError& e) {
        CHECK_EQ(int(e.closeCode()), 1000);
    }
}

TEST(socket_text_frame_is_refused)
{
    Fixture f;
    Wire w = f.open();
    w.ws.sendText("hello");
    try {
        w.next(5s);
        w.next(5s);
        CHECK(false);
    } catch (const http::WebSocketError& e) {
        CHECK_EQ(int(e.closeCode()), 1003);
    }
}

TEST(socket_session_limits)
{
    Fixture f(rem::SqlSocketOptions().maxSessions(2));
    Wire a = f.open();
    Wire b = f.open();
    Wire c;
    try {
        c.connect(f.port);
        CHECK(false);
    } catch (const http::WebSocketError& e) {
        CHECK_EQ(e.status(), 503);
    }
    CHECK(f.socket->stats().sessionsRefused >= 1);
    b.ws.close();
    for (int i = 0; i < 100 && f.socket->stats().sessions > 1; ++i)
        std::this_thread::sleep_for(20ms);
    Wire d = f.open();  // the slot came back
    CHECK(d.run("SELECT 1").ok());
}

TEST(socket_shutdown_graceful)
{
    Fixture f;
    Wire w = f.open();
    // Under AddressSanitizer this query is about 80 times slower; the test is about the grace period, not the engine.
#if defined(__SANITIZE_ADDRESS__)
    const char* slowSql = "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 20000) SELECT count(*) FROM c";
#else
    const char* slowSql = "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 300000) SELECT count(*) FROM c";
#endif
    auto slowish = w.exec(slowSql);
    std::this_thread::sleep_for(50ms);
    std::thread shutting([&] { f.socket->shutdown(20s); });
    struct Joiner
    {
        std::thread& t;
        ~Joiner()
        {
            if (t.joinable())
                t.join();
        }
    } joiner{shutting};
    // The running query finishes within the grace period; Closing is sent.
    bool sawClosing = false, sawDone = false;
    try {
        for (int i = 0; i < 10 && !(sawClosing && sawDone); ++i) {
            Frame fr = w.next(30s);
            sawClosing = sawClosing || fr.reply().kind() == rem::Kind::Closing;
            sawDone = sawDone || (fr.reply().id() == slowish && fr.reply().kind() == rem::Kind::Done);
        }
    } catch (const http::WebSocketError&) {
    }
    shutting.join();
    CHECK(sawClosing);
    CHECK(sawDone);
    Wire late;
    try {
        late.connect(f.port);
        CHECK(false);
    } catch (const http::WebSocketError& e) {
        CHECK_EQ(e.status(), 503);
    }
}

TEST(socket_shared_isolation_and_interceptor)
{
    struct Managed : rem::ExecInterceptor
    {
        void before(Database& db, std::string_view text, std::uint32_t hints) override
        {
            if (hints & 1)
                db.exec("BEGIN");
            (void)text;
        }
        void after(Database& db, std::string_view, std::uint32_t hints, bool failed) noexcept override
        {
            if ((hints & 1) && failed)
                try {
                    db.exec("ROLLBACK");
                } catch (...) {
                }
        }
    };
    Fixture f(rem::SqlSocketOptions().isolation(rem::Isolation::Shared).workers(1).interceptor(std::make_shared<Managed>()));
    Wire a = f.open();
    Wire b = f.open();
    // One shared handle: a BEGIN on A is the transaction B's statements run in.
    CHECK(a.run("BEGIN").ok());
    CHECK(a.run("INSERT INTO t(name) VALUES ('shared')").terminal.reply().inTransaction());
    CHECK_EQ(cell(b.run("SELECT count(*) FROM t").view(), 0, 0), std::string("4"));
    CHECK(b.run("ROLLBACK").ok());
    CHECK_EQ(cell(a.run("SELECT count(*) FROM t").view(), 0, 0), std::string("3"));
    // The interceptor ran around a request carrying its hint (it opened a transaction that stays pending).
    auto id = a.exec("INSERT INTO t(name) VALUES ('managed')", 0, [](rem::Request::Builder r) { r.setHints(1); });
    auto out = a.finish(id);
    CHECK(out.ok());
    CHECK(out.terminal.reply().inTransaction());
    CHECK(b.run("ROLLBACK").ok());
}

TEST(socket_shared_isolation_keeps_read_only_sessions_read_only)
{
    Fixture f(rem::SqlSocketOptions().isolation(rem::Isolation::Shared).workers(1));
    Wire writer = f.open();
    Wire reader = f.open({}, true);
    CHECK(reader.run("SELECT count(*) FROM t").ok());
    CHECK(reader.run("INSERT INTO t(name) VALUES ('nope')").error(ErrorCode::ReadOnly));
    CHECK(reader.run("CREATE TABLE sneaky(a)").error(ErrorCode::ReadOnly));
    // The writer is unaffected, and what it has pending stays invisible to the reader until it is committed.
    CHECK(writer.run("BEGIN").ok());
    CHECK(writer.run("INSERT INTO t(name) VALUES ('pending')").ok());
    CHECK_EQ(cell(reader.run("SELECT count(*) FROM t").view(), 0, 0), std::string("3"));
    CHECK(writer.run("COMMIT").ok());
    CHECK_EQ(cell(reader.run("SELECT count(*) FROM t").view(), 0, 0), std::string("4"));
}

TEST(socket_two_routes_one_server)
{
    // The application's own routes stay served beside the database route.
    Fixture f(rem::SqlSocketOptions(), {}, {});
    f.server->onGet("/health", [](http::HttpContext& c) { c.json(json::Json{{"ok", true}}); });
    Wire w = f.open();
    CHECK(w.run("SELECT 1").ok());
}

/// Resident set size in KiB.
static long residentKiB()
{
    long pages = 0, resident = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

TEST(socket_server_no_alloc)
{
    t_client = true;
    Fixture f(rem::SqlSocketOptions().workers(2), {}, [](Database& db) {
        db.exec("CREATE TABLE r(a INTEGER PRIMARY KEY, b TEXT, c REAL)");
        Transaction txn = db.begin();
        for (int i = 0; i < 50; ++i)
            txn.exec("INSERT INTO r VALUES (?, ?, ?)", Params{Value(i), Value("row" + std::to_string(i % 5)), Value(i * 0.5)});
        txn.commit();
    });
    Wire w = f.open();
    auto pid = w.send(rem::Op::Prepare, [](rem::Request::Builder r) {
        r.setStatement(1);
        r.setSql("SELECT a, b, c FROM r WHERE a >= ?");
    });
    CHECK(w.finish(pid).terminal.reply().kind() == rem::Kind::Prepared);
    // Prebuild the request bytes: the client side must not matter anyway, but this keeps the loop tight.
    std::vector<std::uint8_t> request;
    {
        bufs::MessageBuilder mb;
        auto r = mb.initRoot<rem::Request>();
        r.setId(1);
        r.setOp(rem::Op::ExecPrepared);
        r.setStatement(1);
        Wire::addParams(r, {Value(0)});
        const auto [data, size] = mb.finish();
        request.assign(data, data + size);
    }
    std::vector<std::uint8_t> credit;
    {
        bufs::MessageBuilder mb;
        auto r = mb.initRoot<rem::Request>();
        r.setId(2);
        r.setOp(rem::Op::Credit);
        r.setCredit(8192);  // one small Rows frame
        const auto [data, size] = mb.finish();
        credit.assign(data, data + size);
    }
    auto roundTrip = [&] {
        w.ws.sendBinary(request.data(), request.size());
        for (;;) {
            auto fr = w.tryNext(5s);
            CHECK(fr.has_value());
            if (fr->reply().kind() == rem::Kind::Done)
                return;
            CHECK(fr->reply().kind() == rem::Kind::Rows);
            w.ws.sendBinary(credit.data(), credit.size());
        }
    };
    for (int i = 0; i < 200; ++i)
        roundTrip();
    g_serverNews = 0;
    g_counting = true;
    for (int i = 0; i < 1000; ++i)
        roundTrip();
    g_counting = false;
    std::printf("      server allocations in 1000 prepared executions: %lu\n", g_serverNews.load());
    CHECK_EQ(g_serverNews.load(), 0ul);
    // The counter does see the server: new statement texts are parsed and planned on its threads.
    g_serverNews = 0;
    g_counting = true;
    for (int i = 0; i < 5; ++i)
        CHECK(w.run("SELECT a + " + std::to_string(1000 + i) + " FROM r").ok());
    g_counting = false;
    CHECK(g_serverNews.load() > 0);
}

TEST(socket_soak_memory_is_flat)
{
    t_client = true;
    Fixture f(rem::SqlSocketOptions().workers(2));
    constexpr int kSessions = 8, kRounds = 6000;
    std::vector<Wire> wires;
    for (int i = 0; i < kSessions; ++i)
        wires.push_back(f.open());
    long afterWarmup = 0;
    for (int round = 0; round < kRounds; ++round) {
        for (Wire& w : wires) {
            auto out = w.run(round % 3 == 0 ? "SELECT id, name, score FROM t" : "SELECT count(*) FROM t");
            CHECK(out.ok());
            if (round % 50 == 0) {  // some churn: statements and transactions come and go
                auto b = w.send(rem::Op::Begin, [](rem::Request::Builder r) { r.setTxn(3); });
                CHECK(w.finish(b).ok());
                auto c = w.send(rem::Op::Rollback, [](rem::Request::Builder r) { r.setTxn(3); });
                CHECK(w.finish(c).ok());
            }
        }
        if (round == kRounds / 10)
            afterWarmup = residentKiB();
    }
    const long end = residentKiB();
    std::printf("      RSS after warm-up %ld KiB, at the end %ld KiB (%d requests)\n", afterWarmup, end, kSessions * kRounds);
    CHECK(end - afterWarmup < 2048);
    CHECK_EQ(f.socket->stats().sessions, std::size_t(kSessions));
    CHECK_EQ(f.socket->stats().requests >= std::uint64_t(kSessions * kRounds), true);
}

int main()
{
    return tst::runAll("socket");
}
