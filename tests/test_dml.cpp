// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sql_test.hpp"

using namespace sql;

namespace {

Database seeded(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE items ("
            "  id INTEGER PRIMARY KEY,"
            "  name TEXT NOT NULL,"
            "  price REAL DEFAULT 0.0,"
            "  sku TEXT UNIQUE,"
            "  note TEXT)");
    return db;
}

}  // namespace

TEST(insertsAssignIntegerKeys)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    Result r = db.exec("INSERT INTO items (name) VALUES ('one'), ('two')");
    CHECK_EQ(r.changes(), std::uint64_t(2));
    CHECK_EQ(r.lastInsertId(), std::int64_t(2));

    r = db.exec("INSERT INTO items (id, name) VALUES (10, 'ten')");
    CHECK_EQ(r.lastInsertId(), std::int64_t(10));
    // The counter follows an explicit key, so the next one does not collide.
    r = db.exec("INSERT INTO items (name) VALUES ('eleven')");
    CHECK_EQ(r.lastInsertId(), std::int64_t(11));
}

TEST(defaultsAndNullsAreApplied)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (name) VALUES ('bare')");
    Result r = db.exec("SELECT price, sku, note FROM items");
    CHECK_EQ(r[0]["price"].real(), 0.0);
    CHECK(r[0]["sku"].isNull());
    CHECK(r[0]["note"].isNull());
}

TEST(enforcesNotNullAndUnique)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (name, sku) VALUES ('a', 'X1')");

    CHECK_THROWS(db.exec("INSERT INTO items (name) VALUES (NULL)"),
                 ErrorCode::ConstraintViolation);
    CHECK_THROWS(db.exec("INSERT INTO items (name, sku) VALUES ('b', 'X1')"),
                 ErrorCode::ConstraintViolation);
    // NULLs never collide in a unique index.
    db.exec("INSERT INTO items (name) VALUES ('c'), ('d')");
    CHECK_EQ(db.exec("SELECT id FROM items").size(), std::size_t(3));
}

TEST(duplicatePrimaryKeysAreRejected)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (id, name) VALUES (1, 'a')");
    CHECK_THROWS(db.exec("INSERT INTO items (id, name) VALUES (1, 'b')"),
                 ErrorCode::ConstraintViolation);
    CHECK_EQ(db.exec("SELECT id FROM items").size(), std::size_t(1));
}

TEST(insertChecksItsShape)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    CHECK_THROWS(db.exec("INSERT INTO items (name) VALUES ('a', 'b')"),
                 ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("INSERT INTO items (nope) VALUES (1)"), ErrorCode::NoSuchColumn);
    CHECK_THROWS(db.exec("INSERT INTO missing (a) VALUES (1)"), ErrorCode::NoSuchTable);
}

TEST(updatesRewriteMatchingRows)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (name, price) VALUES ('a', 1.0), ('b', 2.0), ('c', 3.0)");

    Result r = db.exec("UPDATE items SET price = price * 2 WHERE price >= 2.0");
    CHECK_EQ(r.changes(), std::uint64_t(2));
    CHECK_EQ(tst::flatten(db.exec("SELECT price FROM items ORDER BY id")),
             std::string("1;4;6"));

    r = db.exec("UPDATE items SET note = 'all'");
    CHECK_EQ(r.changes(), std::uint64_t(3));
    CHECK_EQ(tst::only(db.exec("SELECT note FROM items WHERE id = 1")), std::string("all"));
}

TEST(updateCanMoveThePrimaryKey)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (id, name) VALUES (1, 'a'), (2, 'b')");

    db.exec("UPDATE items SET id = 9 WHERE id = 1");
    CHECK_EQ(tst::flatten(db.exec("SELECT id, name FROM items ORDER BY id")),
             std::string("2|b;9|a"));
    CHECK_THROWS(db.exec("UPDATE items SET id = 2 WHERE id = 9"),
                 ErrorCode::ConstraintViolation);
}

TEST(updateHonoursConstraints)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (name, sku) VALUES ('a', 'X'), ('b', 'Y')");
    CHECK_THROWS(db.exec("UPDATE items SET sku = 'X' WHERE name = 'b'"),
                 ErrorCode::ConstraintViolation);
    CHECK_THROWS(db.exec("UPDATE items SET name = NULL"), ErrorCode::ConstraintViolation);
    CHECK_THROWS(db.exec("UPDATE items SET nope = 1"), ErrorCode::NoSuchColumn);
    // A failed statement leaves nothing behind.
    CHECK_EQ(tst::flatten(db.exec("SELECT sku FROM items ORDER BY id")), std::string("X;Y"));
}

