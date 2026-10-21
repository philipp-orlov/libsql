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
    CHECK_THROWS(db.prepare("SELECT no_such_function(name) FROM t"), ErrorCode::Unsupported);
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

TEST(bindsBracedValuesInPlace)
{
    tst::Scratch s("prepare-braced");
    Database db = seeded(s);
    Statement insert = db.prepare("INSERT INTO t (name, n) VALUES (?, ?)");
    const std::string longName(100, 'q');
    {
        Transaction txn = db.begin();
        insert.exec(txn, {Value("a"), Value(1)});
        // A borrowing value stays a borrow until the run is over.
        insert.exec(txn, {Value::textRef(longName), Value(2)});
        txn.commit();
    }
    insert.exec({Value(longName), Value()});
    CHECK_THROWS(insert.exec({Value("x")}), ErrorCode::InvalidArgument);
    CHECK_EQ(tst::flatten(db.exec("SELECT length(name), n FROM t ORDER BY id")),
             std::string("1|1;100|2;100|NULL"));
    // A named Params vector still binds the old way.
    const Params params{Value("p"), Value(4)};
    CHECK_EQ(insert.exec(params).changes(), std::uint64_t(1));
}

TEST(execIntoRefillsOneResult)
{
    tst::Scratch s("prepare-into");
    Database db = seeded(s);
    db.exec("INSERT INTO t (name, n) VALUES ('a', 1), ('bb', 2), ('ccc', 3), ('dddd', 4)");
    Statement upTo =
        db.prepare("SELECT id, name, n, n * 2, name || '!' FROM t WHERE n <= ? ORDER BY id");
    Statement insert = db.prepare("INSERT INTO t (name, n) VALUES (?, ?)");
    Result r;
    Transaction txn = db.begin();
    CHECK_EQ(tst::flatten(upTo.bind(1, Value(3)).execInto(txn, r)),
             std::string("1|a|1|2|a!;2|bb|2|4|bb!;3|ccc|3|6|ccc!"));
    CHECK_EQ(tst::flatten(upTo.bind(1, Value(1)).execInto(txn, r)), std::string("1|a|1|2|a!"));
    CHECK_EQ(tst::flatten(upTo.bind(1, Value(9)).execInto(txn, r)),
             std::string("1|a|1|2|a!;2|bb|2|4|bb!;3|ccc|3|6|ccc!;4|dddd|4|8|dddd!"));
    CHECK_EQ(r.columns().size(), std::size_t(5));
    // A write into the same Result reports its counters and no rows.
    insert.bind(1, Value("e")).bind(2, Value(5)).execInto(txn, r);
    CHECK(r.empty());
    CHECK_EQ(r.changes(), std::uint64_t(1));
    CHECK_EQ(r.lastInsertId(), std::int64_t(5));
    CHECK_EQ(upTo.bind(1, Value(0)).execInto(txn, r).size(), std::size_t(0));
    // A copy owns its rows; the original is refilled under it.
    upTo.bind(1, Value(2)).execInto(txn, r);
    const Result copy = r;
    upTo.bind(1, Value(9)).execInto(txn, r);
    txn.commit();
    CHECK_EQ(tst::flatten(copy), std::string("1|a|1|2|a!;2|bb|2|4|bb!"));
    CHECK_EQ(r.size(), std::size_t(5));
}

TEST(eachTakesAnyCallable)
{
    tst::Scratch s("prepare-each");
    Database db = seeded(s);
    db.exec("INSERT INTO t (name, n) VALUES ('a', 1), ('b', 2), ('c', 3)");
    Statement all = db.prepare("SELECT name FROM t ORDER BY id");
    std::string seen;
    // A lambda that captures more than a std::function could hold inline.
    std::string a, b, c, d;
    auto wide = [&seen, &a, &b, &c, &d](const RowView& row) {
        seen += row[0].text();
        return !(a.empty() && b.empty() && c.empty() && d.empty()) || seen.size() < 2;
    };
    CHECK_EQ(all.each(wide), std::uint64_t(0));
    CHECK_EQ(seen, std::string("ab"));
    const RowCallback fn = [&](const RowView& row) {
        seen += row[0].text();
        return true;
    };
    Transaction txn = db.beginRead();
    all.each(txn, fn);
    all.each(txn, [&](const RowView&) { return false; });
    CHECK_EQ(seen, std::string("ababc"));
}

int main()
{
    return tst::runAll("prepare");
}
