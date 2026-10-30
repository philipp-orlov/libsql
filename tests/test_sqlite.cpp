// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The parts of the SQLite language that live at the API boundary: parameters
// by name, SQL transaction control, statement atomicity, conflict algorithms,
// connection counters, attached and temporary stores, and FOR JSON inside the
// larger language. Statement semantics themselves are checked against SQLite
// by tests/conformance.

#include <atomic>
#include <thread>

#include "sql_test.hpp"
#include "sql/json.hpp"
#include "nosql/nosql.hpp"

using namespace sql;

TEST(namedParametersBindByName)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a, b)");
    Statement ins = db.prepare("INSERT INTO t VALUES (:a, $b || :a)");
    CHECK_EQ(ins.parameters(), std::size_t(2));
    CHECK_EQ(ins.parameterIndex(":a"), std::size_t(1));
    CHECK_EQ(ins.parameterIndex("$b"), std::size_t(2));
    CHECK_EQ(ins.parameterName(2), std::string("$b"));
    CHECK_EQ(ins.parameterIndex(":missing"), std::size_t(0));
    ins.exec(NamedParams{{":a", Value(1)}, {"$b", Value("x")}});
    db.exec("INSERT INTO t VALUES (?2, ?1)", {Value("first"), Value("second")});
    db.exec("INSERT INTO t VALUES (@v, @v)", NamedParams{{"@v", Value(7)}});
    CHECK_EQ(tst::flatten(db.exec("SELECT a, b FROM t ORDER BY rowid")),
             std::string("1|x1;second|first;7|7"));
    CHECK_THROWS(ins.bind(":nope", Value(1)), ErrorCode::InvalidArgument);
}

TEST(connectionCountersFollowSqlite)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v)");
    db.exec("INSERT INTO t (v) VALUES (1), (2), (3)");
    CHECK_EQ(db.changes(), std::int64_t(3));
    CHECK_EQ(db.lastInsertRowid(), std::int64_t(3));
    db.exec("UPDATE t SET v = v + 1 WHERE id > 1");
    CHECK_EQ(db.changes(), std::int64_t(2));
    CHECK_EQ(db.totalChanges(), std::int64_t(5));
    CHECK_EQ(tst::only(db.exec("SELECT changes()")), std::string("2"));
    CHECK_EQ(tst::only(db.exec("SELECT last_insert_rowid()")), std::string("3"));
}

TEST(sqlTransactionsSpanCalls)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (v)");
    CHECK(!db.inTransaction());
    db.exec("BEGIN");
    CHECK(db.inTransaction());
    db.exec("INSERT INTO t VALUES (1)");
    db.exec("INSERT INTO t VALUES (2)");
    {
        // Another handle on the store sees nothing until COMMIT.
        Database reader = db.openReader();
        CHECK_EQ(tst::only(reader.exec("SELECT count(*) FROM t")), std::string("0"));
    }
    db.exec("ROLLBACK");
    CHECK(!db.inTransaction());
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("0"));

    db.exec("BEGIN IMMEDIATE; INSERT INTO t VALUES (3); COMMIT");
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("1"));
    CHECK_THROWS(db.exec("COMMIT"), ErrorCode::Runtime);
    db.exec("BEGIN");
    CHECK_THROWS(db.exec("BEGIN"), ErrorCode::Runtime);
    CHECK_THROWS(db.begin(), ErrorCode::BadTransaction);
    db.exec("END");
}

TEST(savepointsNestAndRollBackPartially)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (v)");
    db.exec("SAVEPOINT outer_sp");
    CHECK(db.inTransaction());
    db.exec("INSERT INTO t VALUES (1)");
    db.exec("SAVEPOINT inner_sp");
    db.exec("INSERT INTO t VALUES (2)");
    db.exec("CREATE TABLE gone (x)");
    db.exec("ROLLBACK TO inner_sp");
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("1"));
    CHECK_THROWS(db.exec("SELECT * FROM gone"), ErrorCode::NoSuchTable);
    db.exec("INSERT INTO t VALUES (3)");
    db.exec("RELEASE inner_sp");
    db.exec("RELEASE outer_sp");  // the outermost savepoint commits
    CHECK(!db.inTransaction());
    CHECK_EQ(tst::flatten(db.exec("SELECT v FROM t ORDER BY v")), std::string("1;3"));
    CHECK_THROWS(db.exec("RELEASE never_made"), ErrorCode::Runtime);

    // Savepoints work inside an API transaction too.
    Transaction txn = db.begin();
    txn.exec("SAVEPOINT a");
    txn.exec("INSERT INTO t VALUES (9)");
    txn.exec("ROLLBACK TO a");
    txn.commit();
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("2"));
}

