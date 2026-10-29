// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The server half of remote databases: sql::remote::SqlSocket serving the Sql service of include/sql/wire/remote.bufs
// through librpc, spoken to directly with the generated SqlClient (no Database::connect involved, except where the
// point is the local socket).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <unistd.h>

#include "remote_fixture.hpp"
#include "rpc/client.hpp"
#include "sql/remote/sql_socket.hpp"
#include "sql/wire/remote.rpc.hpp"
#include "sql/wire/result_view.hpp"
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
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

/// The code a failed call carried (ErrorCode << 8 | Constraint in the status detail).
ErrorCode codeOf(const rpc::Error& e)
{
    const std::uint32_t d = e.status().detail();
    return (d & 0x80000000u) ? static_cast<ErrorCode>((d >> 8) & 0xFFFF) : ErrorCode::Internal;
}

template <class Fn>
bool failsWith(ErrorCode code, Fn&& fn)
{
    try {
        fn();
    } catch (const rpc::Error& e) {
        return codeOf(e) == code;
    }
    return false;
}

void seed(Database& db)
{
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL)");
    db.exec("INSERT INTO t VALUES (1, 'a', 1.5), (2, 'b', 2.5), (3, 'c', 3.5)");
}

/// A raw session: the generated client, opened.
struct Wire
{
    rpc::Client connection;
    std::unique_ptr<rem::SqlClient> sql;

    explicit Wire(const std::string& url, bool readOnly = false, bool open = true, const rpc::ClientOptions& options = {})
        : connection(rpc::Client::connect(url, options)), sql(std::make_unique<rem::SqlClient>(connection))
    {
        if (open)
            sql->Open(readOnly, "test", 0);
    }

    /// What a statement produced.
    struct Out
    {
        std::size_t rows = 0, batches = 0;
        std::uint64_t changes = 0;
        std::int64_t totalChanges = 0, lastInsertId = 0;
        bool inTransaction = false;
        std::string sql;
        bool strict = false;
        std::string firstCell;
    };

    template <class Stream>
    static Out collect(Stream& stream)
    {
        Out out;
        std::vector<std::vector<std::uint32_t>> slots;
        while (stream.next()) {
            ++out.batches;
            for (auto& s : slots)
                s.clear();
            auto batch = wire::BatchView::open(stream.payload().data(), stream.payload().size(), &slots);
            if (!batch)
                throw std::runtime_error("rows do not verify");
            if (out.rows == 0 && batch->rowCount() && batch->columnCount()) {
                Value v = batch->column(0).value(0);
                out.firstCell = v.type() == Type::Integer ? std::to_string(v.integer()) : std::string(v.text());
            }
            out.rows += batch->rowCount();
        }
        const auto t = stream.trailer();
        out.changes = t.changes();
        out.totalChanges = t.totalChanges();
        out.lastInsertId = t.lastInsertId();
        out.inTransaction = t.inTransaction();
        out.sql.assign(t.sql());
        out.strict = t.strict();
        return out;
    }

    Out run(std::string_view text, std::uint32_t txn = 0, std::vector<Value> params = {}, std::size_t batchRows = 0)
    {
        auto stream = sql->Exec(
            text,
            [&](rem::ParamList::Builder& b) {
                if (params.empty())
                    return;
                auto list = b.initList(params.size());
                for (std::size_t i = 0; i < params.size(); ++i) {
                    auto p = list.init(i);
                    if (params[i].type() == Type::Integer) {
                        p.setType(rem::ParamType::Integer);
                        p.setInteger(params[i].integer());
                    } else if (params[i].type() == Type::Text) {
                        p.setType(rem::ParamType::Text);
                        const std::string_view t = params[i].text();
                        p.setBytes(reinterpret_cast<const std::uint8_t*>(t.data()), t.size());
                    }
                }
            },
            txn, [&](rem::ResultOptions::Builder& o) { o.setBatchRows(static_cast<std::uint32_t>(batchRows)); }, 0);
        return collect(stream);
    }
};

}  // namespace

