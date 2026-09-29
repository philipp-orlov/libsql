// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Composite primary keys and table-level UNIQUE constraints.
#include "sql_test.hpp"

#include <string>

using namespace sql;

namespace {

/// The same rows twice: `keyed` under a three-column key of three different
/// types, `plain` under an implicit rowid, so that every query through the key
/// has a scan-only answer to be checked against. Twelve (region, day) pairs,
/// each with twenty sequence numbers.
Database twins(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE keyed (region TEXT, day DATE, seq INTEGER, amount REAL,"
            " PRIMARY KEY (region, day, seq))");
    db.exec("CREATE TABLE plain (region TEXT, day DATE, seq INTEGER, amount REAL)");

    Transaction t = db.begin();
    const char* regions[] = {"east", "north", "south", "west"};
    for (int i = 0; i < 240; ++i) {
        const Params row{Value(regions[i % 4]),
                         Value("2024-01-" + std::string(i % 12 < 9 ? "0" : "") +
                               std::to_string(1 + i % 12)),
                         Value(i / 12), Value(double(i) / 8.0)};
        t.exec("INSERT INTO keyed VALUES (?, ?, ?, ?)", row);
        t.exec("INSERT INTO plain VALUES (?, ?, ?, ?)", row);
    }
    t.commit();
    return db;
}

void agree(Database& db, const std::string& tail)
{
    const std::string select = "SELECT region, day, seq, amount FROM ";
    const std::string keyed = tst::flatten(db.exec(select + "keyed WHERE " + tail));
    const std::string plain = tst::flatten(db.exec(select + "plain WHERE " + tail));
    if (keyed != plain)
        throw std::runtime_error("keyed and scanned answers differ for: " + tail +
                                 "\n  keyed = " + keyed + "\n  plain = " + plain);
    if (keyed.empty())
        throw std::runtime_error("query matched nothing, so it proves nothing: " + tail);
}

}  // namespace

TEST(declaresAndReportsACompositeKey)
{
    tst::Scratch s("composite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a INTEGER, b TEXT, c REAL, PRIMARY KEY (b, a))");

    const TableInfo info = db.table("t");
    CHECK_EQ(info.primaryKey, (std::vector<int>{1, 0}));
    CHECK(info.columns[0].primaryKey);
    CHECK(info.columns[0].notNull);
    CHECK(info.columns[1].primaryKey);
    CHECK(info.columns[1].notNull);
    CHECK(!info.columns[2].primaryKey);
    // The key stands in for the rowid, which the table therefore lacks.
    CHECK_THROWS(db.exec("SELECT rowid FROM t"), ErrorCode::NoSuchColumn);
    // No index is needed to enforce the key; the rows themselves do.
    CHECK(db.indexes("t").empty());
}

TEST(compositeKeySurvivesReopening)
{
    tst::Scratch s("composite");
    {
        Database db = s.open();
        db.exec("CREATE TABLE t (a INTEGER, b TEXT, c REAL, PRIMARY KEY (b, a))");
        db.exec("INSERT INTO t VALUES (1, 'x', 0.5), (2, 'x', 1.5)");
    }
    Database db = s.open();
    CHECK_EQ(db.table("t").primaryKey, (std::vector<int>{1, 0}));
    CHECK_EQ(tst::only(db.exec("SELECT c FROM t WHERE b = 'x' AND a = 2")), std::string("1.5"));
    CHECK_THROWS(db.exec("INSERT INTO t VALUES (2, 'x', 9)"), ErrorCode::ConstraintViolation);
}