TEST(aFailedStatementLeavesNothingInsideATransaction)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)");
    Transaction txn = db.begin();
    txn.exec("INSERT INTO t VALUES (1)");
    // The third row collides: the whole statement goes, the earlier one stays
    // (ledger C9 -- this used to leave the statement's first rows behind).
    CHECK_THROWS(txn.exec("INSERT INTO t VALUES (2), (3), (1)"), ErrorCode::ConstraintViolation);
    txn.exec("INSERT INTO t VALUES (4)");
    txn.commit();
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t")), std::string("1;4"));
}

TEST(conflictAlgorithmsResolveAsSqliteDoes)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v)");
    db.exec("INSERT INTO t VALUES (1, 'a'), (2, 'b')");
    CHECK_EQ(db.exec("INSERT OR IGNORE INTO t VALUES (1, 'x'), (3, 'c')").changes(), std::uint64_t(1));
    db.exec("INSERT OR REPLACE INTO t VALUES (2, 'B')");
    db.exec("REPLACE INTO t VALUES (4, 'd')");
    CHECK_EQ(tst::flatten(db.exec("SELECT v FROM t ORDER BY id")), std::string("a;B;c;d"));

    // FAIL keeps the rows the statement changed before the failing one.
    db.exec("BEGIN");
    CHECK_THROWS(db.exec("INSERT OR FAIL INTO t VALUES (5, 'e'), (1, 'dup'), (6, 'f')"),
                 ErrorCode::ConstraintViolation);
    db.exec("COMMIT");
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t WHERE id >= 5")), std::string("1"));

    // ROLLBACK ends the whole transaction.
    db.exec("BEGIN");
    db.exec("INSERT INTO t VALUES (7, 'g')");
    CHECK_THROWS(db.exec("INSERT OR ROLLBACK INTO t VALUES (1, 'dup')"), ErrorCode::ConstraintViolation);
    CHECK(!db.inTransaction());
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t WHERE id = 7")), std::string("0"));
}

TEST(deferredForeignKeysAreCheckedAtCommit)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("PRAGMA foreign_keys = ON");
    db.exec("CREATE TABLE p (id INTEGER PRIMARY KEY)");
    db.exec("CREATE TABLE c (pid REFERENCES p(id) DEFERRABLE INITIALLY DEFERRED)");
    db.exec("BEGIN");
    db.exec("INSERT INTO c VALUES (1)");
    CHECK_THROWS(db.exec("COMMIT"), ErrorCode::ConstraintViolation);
    CHECK(db.inTransaction());  // still open, as in SQLite
    db.exec("INSERT INTO p VALUES (1)");
    db.exec("COMMIT");
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM c")), std::string("1"));

    // Immediate constraints fail the statement.
    db.exec("CREATE TABLE ic (pid REFERENCES p(id))");
    try {
        db.exec("INSERT INTO ic VALUES (42)");
        CHECK(false);
    } catch (const Error& e) {
        CHECK_EQ(e.constraint(), Constraint::ForeignKey);
    }
}

TEST(constraintKindsAreReported)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, u UNIQUE, n NOT NULL, c CHECK (c > 0))");
    db.exec("INSERT INTO t VALUES (1, 1, 1, 1)");
    const auto kind = [&](const char* sqlText) {
        try {
            db.exec(sqlText);
        } catch (const Error& e) {
            return e.constraint();
        }
        return Constraint::None;
    };
    CHECK(kind("INSERT INTO t VALUES (1, 2, 1, 1)") == Constraint::PrimaryKey);
    CHECK(kind("INSERT INTO t VALUES (2, 1, 1, 1)") == Constraint::Unique);
    CHECK(kind("INSERT INTO t VALUES (2, 2, NULL, 1)") == Constraint::NotNull);
    CHECK(kind("INSERT INTO t VALUES (2, 2, 1, -1)") == Constraint::Check);
    db.exec("CREATE TABLE s (i INTEGER) STRICT");
    CHECK(kind("INSERT INTO s VALUES ('x')") == Constraint::Datatype);
}

TEST(completeKnowsWhereAStatementEnds)
{
    CHECK(sql::complete("SELECT 1;"));
    CHECK(!sql::complete("SELECT 1"));
    CHECK(!sql::complete("SELECT ';"));
    CHECK(!sql::complete("CREATE TRIGGER t AFTER INSERT ON x BEGIN SELECT 1;"));
    CHECK(sql::complete("CREATE TRIGGER t AFTER INSERT ON x BEGIN SELECT 1; END;"));
    CHECK(sql::complete("SELECT 1; -- trailing comment"));
}

