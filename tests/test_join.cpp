// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sql_test.hpp"

using namespace sql;

namespace {

Database seeded(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE dept (id INTEGER PRIMARY KEY, name TEXT)");
    db.exec("CREATE TABLE staff (id INTEGER PRIMARY KEY, name TEXT, dept_id INTEGER)");
    db.exec("INSERT INTO dept VALUES (1, 'eng'), (2, 'ops'), (3, 'empty')");
    db.exec("INSERT INTO staff VALUES (10, 'ada', 1), (11, 'grace', 1), (12, 'alan', 2),"
            " (13, 'nobody', NULL), (14, 'ghost', 99)");
    return db;
}

}  // namespace

TEST(joinsOnTheKeyOfTheInnerTable)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    Result r = db.exec("SELECT s.name, d.name FROM staff s INNER JOIN dept d ON s.dept_id = d.id"
                       " ORDER BY s.id");
    CHECK_EQ(tst::flatten(r), std::string("ada|eng;grace|eng;alan|ops"));
    // Result columns carry SQLite's names (ledger C6); the qualified name still
    // finds a column.
    CHECK_EQ(r.columns()[0], std::string("name"));
    CHECK_EQ(r.columns()[1], std::string("name"));
    CHECK_EQ(r[0]["s.name"].text(), std::string("ada"));
    CHECK_EQ(r[0]["d.name"].text(), std::string("eng"));
}

TEST(joinsInEitherDirection)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    Result r = db.exec("SELECT d.name, s.name FROM dept d JOIN staff s ON d.id = s.dept_id"
                       " ORDER BY d.id, s.id");
    CHECK_EQ(tst::flatten(r), std::string("eng|ada;eng|grace;ops|alan"));
}

TEST(joinStarsExpandPerTable)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    Result r = db.exec("SELECT * FROM staff s JOIN dept d ON s.dept_id = d.id WHERE s.id = 10");
    CHECK_EQ(r.columns().size(), std::size_t(5));
    CHECK_EQ(r.columns()[0], std::string("id"));
    CHECK_EQ(r[0]["d.id"].integer(), std::int64_t(1));
    CHECK_EQ(tst::flatten(r), std::string("10|ada|1|1|eng"));

    r = db.exec("SELECT d.* FROM staff s JOIN dept d ON s.dept_id = d.id WHERE s.id = 10");
    CHECK_EQ(r.columns().size(), std::size_t(2));
    CHECK_EQ(tst::flatten(r), std::string("1|eng"));
}

TEST(whereNarrowsAJoin)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    Result r = db.exec("SELECT s.name FROM staff s JOIN dept d ON s.dept_id = d.id"
                       " WHERE d.name = 'eng' ORDER BY s.name");
    CHECK_EQ(tst::flatten(r), std::string("ada;grace"));
}

TEST(joinsThreeTables)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    db.exec("CREATE TABLE site (dept_id INTEGER PRIMARY KEY, city TEXT)");
    db.exec("INSERT INTO site VALUES (1, 'oslo'), (2, 'lima')");

    Result r = db.exec("SELECT s.name, d.name, t.city"
                       " FROM staff s"
                       " JOIN dept d ON s.dept_id = d.id"
                       " JOIN site t ON t.dept_id = d.id"
                       " ORDER BY s.id");
    CHECK_EQ(tst::flatten(r), std::string("ada|eng|oslo;grace|eng|oslo;alan|ops|lima"));
}

TEST(joinsWorkOverAnIndexedColumn)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    db.exec("CREATE INDEX by_dept ON staff (dept_id)");
    Result r = db.exec("SELECT s.name FROM dept d JOIN staff s ON s.dept_id = d.id"
                       " WHERE d.name = 'eng' ORDER BY s.id");
    CHECK_EQ(tst::flatten(r), std::string("ada;grace"));
}