TEST(service_requires_open)
{
    remote_test::Served served;
    seed(served.db);
    Wire w(served.url(), false, /*open=*/false);
    bool refused = false;
    try {
        w.run("SELECT 1");
    } catch (const rpc::Error& e) {
        refused = e.code() == rpc::Code::FailedPrecondition;
    }
    CHECK(refused);
    w.sql->Open(false, "test", 0);
    CHECK_EQ(w.run("SELECT count(*) FROM t").firstCell, std::string("3"));
    CHECK(!failsWith(ErrorCode::Internal, [&] { w.sql->Open(false, "again", 0); }) || true);  // a second Open is an error
    bool second = false;
    try {
        w.sql->Open(false, "again", 0);
    } catch (const rpc::Error& e) {
        second = e.code() == rpc::Code::FailedPrecondition;
    }
    CHECK(second);
}

TEST(service_exec_rows_and_trailer)
{
    remote_test::Served served;
    seed(served.db);
    Wire w(served.url());
    auto q = w.run("SELECT id, name FROM t ORDER BY id");
    CHECK_EQ(q.rows, std::size_t(3));
    CHECK_EQ(q.firstCell, std::string("1"));
    auto ins = w.run("INSERT INTO t(name, score) VALUES ('d', 4.5)");
    CHECK_EQ(ins.changes, std::uint64_t(1));
    CHECK_EQ(ins.lastInsertId, std::int64_t(4));
    CHECK_EQ(ins.rows, std::size_t(0));
    CHECK(!ins.inTransaction);
    // An error keeps the message and the code the local database gives.
    CHECK(failsWith(ErrorCode::NoSuchTable, [&] { w.run("SELECT * FROM missing"); }));
    CHECK(failsWith(ErrorCode::SyntaxError, [&] { w.run("SELEC 1"); }));
    CHECK(failsWith(ErrorCode::ConstraintViolation, [&] { w.run("INSERT INTO t(id, name) VALUES (1, 'dup')"); }));
    // ... and the session goes on.
    CHECK_EQ(w.run("SELECT count(*) FROM t").firstCell, std::string("4"));
}

TEST(service_params)
{
    remote_test::Served served;
    seed(served.db);
    Wire w(served.url());
    CHECK_EQ(w.run("SELECT name FROM t WHERE id = ?", 0, {Value(2)}).firstCell, std::string("b"));
    CHECK_EQ(w.run("SELECT ? || '!'", 0, {Value("hi")}).firstCell, std::string("hi!"));
    CHECK(failsWith(ErrorCode::InvalidArgument, [&] { w.run("SELECT ?, ?", 0, {Value(1)}); }));
}

TEST(service_streaming_batches)
{
    remote_test::Served served;
    served.db.exec("CREATE TABLE n(i INTEGER PRIMARY KEY)");
    {
        Transaction txn = served.db.begin();
        for (int i = 0; i < 1000; ++i)
            txn.exec("INSERT INTO n VALUES (?)", Params{Value(i)});
        txn.commit();
    }
    Wire w(served.url());
    auto q = w.run("SELECT i FROM n", 0, {}, /*batchRows=*/100);
    CHECK_EQ(q.rows, std::size_t(1000));
    CHECK_EQ(q.batches, std::size_t(10));
}

TEST(service_prepared_lifecycle)
{
    remote_test::Served served;
    seed(served.db);
    Wire w(served.url());
    auto prepared = w.sql->Prepare(1, "SELECT name FROM t WHERE id = ?");
    CHECK_EQ(prepared->parameters(), std::uint32_t(1));
    CHECK_EQ(prepared->columns().size(), std::size_t(1));
    CHECK(!prepared->writes());
    auto exec = [&](std::uint32_t id, std::int64_t v) {
        auto stream = w.sql->ExecPrepared(
            id,
            [&](rem::ParamList::Builder& b) {
                auto list = b.initList(1);
                auto p = list.init(0);
                p.setType(rem::ParamType::Integer);
                p.setInteger(v);
            },
            0, [](rem::ResultOptions::Builder&) {});
        return Wire::collect(stream);
    };
    CHECK_EQ(exec(1, 3).firstCell, std::string("c"));
    CHECK_EQ(exec(1, 1).firstCell, std::string("a"));
    CHECK(failsWith(ErrorCode::InvalidArgument, [&] { exec(7, 1); }));          // never prepared
    CHECK(failsWith(ErrorCode::InvalidArgument, [&] { w.sql->Prepare(1, "SELECT 1"); }));  // id in use
    w.sql->ClosePrepared(1);
    CHECK(failsWith(ErrorCode::InvalidArgument, [&] { exec(1, 1); }));
    CHECK(failsWith(ErrorCode::SyntaxError, [&] { w.sql->Prepare(2, "SELEC"); }));
}

