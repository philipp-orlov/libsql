// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What is specific to a remote database: connecting and failing to, credentials, a connection that goes away,
// transparent reconnects, interrupts from another thread, timeouts, local-only members, and the allocation
// behaviour of a warm client.

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

#include "remote_fixture.hpp"

using namespace sql;
using namespace std::chrono_literals;

namespace {
thread_local bool t_client = false;
std::atomic<unsigned long> g_clientNews{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed) && t_client)
        g_clientNews.fetch_add(1, std::memory_order_relaxed);
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

void seed(Database& db)
{
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, n INTEGER)");
    db.exec("INSERT INTO t VALUES (1,'a',10),(2,'b',20),(3,'c',30)");
}

const char* kSlow = "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000000) SELECT count(*) FROM c";

}  // namespace

TEST(remote_connection_refused)
{
    // Nothing listens on this port.
    bool refused = false;
    try {
        Database::connect("ws://127.0.0.1:1/api/v1/ws/sql", RemoteOptions().connectTimeout(2000ms));
    } catch (const Error& e) {
        refused = e.code() == ErrorCode::Connection;
    }
    CHECK(refused);
    CHECK_THROWS(Database::connect("http://nope/"), ErrorCode::InvalidArgument);
    CHECK_THROWS(Database::connect("not a url"), ErrorCode::InvalidArgument);
    remote_test::Served served;
    // A route that is not the database's.
    bool wrongPath = false;
    try {
        Database::connect(served.url("/nothing"));
    } catch (const Error& e) {
        wrongPath = e.code() == ErrorCode::Connection;
    }
    CHECK(wrongPath);
}

TEST(remote_credentials)
{
    sql::remote::SqlSocketOptions options;
    options.authorize([](const sql::remote::Handshake& h) -> sql::remote::SqlSocketOptions::AuthorizeResult {
        if (h.header("Authorization") == "Bearer good")
            return sql::remote::Principal{"user", false, nullptr};
        if (h.header("Authorization") == "Bearer reader")
            return sql::remote::Principal{"reader", true, nullptr};
        if (h.header("Authorization").empty())
            return 0;
        return 403;
    });
    options.authenticate([](std::string_view token) -> std::optional<sql::remote::Principal> {
        if (token == "secret")
            return sql::remote::Principal{"by-token", false, nullptr};
        return std::nullopt;
    });
    options.requireAuth();
    remote_test::Served served(options);
    seed(served.db);
    CHECK_THROWS(served.connect(), ErrorCode::PermissionDenied);
    CHECK_THROWS(served.connect(RemoteOptions().authorization("Bearer wrong")), ErrorCode::PermissionDenied);
    CHECK_THROWS(served.connect(RemoteOptions().token("wrong")), ErrorCode::PermissionDenied);
    Database good = served.connect(RemoteOptions().authorization("Bearer good"));
    CHECK_EQ(tst::only(good.exec("SELECT count(*) FROM t")), std::string("3"));
    Database viaToken = served.connect(RemoteOptions().token("secret"));
    CHECK_EQ(tst::only(viaToken.exec("SELECT 1")), std::string("1"));
    Database reader = served.connect(RemoteOptions().authorization("Bearer reader"));
    CHECK_THROWS(reader.exec("DELETE FROM t"), ErrorCode::ReadOnly);
    // Asking for a read-only session.
    Database asked = served.connect(RemoteOptions().authorization("Bearer good").readOnly());
    CHECK_THROWS(asked.exec("DELETE FROM t"), ErrorCode::ReadOnly);
    CHECK_EQ(tst::only(asked.exec("SELECT count(*) FROM t")), std::string("3"));
}

TEST(remote_local_only_members_throw)
{
    remote_test::Served served;
    Database db = served.connect();
    CHECK_THROWS(db.env(), ErrorCode::Unsupported);
    Transaction t = db.begin();
    CHECK_THROWS(t.txn(), ErrorCode::Unsupported);
    CHECK(db.path().string().find("ws://127.0.0.1") == 0);
}

TEST(remote_interrupt_from_another_thread)
{
    remote_test::Served served;
    Database db = served.connect();
    std::atomic<int> code{-1};
    std::thread runner([&] {
        try {
            db.exec(kSlow);
            code = 0;
        } catch (const Error& e) {
            code = int(e.code());
        }
    });
    std::this_thread::sleep_for(300ms);
    const auto asked = std::chrono::steady_clock::now();
    db.interrupt();
    runner.join();
    CHECK_EQ(code.load(), int(ErrorCode::Interrupted));
    CHECK(std::chrono::steady_clock::now() - asked < 3s);
    CHECK_EQ(tst::only(db.exec("SELECT 2 + 2")), std::string("4"));
    db.interrupt();  // nothing is running: no effect on the next statement
    CHECK_EQ(tst::only(db.exec("SELECT 3 + 3")), std::string("6"));
}

