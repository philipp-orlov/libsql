// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Database::openHandle(): more writable handles on one open store (what a
// server gives each remote session), and Database::interrupt() on them.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "sql_test.hpp"

using namespace sql;
using namespace std::chrono_literals;

TEST(handles_write_in_turn)
{
    tst::Scratch s("handles");
    Database db = s.open();
    Database other = db.openHandle();
    db.exec("CREATE TABLE t(a INTEGER)");
    other.exec("INSERT INTO t VALUES (1)");
    db.exec("INSERT INTO t VALUES (2)");
    CHECK_EQ(tst::only(other.exec("SELECT sum(a) FROM t")), std::string("3"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("2"));
    CHECK(!other.inTransaction());
    // Counters are per handle.
    CHECK_EQ(other.changes(), std::int64_t(1));
    CHECK_EQ(other.lastInsertRowid(), std::int64_t(1));
    CHECK_EQ(db.lastInsertRowid(), std::int64_t(2));
}

TEST(handles_second_writer_waits_then_busy)
{
    tst::Scratch s("handles");
    Database db = Database::configure().busyTimeout(300ms).open(s.file());
    db.exec("CREATE TABLE t(a INTEGER)");
    Database other = db.openHandle();
    Transaction first = db.begin();
    first.exec("INSERT INTO t VALUES (1)");
    const auto started = std::chrono::steady_clock::now();
    CHECK_THROWS(other.begin(), ErrorCode::Busy);
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK(waited >= 250ms);
    CHECK(waited < 5s);
    // A reader is not blocked, and sees the state before the open writer.
    CHECK_EQ(tst::only(other.openReader().exec("SELECT count(*) FROM t")), std::string("0"));
    first.commit();
    Transaction second = other.begin();
    second.exec("INSERT INTO t VALUES (2)");
    second.commit();
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("2"));
}

TEST(handles_second_writer_gets_the_slot_when_it_frees)
{
    tst::Scratch s("handles");
    Database db = Database::configure().busyTimeout(5s).open(s.file());
    db.exec("CREATE TABLE t(a INTEGER)");
    Database other = db.openHandle();
    Transaction first = db.begin();
    std::atomic<bool> got{false};
    std::thread waiter([&] {
        Transaction t = other.begin();  // waits for first
        t.exec("INSERT INTO t VALUES (7)");
        t.commit();
        got = true;
    });
    std::this_thread::sleep_for(150ms);
    CHECK(!got.load());
    first.commit();
    waiter.join();
    CHECK(got.load());
    CHECK_EQ(tst::only(db.exec("SELECT a FROM t")), std::string("7"));
}

TEST(handles_see_each_others_schema_changes)
{
    tst::Scratch s("handles");
    Database db = s.open();
    Database other = db.openHandle();
    db.exec("CREATE TABLE t(a INTEGER, b TEXT)");
    CHECK_EQ(tst::only(other.exec("SELECT count(*) FROM t")), std::string("0"));
    other.exec("CREATE INDEX t_b ON t(b)");
    db.exec("INSERT INTO t VALUES (1, 'x')");
    CHECK_EQ(db.indexes("t").size(), std::size_t(1));
    other.exec("ALTER TABLE t ADD COLUMN c INTEGER");
    CHECK_EQ(db.table("t").columns.size(), std::size_t(3));
    db.exec("DROP TABLE t");
    CHECK_THROWS(other.exec("SELECT * FROM t"), ErrorCode::NoSuchTable);
}

TEST(handles_of_a_read_only_database_are_read_only)
{
    tst::Scratch s("handles");
    {
        Database db = s.open();
        db.exec("CREATE TABLE t(a INTEGER)");
    }
    {
        Database ro = Database::configure().readOnly().open(s.file());
        Database handle = ro.openHandle();
        CHECK_THROWS(handle.exec("INSERT INTO t VALUES (1)"), ErrorCode::ReadOnly);
        CHECK_EQ(tst::only(handle.exec("SELECT count(*) FROM t")), std::string("0"));
    }
    // And a reader of a normal database refuses writes.
    Database rw = s.open();
    CHECK_THROWS(rw.openReader().exec("INSERT INTO t VALUES (1)"), ErrorCode::ReadOnly);
}

TEST(handles_open_from_many_threads_while_statements_run)
{
    tst::Scratch s("handles");
    Database db = Database::configure().busyTimeout(10s).open(s.file());
    db.exec("CREATE TABLE t(a INTEGER)");
    db.exec("INSERT INTO t VALUES (1),(2),(3)");
    std::atomic<bool> stop{false};
    std::atomic<int> opened{0}, failed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            try {
                for (int round = 0; round < 25; ++round) {
                    Database h = i % 2 ? db.openHandle() : db.openReader();
                    if (tst::only(h.exec("SELECT count(*) FROM t")).empty())
                        ++failed;
                    if (i % 2)
                        h.exec("INSERT INTO t VALUES (?)", Params{Value(round)});
                    ++opened;
                }
            } catch (...) {
                ++failed;
            }
        });
    }
    while (opened.load() < 200 && failed.load() == 0) {
        db.exec("SELECT sum(a) FROM t");
        std::this_thread::sleep_for(1ms);
    }
    for (auto& t : threads)
        t.join();
    CHECK_EQ(failed.load(), 0);
    CHECK_EQ(opened.load(), 200);
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::to_string(3 + 4 * 25));
}

TEST(interrupt_stops_the_running_statement_and_not_the_next)
{
    tst::Scratch s("handles");
    Database db = s.open();
    Database h = db.openHandle();
    std::atomic<int> code{-1};
    std::thread runner([&] {
        try {
            h.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000000) SELECT count(*) FROM c");
            code = 0;
        } catch (const Error& e) {
            code = int(e.code());
        }
    });
    std::this_thread::sleep_for(200ms);
    const auto asked = std::chrono::steady_clock::now();
    h.interrupt();
    runner.join();
    CHECK_EQ(code.load(), int(ErrorCode::Interrupted));
    CHECK(std::chrono::steady_clock::now() - asked < 2s);
    // The flag does not leak into the next statement, nor does an interrupt sent between statements.
    CHECK_EQ(tst::only(h.exec("SELECT 41 + 1")), std::string("42"));
    h.interrupt();
    CHECK_EQ(tst::only(h.exec("SELECT 42 + 1")), std::string("43"));
    // The other handle was never involved.
    CHECK_EQ(tst::only(db.exec("SELECT 1")), std::string("1"));
}

int main()
{
    return tst::runAll("handles");
}