TEST(compositeKeysMustBeWholeAndUnique)
{
    tst::Scratch s("composite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a INTEGER, b INTEGER, note TEXT, PRIMARY KEY (a, b))");
    db.exec("INSERT INTO t VALUES (1, 1, 'p'), (1, 2, 'q'), (2, 1, 'r')");

    // Only the whole tuple has to be unique.
    CHECK_THROWS(db.exec("INSERT INTO t VALUES (1, 1, 'again')"), ErrorCode::ConstraintViolation);
    CHECK_EQ(db.exec("SELECT note FROM t").size(), std::size_t(3));

    // No part of a key may be NULL, and nothing is handed out for a missing part.
    CHECK_THROWS(db.exec("INSERT INTO t VALUES (3, NULL, 'x')"), ErrorCode::ConstraintViolation);
    CHECK_THROWS(db.exec("INSERT INTO t (a, note) VALUES (3, 'x')"),
                 ErrorCode::ConstraintViolation);
    CHECK_EQ(db.exec("INSERT INTO t VALUES (3, 3, 'x')").lastInsertId(), std::int64_t(0));

    // Key columns are typed like any other.
    CHECK_THROWS(db.exec("INSERT INTO t VALUES ('one', 1, 'x')"), ErrorCode::TypeMismatch);
    // And a key value spelled as text is read as the column's type.
    db.exec("INSERT INTO t VALUES ('4', '4', 'y')");
    CHECK_EQ(tst::only(db.exec("SELECT note FROM t WHERE a = 4 AND b = 4")), std::string("y"));
}

TEST(lookupsThroughTheKeyMatchScans)
{
    tst::Scratch s("composite");
    Database db = twins(s);
    // The whole key: a point lookup.
    agree(db, "region = 'east' AND day = '2024-01-05' AND seq = 4 ORDER BY seq");
    // Operands in either order, and the columns in any order.
    agree(db, "3 = seq AND '2024-01-04' = day AND 'west' = region ORDER BY seq");
    // A leading run of the key: a window over the rows.
    agree(db, "region = 'north' ORDER BY day, seq");
    agree(db, "region = 'north' AND day = '2024-01-02' ORDER BY seq");
    // ... plus a range on the column after it.
    agree(db, "region = 'south' AND day > '2024-01-06' ORDER BY day, seq");
    agree(db, "region = 'south' AND day >= '2024-01-06' AND day < '2024-01-09' ORDER BY day, seq");
    agree(db, "region = 'south' AND day = '2024-01-03' AND seq BETWEEN 1 AND 3 ORDER BY seq");
    agree(db, "region = 'south' AND day = '2024-01-03' AND seq <= 2 ORDER BY seq");
    // A range on the first key column alone.
    agree(db, "region > 'south' ORDER BY day, seq");
    agree(db, "region < 'north' ORDER BY day, seq");
    agree(db, "region BETWEEN 'east' AND 'north' ORDER BY region, day, seq");
    // Trailing columns without the leading ones cannot use the key at all.
    agree(db, "seq = 2 ORDER BY region, day");
    agree(db, "day = '2024-01-07' AND seq = 1 ORDER BY region");
    // Extra predicates are still applied to what the key produces.
    agree(db, "region = 'east' AND amount > 20.0 ORDER BY day, seq");
    agree(db, "region = 'east' AND seq = 0 ORDER BY day");
    agree(db, "region = 'east' OR seq = 4 ORDER BY region, day, seq");
}

TEST(lookupsThroughTheKeyWidenRatherThanGuess)
{
    tst::Scratch s("composite");
    Database db = twins(s);
    CHECK(db.exec("SELECT seq FROM keyed WHERE region = NULL AND day = '2024-01-01'").empty());
    CHECK(db.exec("SELECT seq FROM keyed WHERE region = 'east' AND day = NULL").empty());
    CHECK(db.exec("SELECT seq FROM keyed WHERE region = 'east' AND day = 'not a date'").empty());
    CHECK(db.exec("SELECT seq FROM keyed WHERE region = 'east' AND day = '2024-01-01' AND seq = 'x'")
              .empty());
    CHECK_EQ(db.exec("SELECT seq FROM keyed WHERE region = 'east' AND seq < 'x'").size(),
             db.exec("SELECT seq FROM plain WHERE region = 'east' AND seq < 'x'").size());
}