TEST(interruptStopsALongStatement)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    std::atomic<bool> stop{false};
    std::thread killer([&] {
        while (!stop.load()) {
            db.interrupt();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    CHECK_THROWS(db.exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n)"
                         " SELECT count(*) FROM n, n AS m"),
                 ErrorCode::Interrupted);
    stop = true;
    killer.join();
    CHECK_EQ(tst::only(db.exec("SELECT 1")), std::string("1"));
}

TEST(attachedAndTemporaryStores)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (v)");
    db.exec("INSERT INTO t VALUES ('main')");
    db.exec("CREATE TEMP TABLE t (v)");
    db.exec("INSERT INTO t VALUES ('temp')");
    CHECK_EQ(tst::only(db.exec("SELECT v FROM t")), std::string("temp"));
    CHECK_EQ(tst::only(db.exec("SELECT v FROM main.t")), std::string("main"));

    const std::string other = s.file("other.db").string();
    db.exec("ATTACH ? AS aux", {Value(other)});
    db.exec("CREATE TABLE aux.t (v)");
    db.exec("INSERT INTO aux.t VALUES ('aux')");
    CHECK_EQ(tst::flatten(db.exec("SELECT name FROM pragma_database_list ORDER BY seq")),
             std::string("main;temp;aux"));
    CHECK_EQ(tst::only(db.exec("SELECT v FROM aux.t")), std::string("aux"));
    db.exec("DETACH aux");
    CHECK_THROWS(db.exec("SELECT v FROM aux.t"), ErrorCode::NoSuchTable);

    Database mem = Database::configure().open(":memory:");
    mem.exec("CREATE TABLE m (v); INSERT INTO m VALUES (1)");
    CHECK_EQ(tst::only(mem.exec("SELECT v FROM m")), std::string("1"));
}

TEST(vacuumIntoCopiesTheStore)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v); CREATE INDEX t_v ON t (v)");
    db.exec("INSERT INTO t (v) VALUES ('a'), ('b')");
    const std::string copy = s.file("copy.db").string();
    db.exec("VACUUM INTO ?", {Value(copy)});
    CHECK_THROWS(db.exec("VACUUM INTO ?", {Value(copy)}), ErrorCode::Runtime);
    Database c = Database::configure().open(copy);
    CHECK_EQ(tst::flatten(c.exec("SELECT v FROM t WHERE v = 'b'")), std::string("b"));
}

TEST(forJsonInsideTheLargerLanguage)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE c (id INTEGER PRIMARY KEY, name TEXT)");
    db.exec("CREATE TABLE l (id INTEGER PRIMARY KEY, cid INTEGER, item TEXT)");
    db.exec("INSERT INTO c VALUES (1, 'Ada'), (2, 'Bo')");
    db.exec("INSERT INTO l VALUES (1, 1, 'anvil'), (2, 1, 'rope'), (3, 2, 'kite')");

    // A nested FOR JSON is embedded, not quoted.
    CHECK_EQ(tst::only(db.exec("SELECT c.name, (SELECT l.item FROM l WHERE l.cid = c.id ORDER BY l.id"
                               " FOR JSON PATH) AS items FROM c ORDER BY c.id FOR JSON PATH")),
             std::string(R"([{"name":"Ada","items":[{"item":"anvil"},{"item":"rope"}]},)"
                         R"({"name":"Bo","items":[{"item":"kite"}]}])"));
    // So are JSON functions' results.
    CHECK_EQ(tst::only(db.exec("SELECT c.name, json_group_array(l.item) AS items FROM c JOIN l"
                               " ON l.cid = c.id GROUP BY c.id ORDER BY c.id FOR JSON PATH")),
             std::string(R"([{"name":"Ada","items":["anvil","rope"]},{"name":"Bo","items":["kite"]}])"));
    // From a CTE, with AUTO nesting by the FROM clause as written.
    CHECK_EQ(tst::only(db.exec("WITH big AS (SELECT * FROM c WHERE id = 1)"
                               " SELECT big.name, l.item FROM big JOIN l ON l.cid = big.id"
                               " ORDER BY l.id FOR JSON AUTO")),
             std::string(R"([{"name":"Ada","l":[{"item":"anvil"},{"item":"rope"}]}])"));
    // `for` is an ordinary column name now.
    db.exec("CREATE TABLE f (for INTEGER)");
    db.exec("INSERT INTO f VALUES (1)");
    CHECK_EQ(tst::only(db.exec("SELECT for FROM f FOR JSON PATH")), std::string(R"([{"for":1}])"));
}