TEST(crossJoinPairsEverything)
{
    tst::Scratch s("join");
    Database db = s.open();
    db.exec("CREATE TABLE a (v INTEGER)");
    db.exec("CREATE TABLE b (v INTEGER)");
    db.exec("INSERT INTO a VALUES (1), (2)");
    db.exec("INSERT INTO b VALUES (10), (20), (30)");

    Result r = db.exec("SELECT a.v, b.v FROM a CROSS JOIN b ORDER BY a.v, b.v");
    CHECK_EQ(r.size(), std::size_t(6));
    CHECK_EQ(tst::flatten(r), std::string("1|10;1|20;1|30;2|10;2|20;2|30"));
}

TEST(joinDiagnosesBadNames)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    CHECK_THROWS(db.exec("SELECT name FROM staff s JOIN dept d ON s.dept_id = d.id"),
                 ErrorCode::AmbiguousColumn);
    // Two sources under one alias make every use of it ambiguous, as in SQLite.
    CHECK_THROWS(db.exec("SELECT s.name FROM staff s JOIN dept s ON 1 = 1"),
                 ErrorCode::AmbiguousColumn);
    CHECK_THROWS(db.exec("SELECT s.name FROM staff s JOIN missing d ON 1 = 1"),
                 ErrorCode::NoSuchTable);
}

TEST(joinResultsAreSortableAndPageable)
{
    tst::Scratch s("join");
    Database db = seeded(s);
    Result r = db.exec("SELECT s.name FROM staff s JOIN dept d ON s.dept_id = d.id"
                       " ORDER BY d.name DESC, s.name LIMIT 2");
    CHECK_EQ(tst::flatten(r), std::string("alan;ada"));
}

namespace {

/// Two tables whose join keys repeat, are missing, are NULL and come in
/// every storage class, so a hash join has each kind of bucket to get wrong.
Database mixedKeys(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE l (id INTEGER PRIMARY KEY, k, tag TEXT)");
    db.exec("CREATE TABLE r (id INTEGER PRIMARY KEY, k, n INTEGER, name TEXT COLLATE NOCASE)");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 400) "
            "INSERT INTO l SELECT i, CASE WHEN i % 11 = 0 THEN NULL WHEN i % 7 = 0 THEN 'k' || (i % 13) "
            "WHEN i % 5 = 0 THEN (i % 17) + 0.0 ELSE i % 17 END, 'T' || (i % 9) FROM c");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 300) "
            "INSERT INTO r SELECT i, CASE WHEN i % 10 = 0 THEN NULL WHEN i % 6 = 0 THEN 'k' || (i % 13) "
            "WHEN i % 4 = 0 THEN (i % 23) + 0.0 ELSE i % 23 END, i % 5, 't' || (i % 12) FROM c");
    return db;
}

/// `hashed` and `looped` are the same join; the second states its join term
/// so that no hash key can be taken from it, and so runs as a plain loop.
void sameRows(Database& db, const std::string& hashed, const std::string& looped)
{
    const std::string a = tst::flatten(db.exec(hashed));
    const std::string b = tst::flatten(db.exec(looped));
    CHECK(a == b);
    CHECK(!a.empty());
}

}  // namespace