TEST(editsThroughACompositeKey)
{
    tst::Scratch s("composite");
    Database db = twins(s);
    for (const char* t : {"keyed", "plain"}) {
        const std::string table = t;
        db.exec("UPDATE " + table + " SET amount = amount * 2 WHERE region = 'east' AND seq = 0");
        db.exec("UPDATE " + table + " SET seq = seq + 100 WHERE region = 'west' AND day = '2024-01-04'");
        db.exec("UPDATE " + table + " SET region = 'far' WHERE region = 'south' AND day > '2024-01-09'");
        db.exec("DELETE FROM " + table + " WHERE region = 'north' AND day = '2024-01-02'");
        db.exec("DELETE FROM " + table + " WHERE region = 'east' AND seq = 3");
        db.exec("INSERT INTO " + table + " VALUES ('east', '2024-02-01', 0, 1.0)");
    }
    agree(db, "region = 'east' ORDER BY day, seq");
    agree(db, "region = 'west' AND day = '2024-01-04' ORDER BY seq");
    agree(db, "region = 'far' ORDER BY day, seq");
    agree(db, "region = 'north' ORDER BY day, seq");
    agree(db, "seq >= 100 ORDER BY region, day, seq");
    CHECK_EQ(db.exec("SELECT seq FROM keyed").size(), db.exec("SELECT seq FROM plain").size());

    // Moving a row onto another's key is a collision, and a statement that
    // collides leaves nothing behind.
    CHECK_THROWS(db.exec("UPDATE keyed SET seq = 1 WHERE region = 'east' AND day = '2024-01-01' AND seq = 0"),
                 ErrorCode::ConstraintViolation);
    agree(db, "region = 'east' AND day = '2024-01-01' ORDER BY seq");
    // Updating a key column to what it already is collides with nothing.
    db.exec("UPDATE keyed SET region = 'east' WHERE region = 'east'");
    agree(db, "region = 'east' ORDER BY day, seq");
}

TEST(secondaryIndexesOnACompositeKeyedTable)
{
    tst::Scratch s("composite");
    Database db = twins(s);
    db.exec("CREATE INDEX keyed_amount ON keyed (amount)");
    db.exec("CREATE INDEX keyed_seq_day ON keyed (seq, day)");
    agree(db, "amount = 3.5 ORDER BY region");
    agree(db, "amount > 28.0 ORDER BY region, day, seq");
    agree(db, "seq = 2 AND day = '2024-01-03' ORDER BY region");
    agree(db, "seq = 2 AND day >= '2024-01-11' ORDER BY region, day");

    // Index entries follow the row through key changes and deletions.
    for (const char* t : {"keyed", "plain"}) {
        const std::string table = t;
        db.exec("UPDATE " + table + " SET seq = seq + 100, amount = 999.0 + seq"
                " WHERE region = 'east' AND day = '2024-01-01' AND seq < 3");
        db.exec("DELETE FROM " + table + " WHERE amount BETWEEN 10.0 AND 12.0");
    }
    agree(db, "amount = 1000.0 ORDER BY seq");
    agree(db, "amount > 999.0 ORDER BY seq");
    agree(db, "seq >= 100 ORDER BY region, day, seq");
    agree(db, "seq = 101 AND day = '2024-01-01' ORDER BY region");
    agree(db, "amount > 9.0 AND amount < 13.0 ORDER BY region, day, seq");

    // A unique index sits happily beside the key.
    db.exec("CREATE UNIQUE INDEX keyed_one_amount ON keyed (amount)");
    CHECK_THROWS(db.exec("INSERT INTO keyed VALUES ('zz', '2024-03-01', 0, 3.5)"),
                 ErrorCode::ConstraintViolation);
    db.exec("INSERT INTO keyed VALUES ('zz', '2024-03-01', 0, 3.51)");
}