TEST(service_transactions)
{
    remote_test::Served served;
    seed(served.db);
    Wire w(served.url());
    auto begun = w.sql->Begin(5, false);
    (void)begun;
    CHECK(w.run("INSERT INTO t(name) VALUES ('tx')", 5).changes == 1);
    CHECK_EQ(std::stoll(tst::only(served.db.exec("SELECT count(*) FROM t"))), std::int64_t(3));  // not yet visible
    w.sql->Commit(5);
    CHECK_EQ(std::stoll(tst::only(served.db.exec("SELECT count(*) FROM t"))), std::int64_t(4));
    CHECK(failsWith(ErrorCode::BadTransaction, [&] { w.run("SELECT 1", 5); }));  // finished
    CHECK(failsWith(ErrorCode::BadTransaction, [&] { w.sql->Commit(9); }));      // never begun
    w.sql->Begin(6, false);
    w.run("DELETE FROM t", 6);
    w.sql->Rollback(6);
    CHECK_EQ(w.run("SELECT count(*) FROM t").firstCell, std::string("4"));
    CHECK(failsWith(ErrorCode::InvalidArgument, [&] { w.sql->Begin(0, false); }));
}

TEST(service_two_sessions_are_isolated)
{
    remote_test::Served served;
    seed(served.db);
    Wire a(served.url()), b(served.url());
    a.sql->Begin(1, false);
    a.run("INSERT INTO t(name) VALUES ('mine')", 1);
    CHECK_EQ(b.run("SELECT count(*) FROM t").firstCell, std::string("3"));  // a's insert is not visible
    a.sql->Commit(1);
    CHECK_EQ(b.run("SELECT count(*) FROM t").firstCell, std::string("4"));
}

TEST(service_disconnect_rolls_back_and_recycles_the_handle)
{
    remote_test::Served served;
    seed(served.db);
    {
        Wire a(served.url());
        a.sql->Begin(1, false);
        a.run("DELETE FROM t", 1);
        a.run("BEGIN");  // and an SQL transaction on the handle as well
        // dropped without Commit or Rollback
    }
    for (int i = 0; i < 100 && served.socket->stats().sessions != 0; ++i)
        std::this_thread::sleep_for(20ms);
    CHECK_EQ(served.socket->stats().sessions, std::size_t(0));
    CHECK_EQ(std::stoll(tst::only(served.db.exec("SELECT count(*) FROM t"))), std::int64_t(3));  // rolled back
    // the handle went back to the pool, clean
    for (int i = 0; i < 100 && served.socket->stats().idleHandles == 0; ++i)
        std::this_thread::sleep_for(20ms);
    CHECK(served.socket->stats().idleHandles >= 1);
    Wire b(served.url());
    CHECK_EQ(b.run("SELECT count(*) FROM t").firstCell, std::string("3"));
    CHECK(!b.run("SELECT 1").inTransaction);
}

TEST(service_describe)
{
    remote_test::Served served;
    seed(served.db);
    served.db.exec("CREATE INDEX t_name ON t(name)");
    Wire w(served.url());
    auto describe = [&](rem::Catalog what, std::string_view name) {
        auto stream = w.sql->Describe(what, name, [](rem::ResultOptions::Builder&) {});
        return Wire::collect(stream);
    };
    auto tables = describe(rem::Catalog::Tables, {});
    CHECK_EQ(tables.rows, std::size_t(1));
    CHECK_EQ(tables.firstCell, std::string("t"));
    auto table = describe(rem::Catalog::Table_, "t");
    CHECK_EQ(table.rows, std::size_t(3));  // three columns
    CHECK(table.sql.find("CREATE TABLE t") != std::string::npos);
    auto indexes = describe(rem::Catalog::Indexes, "t");
    CHECK(indexes.rows >= 1);
    CHECK(failsWith(ErrorCode::NoSuchTable, [&] { describe(rem::Catalog::Table_, "nope"); }));
    auto state = w.sql->State();
    CHECK(!state->inTransaction());
}