TEST(remote_request_timeout)
{
    remote_test::Served served;
    Database db = served.connect(RemoteOptions().requestTimeout(400ms));
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS(db.exec(kSlow), ErrorCode::Interrupted);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took >= 350ms);
    CHECK(took < 5s);
    CHECK_EQ(tst::only(db.exec("SELECT 1")), std::string("1"));
}

TEST(remote_server_statement_timeout)
{
    sql::remote::SqlSocketOptions options;
    options.statementTimeout(300ms);
    remote_test::Served served(options);
    Database db = served.connect();
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS(db.exec(kSlow), ErrorCode::Interrupted);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK_EQ(tst::only(db.exec("SELECT 1")), std::string("1"));
}

TEST(remote_reconnects_after_the_server_closed_an_idle_connection)
{
    sql::remote::SqlSocketOptions options;
    options.idleTimeout(1s);
    remote_test::Served served(options);
    seed(served.db);
    Database db = served.connect();
    Statement st = db.prepare("SELECT name FROM t WHERE id = ?");
    CHECK_EQ(tst::only(st.exec({Value(2)})), std::string("b"));
    std::this_thread::sleep_for(2500ms);  // the server's idle timeout closes the session
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("3"));  // a new connection, by itself
    CHECK_EQ(tst::only(st.exec({Value(3)})), std::string("c"));                // re-prepared on its next use
    Statement fresh = db.prepare("SELECT 1");
    CHECK_EQ(tst::only(fresh.exec()), std::string("1"));
    // With reconnect(false) the same situation is an error.
    Database strict = served.connect(RemoteOptions().reconnect(false));
    std::this_thread::sleep_for(2500ms);
    CHECK_THROWS(strict.exec("SELECT 1"), ErrorCode::Connection);
}

TEST(remote_no_reconnect_inside_a_transaction)
{
    sql::remote::SqlSocketOptions options;
    options.idleTimeout(1s);
    remote_test::Served served(options);
    seed(served.db);
    Database db = served.connect();
    Transaction t = db.begin();
    t.exec("INSERT INTO t VALUES (9, 'z', 90)");
    std::this_thread::sleep_for(2500ms);
    bool lost = false;
    try {
        t.exec("SELECT 1");
    } catch (const Error& e) {
        lost = e.code() == ErrorCode::Connection && std::string(e.what()).find("rolled back") != std::string::npos;
    }
    CHECK(lost);
    // The insert never happened, and the database is usable again on a new connection.
    CHECK_EQ(tst::only(served.db.exec("SELECT count(*) FROM t")), std::string("3"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("3"));
}

TEST(remote_server_goes_away_mid_result)
{
    remote_test::Served served;
    served.db.exec("CREATE TABLE big(n INTEGER PRIMARY KEY, s TEXT)");
    {
        Transaction txn = served.db.begin();
        for (int i = 0; i < 40000; ++i)
            txn.exec("INSERT INTO big VALUES (?, ?)", Params{Value(i), Value(std::string(50, 'x'))});
        txn.commit();
    }
    Database db = served.connect(RemoteOptions().window(64 * 1024));
    struct Slow final : ResultConsumer
    {
        std::function<void()> onFirst;
        std::size_t rows = 0;
        void begin(const ResultShape&) override {}
        bool row(const RowView&) override
        {
            if (++rows == 100 && onFirst)
                onFirst();
            return true;
        }
        void end(std::uint64_t, std::int64_t) override {}
    } consumer;
    consumer.onFirst = [&] { std::thread([&] { served.socket->shutdown(0ms); }).detach(); };
    bool failed = false;
    try {
        db.exec("SELECT * FROM big", Params{}, consumer);
    } catch (const Error& e) {
        failed = e.code() == ErrorCode::Connection;
    }
    CHECK(failed);
    std::this_thread::sleep_for(300ms);
    CHECK_THROWS(db.exec("SELECT 1"), ErrorCode::Connection);  // the server no longer accepts
}

TEST(remote_close)
{
    remote_test::Served served;
    seed(served.db);
    Database db = served.connect();
    Statement st = db.prepare("SELECT 1");
    CHECK_EQ(tst::only(st.exec()), std::string("1"));
    CHECK_EQ(served.socket->stats().sessions, std::size_t(1));
    db.close();
    CHECK(!db.valid());
    CHECK_THROWS(st.exec(), ErrorCode::InvalidArgument);
    for (int i = 0; i < 100 && served.socket->stats().sessions != 0; ++i)
        std::this_thread::sleep_for(20ms);
    CHECK_EQ(served.socket->stats().sessions, std::size_t(0));
}

TEST(remote_a_row_callback_cannot_reenter)
{
    remote_test::Served served;
    seed(served.db);
    Database db = served.connect();
    bool refused = false;
    Statement st = db.prepare("SELECT id FROM t");
    st.each([&](const RowView&) {
        try {
            db.exec("SELECT 1");
        } catch (const Error& e) {
            refused = e.code() == ErrorCode::InvalidArgument;
        }
        return false;
    });
    CHECK(refused);
    CHECK_EQ(tst::only(db.exec("SELECT 5")), std::string("5"));
}

TEST(remote_many_sessions_at_once)
{
    remote_test::Served served(sql::remote::SqlSocketOptions().workers(4));
    seed(served.db);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            try {
                Database db = served.connect();
                Statement st = db.prepare("SELECT n FROM t WHERE id = ?");
                for (int round = 0; round < 200; ++round) {
                    if (tst::only(st.exec({Value(1 + round % 3)})) != std::to_string(10 * (1 + round % 3)))
                        ++failures;
                    if (round % 50 == 0) {
                        Transaction t = db.beginRead();
                        t.exec("SELECT count(*) FROM t");
                    }
                }
            } catch (...) {
                ++failures;
            }
            (void)i;
        });
    }
    for (auto& t : threads)
        t.join();
    CHECK_EQ(failures.load(), 0);
}

