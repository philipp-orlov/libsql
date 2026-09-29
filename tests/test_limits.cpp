// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Statements that are valid but pathological must be refused, not recurse
// until the stack is gone.

#include "nosql/nosql.hpp"
#include "sql_test.hpp"

using namespace sql;

namespace {
Database seeded(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER)");
    db.exec("INSERT INTO t (n) VALUES (1)");
    return db;
}

std::string repeat(const std::string& piece, std::size_t count)
{
    std::string out;
    out.reserve(piece.size() * count);
    for (std::size_t i = 0; i < count; ++i)
        out += piece;
    return out;
}
}  // namespace

TEST(reasonableNestingStillWorks)
{
    tst::Scratch s("limits-ok");
    Database db = seeded(s);
    CHECK_EQ(tst::only(db.exec("SELECT " + repeat("(", 100) + "n" + repeat(")", 100) + " FROM t")),
             std::string("1"));
    // A long flat chain is fine: 1000 terms is a tree 1000 high, well within bounds.
    CHECK_EQ(tst::only(db.exec("SELECT n" + repeat(" + 1", 1000) + " FROM t")), std::string("1001"));
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM t WHERE n = 1" + repeat(" OR n = 2", 500))),
             std::string("1"));
}

TEST(deepParenthesesAreRefused)
{
    tst::Scratch s("limits-parens");
    Database db = seeded(s);
    CHECK_THROWS(db.exec("SELECT " + repeat("(", 200) + "n" + repeat(")", 200) + " FROM t"),
                 ErrorCode::SyntaxError);
    // Far past anything a stack could hold: an error, not a crash.
    CHECK_THROWS(db.exec("SELECT " + repeat("(", 200000) + "n" + repeat(")", 200000) + " FROM t"),
                 ErrorCode::SyntaxError);
}

TEST(deepNotAndSignChainsAreRefused)
{
    tst::Scratch s("limits-unary");
    Database db = seeded(s);
    CHECK_THROWS(db.exec("SELECT n FROM t WHERE " + repeat("NOT ", 300) + "n = 1"),
                 ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT " + repeat("- ", 300) + "n FROM t"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT " + repeat("+ ", 100000) + "n FROM t"), ErrorCode::SyntaxError);
}

TEST(tooTallLeftDeepChainsAreRefused)
{
    tst::Scratch s("limits-chain");
    Database db = seeded(s);
    CHECK_THROWS(db.exec("SELECT n" + repeat(" + 1", 5000) + " FROM t"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT n FROM t WHERE n IS NULL" + repeat(" IS NULL", 5000)),
                 ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT n FROM t WHERE n = 1" + repeat(" AND n = 1", 5000)),
                 ErrorCode::SyntaxError);
}

TEST(oversizedStatementsAreRefused)
{
    tst::Scratch s("limits-size");
    Database db = seeded(s);
    const std::string huge = "SELECT '" + std::string(2u << 20, 'x') + "' FROM t";
    CHECK_THROWS(db.exec(huge), ErrorCode::SyntaxError);
    CHECK_THROWS(db.prepare(huge), ErrorCode::SyntaxError);
    // The same text as a bound parameter is the way to carry a large value.
    Statement insert = db.prepare("INSERT INTO t (n) VALUES (?)");
    CHECK_EQ(insert.exec({Value(7)}).changes(), std::uint64_t(1));
}

TEST(maxSizeBoundsTheFile)
{
    tst::Scratch s("limits-size");
    const std::uint64_t cap = 2u << 20;
    Database db = Database::configure().maxSize(cap).open(s.file());
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)");
    Statement insert = db.prepare("INSERT INTO t (s) VALUES (?)");
    const std::string payload(1024, 'p');
    bool full = false;
    try {
        for (int i = 0; i < 100000; ++i)
            insert.exec({Value(payload)});
    } catch (const ::nosql::Error& e) {
        full = e.code() == nosql::ErrorCode::MapFull;
    }
    CHECK(full);
    CHECK(std::filesystem::file_size(s.file()) <= cap);
    // The store is still consistent and readable after the refusal.
    CHECK(std::stol(tst::only(db.exec("SELECT COUNT(*) FROM t"))) > 0);
}

int main()
{
    return tst::runAll("limits");
}