TEST(deletesRemoveMatchingRows)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (name) VALUES ('a'), ('b'), ('c'), ('d')");

    Result r = db.exec("DELETE FROM items WHERE id IN (2, 3)");
    CHECK_EQ(r.changes(), std::uint64_t(2));
    CHECK_EQ(tst::flatten(db.exec("SELECT name FROM items ORDER BY id")), std::string("a;d"));

    r = db.exec("DELETE FROM items");
    CHECK_EQ(r.changes(), std::uint64_t(2));
    CHECK(db.exec("SELECT name FROM items").empty());
}

TEST(deletesKeepIndexesConsistent)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("CREATE INDEX by_name ON items (name)");
    db.exec("INSERT INTO items (name) VALUES ('keep'), ('drop')");
    db.exec("DELETE FROM items WHERE name = 'drop'");

    CHECK(db.exec("SELECT id FROM items WHERE name = 'drop'").empty());
    // The freed name can be reused, which only works if the entry really went.
    db.exec("INSERT INTO items (name) VALUES ('drop')");
    CHECK_EQ(db.exec("SELECT id FROM items WHERE name = 'drop'").size(), std::size_t(1));
}

TEST(tablesWithoutAKeyGetARowid)
{
    tst::Scratch s("dml");
    Database db = s.open();
    db.exec("CREATE TABLE log (msg TEXT)");
    db.exec("INSERT INTO log VALUES ('first'), ('second')");

    CHECK_EQ(tst::flatten(db.exec("SELECT rowid, msg FROM log ORDER BY rowid")),
             std::string("1|first;2|second"));
    // A bare star leaves the implicit key out.
    CHECK_EQ(db.exec("SELECT * FROM log").columns().size(), std::size_t(1));
    db.exec("DELETE FROM log WHERE rowid = 1");
    CHECK_EQ(tst::only(db.exec("SELECT msg FROM log")), std::string("second"));
}

TEST(parametersAreBoundByPosition)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (name, price) VALUES (?, ?)", {Value("bolt"), Value(1.25)});

    Result r = db.exec("SELECT name FROM items WHERE price < ?", {Value(2)});
    CHECK_EQ(tst::only(r), std::string("bolt"));
    CHECK_THROWS(db.exec("SELECT name FROM items WHERE price < ?"), ErrorCode::InvalidArgument);
}

TEST(transactionsCommitAndRollBack)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    {
        Transaction t = db.begin();
        t.exec("INSERT INTO items (name) VALUES ('kept')");
        t.commit();
    }
    {
        Transaction t = db.begin();
        t.exec("INSERT INTO items (name) VALUES ('lost')");
        t.rollback();
    }
    {
        Transaction t = db.begin();  // dropped without a commit
        t.exec("INSERT INTO items (name) VALUES ('also lost')");
    }
    CHECK_EQ(tst::flatten(db.exec("SELECT name FROM items")), std::string("kept"));
}

TEST(readTransactionsSeeASnapshotAndRefuseWrites)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (name) VALUES ('before')");

    Transaction r = db.beginRead();
    CHECK_EQ(r.exec("SELECT name FROM items").size(), std::size_t(1));
    CHECK_THROWS(r.exec("INSERT INTO items (name) VALUES ('nope')"), ErrorCode::ReadOnly);
    // A reader takes a snapshot rather than the writer slot, so a write can
    // still get through while it is open, and the reader does not see it.
    db.exec("INSERT INTO items (name) VALUES ('after')");
    CHECK_EQ(r.exec("SELECT name FROM items").size(), std::size_t(1));
    r.commit();
    CHECK_EQ(db.exec("SELECT name FROM items").size(), std::size_t(2));
}

TEST(aFailedStatementRollsBackTheWholeBatch)
{
    tst::Scratch s("dml");
    Database db = seeded(s);
    db.exec("INSERT INTO items (id, name) VALUES (1, 'a')");
    CHECK_THROWS(db.exec("INSERT INTO items (id, name) VALUES (2, 'b');"
                         "INSERT INTO items (id, name) VALUES (1, 'clash')"),
                 ErrorCode::ConstraintViolation);
    CHECK_EQ(db.exec("SELECT id FROM items").size(), std::size_t(1));
}

int main()
{
    return tst::runAll("dml");
}