TEST(joinsThroughACompositeKey)
{
    tst::Scratch s("composite");
    Database db = s.open();
    db.exec("CREATE TABLE price (sku TEXT, region TEXT, cents INTEGER, PRIMARY KEY (sku, region))");
    db.exec("CREATE TABLE sale (id INTEGER PRIMARY KEY, sku TEXT, region TEXT, qty INTEGER)");
    db.exec("INSERT INTO price VALUES ('a', 'eu', 100), ('a', 'us', 120), ('b', 'eu', 300)");
    db.exec("INSERT INTO sale VALUES (1, 'a', 'eu', 2), (2, 'a', 'us', 1), (3, 'b', 'eu', 1),"
            " (4, 'b', 'us', 5)");

    // The inner table is reached by its whole key ...
    Result r = db.exec("SELECT s.id, p.cents * s.qty FROM sale s"
                       " INNER JOIN price p ON p.sku = s.sku AND p.region = s.region"
                       " ORDER BY s.id");
    CHECK_EQ(tst::flatten(r), std::string("1|200;2|120;3|300"));
    // ... or by a leading part of it.
    r = db.exec("SELECT s.id, p.region, p.cents FROM sale s"
                " INNER JOIN price p ON p.sku = s.sku WHERE s.id = 4 ORDER BY p.region");
    CHECK_EQ(tst::flatten(r), std::string("4|eu|300"));
    // Joining from the keyed side works the same.
    r = db.exec("SELECT p.sku, p.region, s.qty FROM price p"
                " INNER JOIN sale s ON s.sku = p.sku AND s.region = p.region"
                " ORDER BY p.sku, p.region");
    CHECK_EQ(tst::flatten(r), std::string("a|eu|2;a|us|1;b|eu|1"));
}

TEST(groupsAndOrdersOverACompositeKey)
{
    tst::Scratch s("composite");
    Database db = twins(s);
    Result r = db.exec("SELECT region, COUNT(*), SUM(seq) FROM keyed"
                       " WHERE region >= 'north' GROUP BY region ORDER BY region");
    CHECK_EQ(tst::flatten(r), std::string("north|60|570;south|60|570;west|60|570"));
    r = db.exec("SELECT day, seq FROM keyed WHERE region = 'east' ORDER BY day DESC, seq DESC LIMIT 2");
    CHECK_EQ(tst::flatten(r), std::string("2024-01-09 00:00:00|19;2024-01-09 00:00:00|18"));
}

TEST(preparedStatementsBindCompositeKeys)
{
    tst::Scratch s("composite");
    Database db = twins(s);
    Statement byKey = db.prepare(
        "SELECT amount FROM keyed WHERE region = ? AND day = ? AND seq = ?");
    CHECK_EQ(tst::only(byKey.exec({Value("east"), Value("2024-01-01"), Value(0)})),
             tst::only(db.exec("SELECT amount FROM plain WHERE region = 'east'"
                               " AND day = '2024-01-01' AND seq = 0")));
    CHECK(byKey.exec({Value("east"), Value("2024-01-01"), Value(99)}).empty());
    CHECK(byKey.exec({Value("east"), Value(), Value(0)}).empty());

    Statement insert = db.prepare("INSERT INTO keyed VALUES (?, ?, ?, ?)");
    CHECK_THROWS(insert.exec({Value("east"), Value("2024-01-01"), Value(0), Value(0.25)}),
                 ErrorCode::ConstraintViolation);
    insert.exec({Value("east"), Value("2024-01-01"), Value(99), Value(0.25)});
    CHECK_EQ(tst::only(byKey.exec({Value("east"), Value("2024-01-01"), Value(99)})),
             std::string("0.25"));
}

TEST(rejectsMalformedCompositeKeys)
{
    tst::Scratch s("composite");
    Database db = s.open();
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER, b INTEGER, PRIMARY KEY (a, c))"),
                 ErrorCode::NoSuchColumn);
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER, b INTEGER, PRIMARY KEY (a, a))"),
                 ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER PRIMARY KEY, b INTEGER, PRIMARY KEY (a, b))"),
                 ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER, b INTEGER, PRIMARY KEY (a), PRIMARY KEY (b))"),
                 ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER, b INTEGER, PRIMARY KEY ())"),
                 ErrorCode::SyntaxError);
    CHECK(db.tables().empty());
}