TEST(a_hash_join_returns_the_rows_of_the_loop_it_replaces)
{
    tst::Scratch s("join");
    Database db = mixedKeys(s);
    // an inner join of two tables without an index on either key
    sameRows(db, "SELECT l.id, r.id FROM l JOIN r ON r.k = l.k ORDER BY 1, 2",
             "SELECT l.id, r.id FROM l JOIN r ON (r.k = l.k) = 1 ORDER BY 1, 2");
    // with a term of the inner table alone, and one that needs both rows
    sameRows(db, "SELECT l.id, r.id FROM l JOIN r ON r.k = l.k AND r.n > 1 AND r.id + l.id > 50 ORDER BY 1, 2",
             "SELECT l.id, r.id FROM l JOIN r ON (r.k = l.k) = 1 AND r.n > 1 AND r.id + l.id > 50 ORDER BY 1, 2");
    // a LEFT join keeps the rows that match nothing, NULL keys among them
    sameRows(db, "SELECT l.id, r.id FROM l LEFT JOIN r ON r.k = l.k ORDER BY 1, 2",
             "SELECT l.id, r.id FROM l LEFT JOIN r ON (r.k = l.k) = 1 ORDER BY 1, 2");
    // ... and its WHERE runs on the joined row: the anti-join
    sameRows(db, "SELECT l.id FROM l LEFT JOIN r ON r.k = l.k AND r.n = 2 WHERE r.id IS NULL ORDER BY 1",
             "SELECT l.id FROM l LEFT JOIN r ON (r.k = l.k) = 1 AND r.n = 2 WHERE r.id IS NULL ORDER BY 1");
    sameRows(db, "SELECT l.id, r.id FROM l LEFT JOIN r ON r.k = l.k WHERE r.n = 3 OR r.n IS NULL ORDER BY 1, 2",
             "SELECT l.id, r.id FROM l LEFT JOIN r ON (r.k = l.k) = 1 WHERE r.n = 3 OR r.n IS NULL ORDER BY 1, 2");
    // a collation decides which texts are equal
    sameRows(db, "SELECT l.id, r.id FROM l JOIN r ON r.name = l.tag ORDER BY 1, 2",
             "SELECT l.id, r.id FROM l JOIN r ON (r.name = l.tag) = 1 ORDER BY 1, 2");
    // a key computed on either side
    sameRows(db, "SELECT l.id, r.id FROM l JOIN r ON r.n + 1 = l.id % 6 ORDER BY 1, 2",
             "SELECT l.id, r.id FROM l JOIN r ON (r.n + 1 = l.id % 6) = 1 ORDER BY 1, 2");
    // three tables: the inner join after a LEFT join keeps its ON terms
    sameRows(db,
             "SELECT a.id, b.id, c.id FROM l a LEFT JOIN r b ON b.k = a.k AND b.n = 0 JOIN r c ON c.n = a.id % 5 "
             "AND c.id < 20 ORDER BY 1, 2, 3",
             "SELECT a.id, b.id, c.id FROM l a LEFT JOIN r b ON (b.k = a.k) = 1 AND b.n = 0 JOIN r c ON "
             "(c.n = a.id % 5) = 1 AND c.id < 20 ORDER BY 1, 2, 3");
}

TEST(a_hash_join_serves_sub_queries_views_and_ctes)
{
    tst::Scratch s("join");
    Database db = mixedKeys(s);
    db.exec("CREATE VIEW rv AS SELECT k, count(*) AS c, max(id) AS top FROM r GROUP BY k");
    sameRows(db, "SELECT l.id, t.id FROM l JOIN (SELECT id, k FROM r WHERE n <> 4) t ON t.k = l.k ORDER BY 1, 2",
             "SELECT l.id, t.id FROM l JOIN (SELECT id, k FROM r WHERE n <> 4) t ON (t.k = l.k) = 1 ORDER BY 1, 2");
    sameRows(db, "SELECT l.id, rv.c, rv.top FROM l LEFT JOIN rv ON rv.k = l.k ORDER BY 1, 2, 3",
             "SELECT l.id, rv.c, rv.top FROM l LEFT JOIN rv ON (rv.k = l.k) = 1 ORDER BY 1, 2, 3");
    sameRows(db,
             "WITH g AS (SELECT n, sum(id) AS total FROM r GROUP BY n) "
             "SELECT l.id, g.total FROM l JOIN g ON g.n = l.id % 7 ORDER BY 1, 2",
             "WITH g AS (SELECT n, sum(id) AS total FROM r GROUP BY n) "
             "SELECT l.id, g.total FROM l JOIN g ON (g.n = l.id % 7) = 1 ORDER BY 1, 2");
    // a CTE read twice, each reference with a table of its own
    sameRows(db,
             "WITH g AS (SELECT k, min(id) AS first FROM r GROUP BY k) "
             "SELECT l.id, a.first, b.first FROM l JOIN g a ON a.k = l.k LEFT JOIN g b ON b.first = l.id ORDER BY 1, 2, 3",
             "WITH g AS (SELECT k, min(id) AS first FROM r GROUP BY k) "
             "SELECT l.id, a.first, b.first FROM l JOIN g a ON (a.k = l.k) = 1 LEFT JOIN g b ON (b.first = l.id) = 1 "
             "ORDER BY 1, 2, 3");
    // inside a correlated sub-query the rows are made anew for each outer row
    sameRows(db,
             "SELECT l.id, (SELECT count(*) FROM r x JOIN (SELECT k FROM r WHERE id < l.id) y ON y.k = x.k) FROM l "
             "WHERE l.id < 40 ORDER BY 1",
             "SELECT l.id, (SELECT count(*) FROM r x JOIN (SELECT k FROM r WHERE id < l.id) y ON (y.k = x.k) = 1) FROM l "
             "WHERE l.id < 40 ORDER BY 1");
}