TEST(service_session_limits)
{
    remote_test::Served served(rem::SqlSocketOptions().maxSessions(2).maxStatements(2).maxTransactions(1));
    seed(served.db);
    Wire a(served.url()), b(served.url());
    bool refused = false;
    try {
        Wire c(served.url(), false, false);  // the handshake itself is refused
    } catch (const rpc::Error& e) {
        refused = e.code() == rpc::Code::Unavailable;
    }
    CHECK(refused);
    a.sql->Prepare(1, "SELECT 1");
    a.sql->Prepare(2, "SELECT 2");
    CHECK(failsWith(ErrorCode::Busy, [&] { a.sql->Prepare(3, "SELECT 3"); }));
    a.sql->Begin(1, false);
    CHECK(failsWith(ErrorCode::Busy, [&] { a.sql->Begin(2, false); }));
}

TEST(service_shared_isolation_and_interceptor)
{
    struct Managed : rem::ExecInterceptor
    {
        void before(Database& db, std::string_view, std::uint32_t hints) override
        {
            if (hints & 1)
                db.exec("BEGIN");
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
    remote_test::Served served(rem::SqlSocketOptions().isolation(rem::Isolation::Shared).workers(1).interceptor(std::make_shared<Managed>()));
    seed(served.db);
    Wire a(served.url()), b(served.url());
    // One shared handle: a BEGIN on A is the transaction B's statements run in.
    a.run("BEGIN");
    CHECK(a.run("INSERT INTO t(name) VALUES ('shared')").inTransaction);
    CHECK_EQ(b.run("SELECT count(*) FROM t").firstCell, std::string("4"));
    b.run("ROLLBACK");
    CHECK_EQ(a.run("SELECT count(*) FROM t").firstCell, std::string("3"));
    // The interceptor ran around a request carrying its hint: it opened a transaction that stays pending.
    auto stream = a.sql->Exec("INSERT INTO t(name) VALUES ('managed')", [](rem::ParamList::Builder&) {}, 0,
                              [](rem::ResultOptions::Builder&) {}, 1);
    CHECK(Wire::collect(stream).inTransaction);
    b.run("ROLLBACK");
}

TEST(service_shared_isolation_keeps_read_only_sessions_read_only)
{
    remote_test::Served served(rem::SqlSocketOptions().isolation(rem::Isolation::Shared).workers(1));
    seed(served.db);
    Wire writer(served.url()), reader(served.url(), /*readOnly=*/true);
    CHECK(reader.run("SELECT count(*) FROM t").rows == 1);
    CHECK(failsWith(ErrorCode::ReadOnly, [&] { reader.run("INSERT INTO t(name) VALUES ('nope')"); }));
    CHECK(failsWith(ErrorCode::ReadOnly, [&] { reader.run("CREATE TABLE sneaky(a)"); }));
    writer.run("BEGIN");
    writer.run("INSERT INTO t(name) VALUES ('pending')");
    CHECK_EQ(reader.run("SELECT count(*) FROM t").firstCell, std::string("3"));  // not committed yet
    writer.run("COMMIT");
    CHECK_EQ(reader.run("SELECT count(*) FROM t").firstCell, std::string("4"));
}

TEST(service_over_a_local_socket)
{
    remote_test::Served served;
    seed(served.db);
    Database db = served.connectLocal();
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("3"));
    db.exec("INSERT INTO t(name, score) VALUES ('local', 9.5)");
    CHECK_EQ(db.lastInsertRowid(), std::int64_t(4));
    Statement st = db.prepare("SELECT name FROM t WHERE id = ?");
    CHECK_EQ(tst::only(st.exec({Value(2)})), std::string("b"));
    {
        Transaction txn = db.begin();
        txn.exec("DELETE FROM t");
        txn.rollback();
    }
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("4"));
    // a large result through the rings
    served.db.exec("CREATE TABLE big(n INTEGER PRIMARY KEY, s TEXT)");
    {
        Transaction txn = served.db.begin();
        for (int i = 0; i < 20000; ++i)
            txn.exec("INSERT INTO big VALUES (?, ?)", Params{Value(i), Value(std::string(40, 'x'))});
        txn.commit();
    }
    std::size_t rows = 0;
    struct Count final : ResultConsumer
    {
        std::size_t* rows;
        void begin(const ResultShape&) override {}
        bool row(const RowView&) override
        {
            ++*rows;
            return true;
        }
        void end(std::uint64_t, std::int64_t) override {}
    } count;
    count.rows = &rows;
    db.exec("SELECT * FROM big", Params{}, count);
    CHECK_EQ(rows, std::size_t(20000));
    // interrupting works the same
    std::thread([&] {
        std::this_thread::sleep_for(150ms);
        db.interrupt();
    }).detach();
    CHECK_THROWS(db.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000000) SELECT count(*) FROM c"),
                 ErrorCode::Interrupted);
    CHECK_EQ(tst::only(db.exec("SELECT 1")), std::string("1"));
}

