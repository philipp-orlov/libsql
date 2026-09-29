// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sql_test.hpp"

using namespace sql;

namespace {

Database seeded(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)");
    return db;
}

}  // namespace

TEST(reportsWhatItParsed)
{
    tst::Scratch s("prepare");
    Database db = seeded(s);

    Statement insert = db.prepare("INSERT INTO t (name, n) VALUES (?, ?)");
    CHECK(insert.valid());
    CHECK_EQ(insert.parameters(), std::size_t(2));
    CHECK(insert.writes());
    CHECK_EQ(insert.sql(), std::string("INSERT INTO t (name, n) VALUES (?, ?)"));

    Statement query = db.prepare("SELECT name FROM t WHERE n > ?");
    CHECK_EQ(query.parameters(), std::size_t(1));
    CHECK(!query.writes());

    CHECK_EQ(db.prepare("SELECT 1 FROM t").parameters(), std::size_t(0));
    // Parameters are numbered across a whole batch, as they are for exec().
    CHECK_EQ(db.prepare("INSERT INTO t (name) VALUES (?); DELETE FROM t WHERE id = ?")
                 .parameters(),
             std::size_t(2));
}

TEST(runsRepeatedlyWithFreshBindings)
{
    tst::Scratch s("prepare");
    Database db = seeded(s);

    Statement insert = db.prepare("INSERT INTO t (name, n) VALUES (?, ?)");
    for (int i = 1; i <= 5; ++i) {
        Result r = insert.bind(1, Value("row" + std::to_string(i))).bind(2, Value(i * 10)).exec();
        CHECK_EQ(r.changes(), std::uint64_t(1));
        CHECK_EQ(r.lastInsertId(), std::int64_t(i));
    }

    Statement above = db.prepare("SELECT name FROM t WHERE n > ? ORDER BY n");
    CHECK_EQ(tst::flatten(above.exec({Value(25)})), std::string("row3;row4;row5"));
    CHECK_EQ(tst::flatten(above.exec({Value(45)})), std::string("row5"));
    CHECK(above.exec({Value(99)}).empty());
}

TEST(bindingIsOneBasedAndChecked)
{
    tst::Scratch s("prepare");
    Database db = seeded(s);
    Statement insert = db.prepare("INSERT INTO t (name, n) VALUES (?, ?)");

    CHECK_THROWS(insert.bind(0, Value("x")), ErrorCode::InvalidArgument);
    CHECK_THROWS(insert.bind(3, Value("x")), ErrorCode::InvalidArgument);
    CHECK_THROWS(insert.bind({Value("x")}), ErrorCode::InvalidArgument);
    CHECK_THROWS(insert.bind({Value("x"), Value(1), Value(2)}), ErrorCode::InvalidArgument);
}

TEST(unboundParametersAreNull)
{
    tst::Scratch s("prepare");
    Database db = seeded(s);

    // Only the name is bound, so n goes in NULL rather than being an error.
    db.prepare("INSERT INTO t (name, n) VALUES (?, ?)").bind(1, Value("bare")).exec();
    CHECK(db.exec("SELECT n FROM t")[0][0].isNull());

    Statement insert = db.prepare("INSERT INTO t (name, n) VALUES (?, ?)");
    insert.bind({Value("full"), Value(7)}).exec();
    insert.clear().bind(1, Value("cleared")).exec();
    CHECK_EQ(tst::flatten(db.exec("SELECT name, n FROM t ORDER BY id")),
             std::string("bare|NULL;full|7;cleared|NULL"));
}

TEST(runsInsideACallersTransaction)
{
    tst::Scratch s("prepare");
    Database db = seeded(s);
    Statement insert = db.prepare("INSERT INTO t (name) VALUES (?)");
    Statement count = db.prepare("SELECT COUNT(*) FROM t");

    {
        Transaction w = db.begin();
        insert.exec(w, {Value("kept")});
        insert.exec(w, {Value("also kept")});
        // The statement sees the transaction's own uncommitted work.
        CHECK_EQ(count.exec(w)[0][0].integer(), std::int64_t(2));
        w.commit();
    }
    {
        Transaction w = db.begin();
        insert.exec(w, {Value("lost")});
        w.rollback();
    }
    CHECK_EQ(count.exec()[0][0].integer(), std::int64_t(2));
}

TEST(refusesToWriteWhereItMayNot)
{
    tst::Scratch s("prepare");
    {
        Database db = seeded(s);
        Statement insert = db.prepare("INSERT INTO t (name) VALUES (?)");
        insert.bind(1, Value("x"));

        Transaction r = db.beginRead();
        CHECK_THROWS(insert.exec(r), ErrorCode::ReadOnly);
        CHECK_EQ(db.prepare("SELECT COUNT(*) FROM t").exec(r)[0][0].integer(), std::int64_t(0));
        r.rollback();
    }
    // A statement holds its store open, so everything above has to go out of
    // scope before a second handle can take the file.
    Database reader = Database::configure().readOnly().open(s.file());
    CHECK_THROWS(reader.prepare("INSERT INTO t (name) VALUES (?)"), ErrorCode::ReadOnly);
    CHECK_EQ(reader.prepare("SELECT COUNT(*) FROM t").exec()[0][0].integer(), std::int64_t(0));
}

TEST(reportsBadStatementsAtPrepareTime)
{
    tst::Scratch s("prepare");
    Database db = seeded(s);
    CHECK_THROWS(db.prepare("SELCT 1"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.prepare("SELECT * FROM t WHERE"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.prepare("SELECT lower(name) FROM t"), ErrorCode::Unsupported);
    // A missing table is a planning matter, so it surfaces on the run instead.
    Statement late = db.prepare("SELECT * FROM missing");
    CHECK_THROWS(late.exec(), ErrorCode::NoSuchTable);
}

TEST(followsTheSchemaItIsRunAgainst)
{
    tst::Scratch s("prepare");
    Database db = seeded(s);
    db.exec("INSERT INTO t (name, n) VALUES ('a', 1), ('b', 2), ('c', 1)");

    Statement byN = db.prepare("SELECT name FROM t WHERE n = ? ORDER BY id");
    CHECK_EQ(tst::flatten(byN.exec({Value(1)})), std::string("a;c"));

    // Planning happens per run, so an index created later is picked up without
    // re-preparing.
    db.exec("CREATE INDEX t_by_n ON t (n)");
    CHECK_EQ(tst::flatten(byN.exec({Value(1)})), std::string("a;c"));
    db.exec("DROP INDEX t_by_n");
    CHECK_EQ(tst::flatten(byN.exec({Value(1)})), std::string("a;c"));
}

TEST(outlivesTheDatabaseHandle)
{
    tst::Scratch s("prepare");
    Statement count;
    CHECK(!count.valid());
    {
        Database db = seeded(s);
        db.exec("INSERT INTO t (name) VALUES ('x')");
        count = db.prepare("SELECT COUNT(*) FROM t");
        db.close();  // the statement holds the store open
    }
    CHECK_EQ(count.exec()[0][0].integer(), std::int64_t(1));
    CHECK_THROWS(Statement().exec(), ErrorCode::InvalidArgument);
}

int main()
{
    return tst::runAll("prepare");
}
