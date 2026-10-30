// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Database::openReader(): per-thread read-only handles on one open store.

#include <atomic>
#include <thread>
#include <vector>

#include "sql_test.hpp"

using namespace sql;

TEST(readerSeesCommittedDataAndRejectsWrites)
{
    tst::Scratch s("reader-basic");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER)");
    db.exec("INSERT INTO t (n) VALUES (1)");

    Database reader = db.openReader();
    CHECK(reader.valid());
    CHECK_EQ(tst::only(reader.exec("SELECT COUNT(*) FROM t")), std::string("1"));

    CHECK_THROWS(reader.exec("INSERT INTO t (n) VALUES (2)"), ErrorCode::ReadOnly);
    CHECK_THROWS(reader.exec("CREATE TABLE u (a INTEGER)"), ErrorCode::ReadOnly);
    CHECK_THROWS(reader.prepare("DELETE FROM t"), ErrorCode::ReadOnly);

    // Commits, including DDL, made through the writer show up in the reader.
    db.exec("INSERT INTO t (n) VALUES (2)");
    db.exec("CREATE TABLE u (a INTEGER)");
    CHECK_EQ(tst::only(reader.exec("SELECT COUNT(*) FROM t")), std::string("2"));
    CHECK_EQ(reader.tables().size(), std::size_t(2));
}

TEST(readerOutlivesTheDatabaseItCameFrom)
{
    tst::Scratch s("reader-outlive");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER)");
    db.exec("INSERT INTO t (n) VALUES (7)");
    Database reader = db.openReader();
    db.close();
    CHECK_EQ(tst::only(reader.exec("SELECT n FROM t")), std::string("7"));

    Database closed;
    CHECK_THROWS(closed.openReader(), ErrorCode::InvalidArgument);
}

TEST(readersRunConcurrentlyWithTheWriter)
{
    tst::Scratch s("reader-threads");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER)");
    db.exec("CREATE TABLE u (id INTEGER PRIMARY KEY, n INTEGER)");

    constexpr int kReaders = 4;
    constexpr int kWrites = 300;
    std::vector<Database> readers;
    for (int i = 0; i < kReaders; ++i)
        readers.push_back(db.openReader());

    std::atomic<bool> done{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kReaders; ++i) {
        threads.emplace_back([&, i] {
            try {
                Statement countT = readers[std::size_t(i)].prepare("SELECT COUNT(*) FROM t");
                Statement countU = readers[std::size_t(i)].prepare("SELECT COUNT(*) FROM u");
                long last = 0;
                while (!done.load()) {
                    // One transaction sees both tables at the same instant, and
                    // the writer keeps them equal within every commit.
                    Transaction txn = readers[std::size_t(i)].beginRead();
                    const long a = std::stol(tst::only(countT.exec(txn)));
                    const long b = std::stol(tst::only(countU.exec(txn)));
                    txn.rollback();
                    if (a != b || a < last)
                        ++failures;
                    last = a;
                }
            } catch (...) {
                ++failures;
            }
        });
    }

    for (int i = 0; i < kWrites; ++i) {
        Transaction txn = db.begin();
        txn.exec("INSERT INTO t (n) VALUES (1)");
        txn.exec("INSERT INTO u (n) VALUES (1)");
        txn.commit();
    }
    done = true;
    for (std::thread& t : threads)
        t.join();

    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(tst::only(readers[0].exec("SELECT COUNT(*) FROM t")), std::to_string(kWrites));
}

int main() { return tst::runAll("reader"); }