TEST(schemaTablesDescribeTheStore)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a INTEGER PRIMARY KEY, b TEXT UNIQUE)");
    db.exec("CREATE INDEX t_b ON t (b DESC)");
    db.exec("CREATE VIEW v AS SELECT a FROM t");
    CHECK_EQ(tst::flatten(db.exec("SELECT type, name, tbl_name FROM sqlite_schema ORDER BY name")),
             std::string("index|sqlite_autoindex_t_1|t;table|t|t;index|t_b|t;view|v|v"));
    CHECK_EQ(tst::only(db.exec("SELECT sql FROM sqlite_master WHERE name = 't_b'")),
             std::string("CREATE INDEX t_b ON t (b DESC)"));
    CHECK_EQ(db.table("t").sql, std::string("CREATE TABLE t (a INTEGER PRIMARY KEY, b TEXT UNIQUE)"));
    db.exec("PRAGMA user_version = 7");
    CHECK_EQ(tst::only(db.exec("PRAGMA user_version")), std::string("7"));
    CHECK_EQ(tst::only(db.exec("PRAGMA integrity_check")), std::string("ok"));
}

TEST(analyzeFillsSqliteStat1)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a, b); CREATE INDEX ta ON t (a);"
            "INSERT INTO t VALUES (1, 1), (1, 2), (2, 3)");
    // The first ANALYZE creates sqlite_stat1, which sorts ahead of t: the rows
    // must still land in sqlite_stat1, not in t.
    db.exec("ANALYZE");
    CHECK_EQ(tst::flatten(db.exec("SELECT * FROM sqlite_stat1")), std::string("t|ta|3 2"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("3"));
    CHECK_EQ(tst::only(db.exec("PRAGMA integrity_check")), std::string("ok"));
    db.exec("UPDATE sqlite_stat1 SET stat = '3 1'");
    CHECK_EQ(tst::only(db.exec("SELECT stat FROM sqlite_stat1")), std::string("3 1"));
    CHECK_THROWS(db.exec("DELETE FROM sqlite_schema"), ErrorCode::ReadOnly);
}

TEST(preparedStatementsReplanAfterEveryDdlInATransaction)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE z (q)");
    Statement ins = db.prepare("INSERT INTO b VALUES (1)");
    Statement sel = db.prepare("SELECT * FROM b");
    Transaction t = db.begin();
    t.exec("CREATE TABLE b (x)");
    ins.exec(t);
    CHECK_EQ(sel.exec(t).size(), std::size_t(1));
    // The second DDL edits the transaction's schema copy in place; the plans
    // made against the first edit must not survive it.
    t.exec("DROP TABLE b; CREATE TABLE a (p); CREATE TABLE b (x, y)");
    CHECK_THROWS(ins.exec(t), ErrorCode::InvalidArgument);
    const Result r = sel.exec(t);
    CHECK_EQ(r.size(), std::size_t(0));
    CHECK_EQ(r.columns().size(), std::size_t(2));
    t.commit();
}

TEST(updatesKeepEveryKindOfIndexExact)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE u (a INTEGER PRIMARY KEY, b, c, d);"
            "CREATE UNIQUE INDEX ub ON u (b);"
            "CREATE INDEX uc ON u (lower(c));"
            "CREATE INDEX ud ON u (d) WHERE d > 10;"
            "INSERT INTO u VALUES (1, 'x', 'Ab', 5), (2, 'y', 'cd', 20), (3, 'z', 'EF', 30)");
    db.exec("UPDATE u SET c = 'AB' WHERE a = 1");    // expression entry unchanged
    db.exec("UPDATE u SET d = 50 WHERE a = 1");      // enters the partial index
    db.exec("UPDATE u SET d = 1 WHERE a = 2");       // leaves it
    db.exec("UPDATE u SET d = 31 WHERE a = 3");      // moves within it
    db.exec("UPDATE u SET b = 'w' WHERE a = 2");     // unique entry changes
    db.exec("UPDATE u SET b = b WHERE a = 3");       // unique entry unchanged: no self-conflict
    CHECK_THROWS(db.exec("UPDATE u SET b = 'x' WHERE a = 3"), ErrorCode::ConstraintViolation);
    db.exec("UPDATE u SET a = 7 WHERE a = 1");       // the row moves: every entry follows
    CHECK_EQ(tst::only(db.exec("PRAGMA integrity_check")), std::string("ok"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a FROM u WHERE b = 'w'")), std::string("2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a FROM u WHERE lower(c) = 'ab'")), std::string("7"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a FROM u WHERE d > 10 ORDER BY d")), std::string("3;7"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a FROM u WHERE b = 'x'")), std::string("7"));
}