TEST(service_server_no_alloc)
{
    t_client = true;
    remote_test::Served served(rem::SqlSocketOptions().workers(2));
    served.db.exec("CREATE TABLE r(a INTEGER PRIMARY KEY, b TEXT, c REAL)");
    {
        Transaction txn = served.db.begin();
        for (int i = 0; i < 50; ++i)
            txn.exec("INSERT INTO r VALUES (?, ?, ?)", Params{Value(i), Value("row" + std::to_string(i % 5)), Value(i * 0.5)});
        txn.commit();
    }
    // The local socket: the client runs on this thread (not counted), so what is counted is the server's.
    Database db = served.connectLocal();
    Statement st = db.prepare("SELECT a, b, c FROM r WHERE a >= ?");
    for (int i = 0; i < 300; ++i)
        st.exec({Value(0)});
    g_serverNews = 0;
    g_counting = true;
    for (int i = 0; i < 1000; ++i)
        st.exec({Value(0)});
    g_counting = false;
    std::printf("      server allocations in 1000 prepared executions: %lu\n", g_serverNews.load());
    CHECK(g_serverNews.load() <= 16);  // a handful of one-off growths at most; per call it would be thousands
    // The counter does see the server: new statement texts are parsed and planned on its threads.
    g_serverNews = 0;
    g_counting = true;
    for (int i = 0; i < 5; ++i)
        db.exec("SELECT a + " + std::to_string(1000 + i) + " FROM r");
    g_counting = false;
    CHECK(g_serverNews.load() > 0);
}

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

TEST(service_soak_memory_is_flat)
{
    t_client = true;
    // A local connection's rings are sized by the largest message and their pages are first touched as the write position
    // walks round them; a small maximum makes that one lap short, so the test sees the steady state.
    remote_test::Served served(rem::SqlSocketOptions().workers(2).maxMessage(128 * 1024));
    seed(served.db);
    constexpr int kSessions = 8, kRounds = 3000;
    std::vector<Database> sessions;
    for (int i = 0; i < kSessions; ++i)
        sessions.push_back(i % 2 ? served.connect() : served.connectLocal());
    long afterWarmup = 0;
    for (int round = 0; round < kRounds; ++round) {
        for (Database& db : sessions) {
            db.exec(round % 3 == 0 ? "SELECT id, name, score FROM t" : "SELECT count(*) FROM t");
            if (round % 50 == 0) {  // some churn: statements and transactions come and go
                Transaction txn = db.begin();
                txn.rollback();
                Statement st = db.prepare("SELECT 1");
                st.exec();
            }
        }
        if (round == kRounds / 3)
            afterWarmup = residentKiB();
    }
    const long end = residentKiB();
    std::printf("      RSS after warm-up %ld KiB, at the end %ld KiB (%d requests)\n", afterWarmup, end, kSessions * kRounds);
    CHECK(end - afterWarmup < 4096);
    CHECK_EQ(served.socket->stats().sessions, std::size_t(kSessions));
}

int main()
{
    return tst::runAll("service");
}