TEST(tableLevelUniqueConstraints)
{
    tst::Scratch s("composite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, c INTEGER,"
            " UNIQUE (a, b), UNIQUE (c))");

    // Each constraint is kept by a unique index the schema reports.
    std::vector<IndexInfo> all = db.indexes("t");
    CHECK_EQ(all.size(), std::size_t(2));
    CHECK(all[0].unique && all[1].unique);
    CHECK_EQ(all[0].columns, (std::vector<std::string>{"a", "b"}));
    CHECK_EQ(all[1].columns, (std::vector<std::string>{"c"}));

    db.exec("INSERT INTO t (a, b, c) VALUES (1, 'x', 1), (1, 'y', 2), (2, 'x', 3)");
    CHECK_THROWS(db.exec("INSERT INTO t (a, b, c) VALUES (1, 'x', 4)"),
                 ErrorCode::ConstraintViolation);
    CHECK_THROWS(db.exec("INSERT INTO t (a, b, c) VALUES (9, 'z', 3)"),
                 ErrorCode::ConstraintViolation);
    CHECK_THROWS(db.exec("UPDATE t SET b = 'x' WHERE a = 1 AND b = 'y'"),
                 ErrorCode::ConstraintViolation);
    // NULL never collides.
    db.exec("INSERT INTO t (a, b) VALUES (1, NULL), (1, NULL)");
    CHECK_EQ(db.exec("SELECT id FROM t").size(), std::size_t(5));
    // The unique pair is also an index the planner can use.
    CHECK_EQ(tst::only(db.exec("SELECT id FROM t WHERE a = 2 AND b = 'x'")), std::string("3"));
}

TEST(uniqueConstraintsTheKeyAlreadyGivesAreDropped)
{
    tst::Scratch s("composite");
    Database db = s.open();
    db.exec("CREATE TABLE t (a INTEGER, b INTEGER, c TEXT, PRIMARY KEY (a, b),"
            " UNIQUE (b, a), UNIQUE (a, b), UNIQUE (c), UNIQUE (c))");
    std::vector<IndexInfo> all = db.indexes("t");
    CHECK_EQ(all.size(), std::size_t(1));
    CHECK_EQ(all[0].columns, (std::vector<std::string>{"c"}));

    // A column-level UNIQUE on part of a composite key still narrows it.
    db.exec("CREATE TABLE u (a INTEGER UNIQUE, b INTEGER, PRIMARY KEY (a, b))");
    CHECK_EQ(db.indexes("u").size(), std::size_t(1));
    db.exec("INSERT INTO u VALUES (1, 1)");
    CHECK_THROWS(db.exec("INSERT INTO u VALUES (1, 2)"), ErrorCode::ConstraintViolation);

    CHECK_THROWS(db.exec("CREATE TABLE v (a INTEGER, UNIQUE (nope))"), ErrorCode::NoSuchColumn);
    CHECK_THROWS(db.exec("CREATE TABLE v (a INTEGER, UNIQUE (a, a))"), ErrorCode::InvalidArgument);
}

TEST(singleColumnKeysAreUnchanged)
{
    tst::Scratch s("composite");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER, name TEXT, PRIMARY KEY (id))");
    CHECK_EQ(db.table("t").primaryKey, std::vector<int>{0});
    Result r = db.exec("INSERT INTO t (name) VALUES ('auto')");
    CHECK_EQ(r.lastInsertId(), std::int64_t(1));
    db.exec("INSERT INTO t VALUES (10, 'ten')");
    CHECK_EQ(db.exec("INSERT INTO t (name) VALUES ('next')").lastInsertId(), std::int64_t(11));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t WHERE id > 5 ORDER BY id")),
             std::string("10;11"));
}

int main()
{
    return tst::runAll("composite");
}