TEST(integrityCheckFindsEntriesMissingFromAnIndex)
{
    tst::Scratch s("sqlite");
    {
        Database db = s.open();
        db.exec("CREATE TABLE t (a INTEGER PRIMARY KEY, b); CREATE INDEX tb ON t (lower(b));"
                "INSERT INTO t VALUES (1, 'X'), (2, 'Y')");
        CHECK_EQ(tst::only(db.exec("PRAGMA integrity_check")), std::string("ok"));
        const std::string sub = "i#" + tst::only(db.exec("SELECT rootpage FROM sqlite_schema WHERE name = 'tb'"));
        // Swap row 2's entry for one no row implies.
        db.env().write([&](nosql::Txn& t) {
            nosql::Db ix = t.db(nosql::Slice(sub.data(), sub.size()));
            nosql::Cursor cur = ix.cursor();
            CHECK(cur.last());
            std::string entry(cur.key().view());
            ix.erase(nosql::Slice(entry.data(), entry.size()));
            entry.back() ^= 0x7f;
            ix.put(nosql::Slice(entry.data(), entry.size()), nosql::Slice());
        });
    }
    Database db = s.open();
    CHECK_EQ(tst::flatten(db.exec("PRAGMA integrity_check")), std::string("row 2 missing from index tb"));
    db.exec("REINDEX tb");
    CHECK_EQ(tst::only(db.exec("PRAGMA integrity_check")), std::string("ok"));
}