TEST(remote_streams_a_large_result_through_a_small_window)
{
    remote_test::Served served;
    served.db.exec("CREATE TABLE big(n INTEGER PRIMARY KEY, s TEXT)");
    {
        Transaction txn = served.db.begin();
        for (int i = 0; i < 60000; ++i)
            txn.exec("INSERT INTO big VALUES (?, ?)", Params{Value(i), Value("row " + std::to_string(i))});
        txn.commit();
    }
    Database db = served.connect(RemoteOptions().window(32 * 1024).batchBytes(8 * 1024));
    struct Counter final : ResultConsumer
    {
        std::size_t rows = 0;
        std::int64_t sum = 0;
        void begin(const ResultShape&) override {}
        bool row(const RowView& r) override
        {
            ++rows;
            sum += r[0].integer();
            return true;
        }
        void end(std::uint64_t, std::int64_t) override {}
    } counter;
    db.exec("SELECT n, s FROM big ORDER BY n", Params{}, counter);
    CHECK_EQ(counter.rows, std::size_t(60000));
    CHECK_EQ(counter.sum, std::int64_t(59999) * 60000 / 2);
}

TEST(remote_client_no_alloc)
{
    t_client = true;
    remote_test::Served served;
    served.db.exec("CREATE TABLE r(a INTEGER PRIMARY KEY, b TEXT, c REAL)");
    {
        Transaction txn = served.db.begin();
        for (int i = 0; i < 50; ++i)
            txn.exec("INSERT INTO r VALUES (?, ?, ?)", Params{Value(i), Value("row" + std::to_string(i % 5)), Value(i * 0.5)});
        txn.commit();
    }
    Database db = served.connect();
    Statement st = db.prepare("SELECT a, b, c FROM r WHERE a >= ?");
    struct Sum final : ResultConsumer
    {
        std::int64_t total = 0;
        std::size_t rows = 0;
        void begin(const ResultShape&) override {}
        bool row(const RowView& r) override
        {
            total += r[0].integer();
            ++rows;
            return true;
        }
        void end(std::uint64_t, std::int64_t) override {}
    } sum;
    st.bind(1, Value(0));
    for (int i = 0; i < 300; ++i)
        st.exec(sum);
    g_clientNews = 0;
    g_counting = true;
    for (int i = 0; i < 1000; ++i)
        st.exec(sum);
    g_counting = false;
    std::printf("      client allocations in 1000 prepared executions: %lu\n", g_clientNews.load());
    CHECK_EQ(sum.rows, std::size_t(1300 * 50));
    CHECK_EQ(g_clientNews.load(), 0ul);
}

int main()
{
    return tst::runAll("remote_client");
}