TEST(a_hash_join_takes_a_table_too_large_to_replay)
{
    tst::Scratch s("join");
    Database db = s.open();
    db.exec("CREATE TABLE big (id INTEGER PRIMARY KEY, k INTEGER, v INTEGER)");
    db.exec("CREATE TABLE probe (id INTEGER PRIMARY KEY, k INTEGER)");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 90000) "
            "INSERT INTO big SELECT i, i % 30000, i FROM c");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 3000) "
            "INSERT INTO probe SELECT i, i * 7 FROM c");
    // 3000 probes of 90000 rows: a loop would read 270 million rows
    Statement st = db.prepare("SELECT count(*), sum(b.v) FROM probe p JOIN big b ON b.k = p.k");
    const std::string once = tst::flatten(st.exec());
    CHECK_EQ(once, std::string("9000|364531500"));
    // the statement keeps no stale table: the rows change and so does the answer
    db.exec("DELETE FROM big WHERE id > 60000");
    CHECK_EQ(tst::flatten(st.exec()), std::string("6000|153021000"));
    CHECK_EQ(tst::flatten(db.exec("SELECT count(*) FROM probe p LEFT JOIN big b ON b.k = p.k WHERE b.id IS NULL")),
             std::string("0"));
    CHECK_EQ(tst::flatten(db.exec("SELECT count(*) FROM probe p LEFT JOIN big b ON b.k = p.k + 30000 WHERE b.id IS NULL")),
             std::string("3000"));
}

int main()
{
    return tst::runAll("join");
}

TEST(right_and_full_join_keep_unmatched_right_rows_whatever_the_on_clause_pins)
{
    tst::Scratch s("join");
    sql::Database db = s.open();
    db.exec("CREATE TABLE a(id INTEGER PRIMARY KEY, x, y)");
    db.exec("CREATE TABLE b(id INTEGER PRIMARY KEY, x, z)");
    db.exec("INSERT INTO a VALUES (1,1,'a1'),(2,2,'a2'),(3,3,'a3')");
    db.exec("INSERT INTO b VALUES (1,2,'b1'),(2,2,'b2'),(3,9,'b3'),(4,3,'b4')");
    CHECK_EQ(tst::flatten(db.exec("SELECT a.id, b.id FROM a RIGHT JOIN b ON a.id = b.id ORDER BY 1, 2")),
             std::string("NULL|4;1|1;2|2;3|3"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a.id, b.id FROM a FULL JOIN b ON a.id = b.id ORDER BY 1, 2")),
             std::string("NULL|4;1|1;2|2;3|3"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a.id, b.id FROM a RIGHT JOIN b USING (id) ORDER BY 1, 2")),
             std::string("NULL|4;1|1;2|2;3|3"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a.id, b.id FROM a RIGHT JOIN b ON a.x = b.x AND b.id < 2 ORDER BY 1, 2")),
             std::string("NULL|2;NULL|3;NULL|4;2|1"));
}