TEST(busyTimeoutBoundsTheWaitForTheWriter)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a)");
    CHECK_EQ(tst::only(db.exec("PRAGMA busy_timeout")), std::string("0"));
    db.exec("PRAGMA busy_timeout = 50");
    CHECK_EQ(tst::only(db.exec("PRAGMA busy_timeout")), std::string("50"));
    Transaction held = db.begin();
    held.exec("INSERT INTO t VALUES (1)");
    std::atomic<bool> busy{false};
    std::thread other([&] {
        try {
            db.exec("INSERT INTO t VALUES (2)");
        } catch (const Error& e) {
            busy = e.code() == ErrorCode::Busy && std::string(e.what()) == "database is locked";
        }
    });
    other.join();
    CHECK(busy.load());
    // Reads never wait.
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t")), std::string("0"));
    // A writer that finishes within the timeout lets the waiter through.
    db.exec("PRAGMA busy_timeout = 10000");
    std::thread waiter([&] { db.exec("INSERT INTO t VALUES (3)"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    held.commit();
    waiter.join();
    CHECK_EQ(tst::flatten(db.exec("SELECT a FROM t ORDER BY a")), std::string("1;3"));
    Database other2 = Database::configure().busyTimeout(std::chrono::milliseconds(25)).open(s.file("b.db"));
    CHECK_EQ(tst::only(other2.exec("PRAGMA busy_timeout")), std::string("25"));
}

TEST(closingWithAnOpenBeginRollsBackAndFreesTheWriter)
{
    tst::Scratch s("sqlite");
    {
        Database db = s.open();
        db.exec("CREATE TABLE t (a)");
        db.exec("BEGIN; INSERT INTO t VALUES (1)");
        CHECK(db.inTransaction());
        Database savepoint = Database::configure().open(s.file("b.sqldb"));
        savepoint.exec("SAVEPOINT p");
    }
    Database db = Database::configure().busyTimeout(std::chrono::milliseconds(25)).open(s.file());
    db.exec("INSERT INTO t VALUES (2)");
    CHECK_EQ(tst::flatten(db.exec("SELECT a FROM t")), std::string("2"));
}

TEST(eachStreamsRowsWithoutAResult)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
    Statement ins = db.prepare("INSERT INTO t (name) VALUES (?) RETURNING id, name");
    std::string returned;
    CHECK_EQ(ins.bind(1, Value("a")).each([&](const RowView& r) {
        returned += r[0].toText() + r["name"].toText();
        return true;
    }), std::uint64_t(1));
    CHECK_EQ(returned, std::string("1a"));
    for (int i = 0; i < 20; ++i)
        db.exec("INSERT INTO t (name) VALUES (?)", {Value("n" + std::to_string(i))});
    Statement sel = db.prepare("SELECT t.id, name FROM t WHERE id > ? ORDER BY id");
    sel.bind(1, Value(5));
    std::string seen;
    std::vector<Row> kept;
    sel.each([&](const RowView& r) {
        seen += r.at("t.id").toText() + ",";
        CHECK_EQ(r.columns().size(), std::size_t(2));
        kept.push_back(r.row());
        return r[0].integer() < 8;
    });
    CHECK_EQ(seen, std::string("6,7,8,"));
    CHECK_EQ(kept.size(), std::size_t(3));
    CHECK_EQ(kept[2]["name"].toText(), std::string("n6"));
    // The same rows exec() would return, also inside a transaction.
    Transaction t = db.beginRead();
    const Result expected = sel.exec(t);
    std::size_t n = 0;
    sel.each(t, [&](const RowView& r) {
        CHECK_EQ(r[1].toText(), expected[n][1].toText());
        ++n;
        return true;
    });
    CHECK_EQ(n, expected.size());
    CHECK_THROWS(sel.each([](const RowView& r) { return r.at(5).isNull(); }), ErrorCode::InvalidArgument);
    // The statement cannot run again from its own callback; it can afterwards.
    CHECK_THROWS(sel.each(t, [&](const RowView&) { return sel.exec(t).empty(); }), ErrorCode::InvalidArgument);
    CHECK_EQ(sel.exec(t).size(), expected.size());
}

TEST(streamedCellsBorrowAndKeptCellsOwn)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE a (id INTEGER PRIMARY KEY, name TEXT, tag BLOB)");
    db.exec("CREATE TABLE b (id INTEGER PRIMARY KEY, a_id INTEGER, note TEXT)");
    db.exec("CREATE INDEX a_by_name ON a (name)");
    {
        // Short names fit inside a cell; every third one needs a buffer.
        Transaction t = db.begin();
        for (int i = 1; i <= 300; ++i) {
            const std::string name = "name-" + std::to_string(i) + std::string(i % 3 ? 0 : 40, 'z');
            const std::string tag(std::size_t(i % 50), char(i));
            t.exec("INSERT INTO a (id, name, tag) VALUES (?, ?, ?)",
                   {Value(i), Value(name), Value::blob(tag.data(), tag.size())});
            for (int k = 0; k < 2; ++k)
                t.exec("INSERT INTO b (a_id, note) VALUES (?, ?)",
                       {Value(i), Value("note " + std::to_string(k))});
        }
        t.commit();
    }
    const std::string join =
        "SELECT a.id, a.name, a.tag, b.note FROM a JOIN b ON b.a_id = a.id WHERE a.id <= 120";
    const Result expected = db.exec(join);
    CHECK_EQ(expected.size(), std::size_t(240));
    // A Result owns every cell, sorted or not.
    for (const Result& r : {expected, db.exec("SELECT name, tag FROM a ORDER BY name")}) {
        for (const Row& row : r) {
            for (const Value& v : row)
                CHECK(!v.borrowed());
        }
    }

    // Read-only streaming hands out cells that point into the store; copies
    // made in the callback own their bytes and outlive the transaction.
    Statement sel = db.prepare(join);
    std::vector<std::vector<Value>> kept;
    bool sawBorrowed = false;
    sel.each([&](const RowView& r) {
        for (const Value& v : r)
            sawBorrowed = sawBorrowed || v.borrowed();
        kept.emplace_back(r.begin(), r.end());
        return true;
    });
    CHECK(sawBorrowed);
    bool coveringBorrowed = true;
    db.prepare("SELECT name FROM a WHERE name > 'name-5'").each([&](const RowView& r) {
        coveringBorrowed = coveringBorrowed && r[0].borrowed();
        return true;
    });
    CHECK(coveringBorrowed);
    db.exec("UPDATE a SET name = upper(name), tag = x'00'");
    db.exec("DELETE FROM b WHERE id % 2 = 0");
    CHECK_EQ(kept.size(), expected.size());
    for (std::size_t i = 0; i < kept.size(); ++i) {
        for (std::size_t c = 0; c < kept[i].size(); ++c) {
            CHECK(!kept[i][c].borrowed());
            CHECK(kept[i][c] == expected[i][c]);
        }
    }

    // A callback may write through the transaction it reads in, rewriting the
    // very pages the scan stands on: the cells it sees stay what they were.
    Transaction t = db.begin();
    t.exec("UPDATE a SET name = lower(name)");
    const Result before = sel.exec(t);
    std::size_t n = 0;
    sel.each(t, [&](const RowView& r) {
        t.exec("UPDATE a SET name = upper(name), tag = x'ffff' WHERE id = ?",
               {Value(r[0].integer())});
        for (std::size_t c = 0; c < r.size(); ++c)
            CHECK(r[c] == before[n][c]);
        ++n;
        return true;
    });
    CHECK_EQ(n, before.size());
    t.rollback();
}

TEST(largeSortsSpillAndMergeInOrder)
{
    tst::Scratch s("sqlite");
    // 64 KiB of sort memory: 30,000 rows make dozens of runs.
    Database db = Database::configure().sortMemoryLimit(64 << 10).open(s.file());
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, k, v TEXT, j)");
    {
        Transaction t = db.begin();
        Statement ins = db.prepare("INSERT INTO t (k, v, j) VALUES (?, ?, json_object('n', ?))");
        for (int i = 0; i < 30000; ++i) {
            const Value k = i % 7 == 0 ? Value() : (i % 3 ? Value(std::int64_t((i * 7919) % 1000)) : Value((i % 1000) + 0.5));
            ins.exec(t, {k, Value("v" + std::to_string(i % 97)), Value(i)});
        }
        t.commit();
    }
    const char* queries[] = {
        "SELECT id, k, v FROM t ORDER BY k, v DESC",
        "SELECT id, k FROM t ORDER BY k DESC NULLS FIRST, id",
        "SELECT k, count(*) FROM (SELECT k FROM t ORDER BY v) GROUP BY k",
        "SELECT id, v FROM t ORDER BY v LIMIT 20000 OFFSET 7",
        "SELECT id FROM t ORDER BY k LIMIT 15 OFFSET 30",
        "SELECT j FROM t ORDER BY k, id LIMIT 3",
    };
    for (const char* q : queries) {
        db.exec("PRAGMA temp_store = MEMORY");
        const std::string inMemory = tst::flatten(db.exec(q));
        db.exec("PRAGMA temp_store = DEFAULT");
        CHECK_EQ(tst::flatten(db.exec(q)), inMemory);
    }
    CHECK_EQ(tst::only(db.exec("PRAGMA temp_store")), std::string("0"));
    // JSON keeps its subtype through a spilled run: nested, not quoted.
    db.exec("PRAGMA temp_store = FILE");
    CHECK_EQ(tst::only(db.exec("SELECT json_group_array(j) FROM (SELECT j FROM t ORDER BY k, id LIMIT 2)")),
             tst::only(db.exec("SELECT json_group_array(json(j)) FROM (SELECT j FROM t ORDER BY k, id LIMIT 2)")));
    const std::string doc = tst::only(db.exec("SELECT json_object('n', id) AS j FROM t ORDER BY v, id FOR JSON PATH"));
    CHECK(doc.find("\"j\":{\"n\":") != std::string::npos);
    db.exec("PRAGMA temp_store = MEMORY");
    CHECK_EQ(tst::only(db.exec("SELECT json_object('n', id) AS j FROM t ORDER BY v, id FOR JSON PATH")), doc);
}

TEST(limitedSortsOfWideRowsHonorTheMemoryLimit)
{
    tst::Scratch s("sqlite");
    // 128 KiB of sort memory against ~6 MiB of wide rows, of which a large LIMIT + OFFSET must keep half.
    Database db = Database::configure().sortMemoryLimit(128 << 10).open(s.file());
    db.exec("CREATE TABLE w (id INTEGER PRIMARY KEY, k, pad TEXT, b BLOB)");
    {
        Transaction t = db.begin();
        Statement ins = db.prepare("INSERT INTO w (k, pad, b) VALUES (?, ?, zeroblob(512))");
        for (int i = 0; i < 3000; ++i) {
            const Value k = i % 11 == 0 ? Value() : Value(std::int64_t((i * 7919) % 40));  // many ties
            ins.exec(t, {k, Value(std::string(1500, char('a' + i % 26)) + std::to_string(i))});
        }
        t.commit();
    }
    const char* queries[] = {
        "SELECT id, k, length(pad) FROM w ORDER BY k LIMIT 1500 OFFSET 20",
        "SELECT id, pad FROM w ORDER BY k DESC NULLS FIRST LIMIT 1800",
        "SELECT id FROM w ORDER BY k, pad DESC LIMIT 5 OFFSET 2000",
        "SELECT id FROM w ORDER BY pad LIMIT 0",
        "SELECT count(*), sum(length(pad)) FROM (SELECT pad FROM w ORDER BY k LIMIT 2500 OFFSET 100)",
    };
    for (const char* q : queries) {
        db.exec("PRAGMA temp_store = MEMORY");  // never spills: the reference answer, ties in arrival order
        const std::string inMemory = tst::flatten(db.exec(q));
        db.exec("PRAGMA temp_store = FILE");
        CHECK_EQ(tst::flatten(db.exec(q)), inMemory);
    }
}

TEST(unprefixedNamesBindAnyPrefix)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a, b)");
    db.exec("INSERT INTO t VALUES (:a, @b)", NamedParams{{"a", Value(1)}, {"@b", Value(2)}});
    CHECK_EQ(tst::flatten(db.exec("SELECT a, b FROM t")), std::string("1|2"));
    Statement st = db.prepare("SELECT b FROM t WHERE a = $a");
    CHECK_EQ(st.parameterIndex("a"), std::size_t(1));
    CHECK_EQ(tst::only(st.bind("a", Value(1)).exec()), std::string("2"));
    Transaction t = db.begin();
    t.exec("UPDATE t SET b = :v", NamedParams{{"v", Value(5)}});
    t.commit();
    CHECK_EQ(tst::only(db.exec("SELECT b FROM t")), std::string("5"));
    CHECK_THROWS(db.exec("SELECT :a", NamedParams{{"x", Value(1)}}), ErrorCode::InvalidArgument);
}

TEST(substrOfAnEmptyBlobIsNull)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    CHECK_EQ(tst::only(db.exec("SELECT typeof(substr(x'', 1, 2))")), std::string("null"));
    CHECK_EQ(tst::only(db.exec("SELECT typeof(substr(x'00', 5, 2))")), std::string("blob"));
    CHECK_EQ(tst::only(db.exec("SELECT typeof(substr('', 1, 2))")), std::string("text"));
}

TEST(memoryProfilesAndCacheSizesAllWork)
{
    tst::Scratch s("sqlite");
    const MemoryProfile profiles[] = {MemoryProfile::Compact, MemoryProfile::Balanced, MemoryProfile::Throughput};
    int n = 0;
    for (const MemoryProfile p : profiles) {
        for (const std::size_t cache : {std::size_t(0), std::size_t(1), std::size_t(3)}) {
            const std::string name = "m" + std::to_string(n++) + ".db";
            Database db = Database::configure()
                              .memoryProfile(p)
                              .statementCacheSize(cache)
                              .arenaBlockSize(cache * 300)
                              .scratchRetain(cache * 1000)
                              .open(s.file(name.c_str()));
            db.exec("CREATE TABLE t (a INTEGER, b TEXT)");
            for (int i = 0; i < 300; ++i)
                db.exec("INSERT INTO t VALUES (?, ?)", {Value(i % 7), Value("v" + std::to_string(i))});
            // More distinct texts than the cache holds, repeated: evicted and
            // replanned without changing results.
            for (int round = 0; round < 3; ++round) {
                for (int k = 0; k < 6; ++k) {
                    const std::string q = "SELECT count(*) FROM t WHERE a = " + std::to_string(k);
                    CHECK_EQ(tst::only(db.exec(q)), std::string("43"));
                }
            }
            CHECK_EQ(tst::flatten(db.exec("SELECT a, count(DISTINCT b) FROM t GROUP BY a ORDER BY a LIMIT 2")),
                     std::string("0|43;1|43"));
        }
    }
}

TEST(wideResultsSurviveCopiesAndMoves)
{
    tst::Scratch s("sqlite");
    Database db = s.open();
    db.exec("CREATE TABLE w (a, b, c, d, e, f, g)");
    for (int i = 0; i < 500; ++i)
        db.exec("INSERT INTO w VALUES (?, ?, 'text value ' || ?, ?, ?, ?, ?)",
                {Value(i), Value(i * 2.5), Value(i), Value(std::string(40, char('a' + i % 26))), Value(i % 3),
                 Value(), Value::blob("xyz", 3)});
    Row kept;
    Result copy;
    {
        Result r = db.exec("SELECT * FROM w ORDER BY a");
        CHECK_EQ(r.size(), std::size_t(500));
        kept = r[123];                // a row copied out owns its values
        copy = r;                     // so does a copied result
        Result moved = std::move(r);  // a move keeps the cells it points into
        Result assigned = db.exec("SELECT * FROM w WHERE a < 3");
        assigned = std::move(moved);  // replacing wide rows with wide rows
        CHECK_EQ(assigned.size(), std::size_t(500));
        CHECK_EQ(assigned[499]["c"].text(), std::string("text value 499"));
        assigned = db.exec("SELECT 1");
        CHECK_EQ(assigned.size(), std::size_t(1));
    }
    CHECK_EQ(kept["a"].integer(), std::int64_t(123));
    CHECK_EQ(kept["d"].text(), std::string(40, char('a' + 123 % 26)));
    CHECK_EQ(copy[7]["c"].text(), std::string("text value 7"));
    CHECK_EQ(copy[300]["g"].blob().size(), std::size_t(3));
}

TEST(aBatchThatDetachesKeepsRunning)
{
    tst::Scratch s("sqlite");
    Database db = Database::configure().statementCacheSize(1).open(s.file());
    const std::string other = s.file("other.db").string();
    db.exec("ATTACH ? AS aux", {Value(other)});
    db.exec("CREATE TABLE aux.t (v)");
    // DETACH clears the statement cache while this batch, which is in it,
    // still has statements to run.
    CHECK_EQ(tst::only(db.exec("SELECT 1; DETACH aux; SELECT 2 + 3")), std::string("5"));
    CHECK_THROWS(db.exec("SELECT v FROM aux.t"), ErrorCode::NoSuchTable);
}

int main()
{
    return tst::runAll("sqlite");
}
