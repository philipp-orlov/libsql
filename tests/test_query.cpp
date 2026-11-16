// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sql_test.hpp"

using namespace sql;

namespace {

Database seeded(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE staff (id INTEGER PRIMARY KEY, name TEXT, dept TEXT, pay INTEGER)");
    db.exec("INSERT INTO staff VALUES"
            " (1, 'ada',    'eng',   100),"
            " (2, 'grace',  'eng',   120),"
            " (3, 'edsger', 'ops',    90),"
            " (4, 'barbara','ops',   130),"
            " (5, 'alan',   NULL,   NULL)");
    return db;
}

}  // namespace

TEST(selectsColumnsAndStars)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    Result r = db.exec("SELECT * FROM staff WHERE id = 1");
    CHECK_EQ(r.columns().size(), std::size_t(4));
    CHECK_EQ(r.columns()[2], std::string("dept"));
    CHECK_EQ(tst::flatten(r), std::string("1|ada|eng|100"));

    r = db.exec("SELECT name, pay FROM staff WHERE id = 2");
    CHECK_EQ(tst::flatten(r), std::string("grace|120"));
}

TEST(namesResultColumns)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    Result r = db.exec("SELECT pay * 2 AS doubled, name FROM staff WHERE id = 1");
    CHECK_EQ(r.columns()[0], std::string("doubled"));
    CHECK_EQ(r[0]["doubled"].integer(), std::int64_t(200));

    r = db.exec("SELECT pay + 1 FROM staff WHERE id = 1");
    CHECK_EQ(r.columns()[0], std::string("pay + 1"));
}

TEST(filtersWithTheUsualOperators)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    auto ids = [&](const char* where) {
        return tst::flatten(db.exec(std::string("SELECT id FROM staff WHERE ") + where +
                                    " ORDER BY id"));
    };
    CHECK_EQ(ids("pay > 100"), std::string("2;4"));
    CHECK_EQ(ids("pay >= 100 AND dept = 'eng'"), std::string("1;2"));
    CHECK_EQ(ids("dept = 'ops' OR pay = 100"), std::string("1;3;4"));
    CHECK_EQ(ids("pay BETWEEN 95 AND 125"), std::string("1;2"));
    CHECK_EQ(ids("pay NOT BETWEEN 95 AND 125"), std::string("3;4"));
    CHECK_EQ(ids("dept IN ('eng', 'hr')"), std::string("1;2"));
    CHECK_EQ(ids("dept NOT IN ('eng')"), std::string("3;4"));
    CHECK_EQ(ids("name LIKE 'a%'"), std::string("1;5"));
    CHECK_EQ(ids("name LIKE '_da'"), std::string("1"));
    CHECK_EQ(ids("NOT (dept = 'eng')"), std::string("3;4"));
    CHECK_EQ(ids("id <> 1 AND id != 2 AND id < 5"), std::string("3;4"));
}

TEST(nullsCompareToNothing)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    auto ids = [&](const char* where) {
        return tst::flatten(db.exec(std::string("SELECT id FROM staff WHERE ") + where +
                                    " ORDER BY id"));
    };
    CHECK_EQ(ids("dept IS NULL"), std::string("5"));
    CHECK_EQ(ids("dept IS NOT NULL"), std::string("1;2;3;4"));
    CHECK_EQ(ids("dept = NULL"), std::string(""));
    CHECK_EQ(ids("dept <> 'eng'"), std::string("3;4"));  // the NULL row drops out
    CHECK_EQ(ids("pay IS NULL OR pay > 125"), std::string("4;5"));
    CHECK_EQ(tst::flatten(db.exec("SELECT pay + 1 FROM staff WHERE id = 5")),
             std::string("NULL"));
}

TEST(ordersAndPages)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    CHECK_EQ(tst::flatten(db.exec("SELECT name FROM staff ORDER BY name")),
             std::string("ada;alan;barbara;edsger;grace"));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM staff ORDER BY pay DESC")),
             std::string("4;2;1;3;5"));  // NULLs sort last under DESC
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM staff ORDER BY dept, pay DESC")),
             std::string("5;2;1;4;3"));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM staff ORDER BY id LIMIT 2")),
             std::string("1;2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM staff ORDER BY id LIMIT 2 OFFSET 3")),
             std::string("4;5"));
    CHECK_EQ(db.exec("SELECT id FROM staff LIMIT 2").size(), std::size_t(2));
}

TEST(evaluatesArithmeticAndConcatenation)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    CHECK_EQ(tst::only(db.exec("SELECT 6 * 7 FROM staff WHERE id = 1")), std::string("42"));
    CHECK_EQ(tst::only(db.exec("SELECT 7 / 2 FROM staff WHERE id = 1")), std::string("3"));
    CHECK_EQ(tst::only(db.exec("SELECT 7.0 / 2 FROM staff WHERE id = 1")), std::string("3.5"));
    CHECK_EQ(tst::only(db.exec("SELECT 7 % 3 FROM staff WHERE id = 1")), std::string("1"));
    CHECK_EQ(tst::only(db.exec("SELECT -pay FROM staff WHERE id = 1")), std::string("-100"));
    CHECK_EQ(tst::only(db.exec("SELECT name || '@' || dept FROM staff WHERE id = 1")),
             std::string("ada@eng"));
    CHECK(db.exec("SELECT 1 / 0 FROM staff WHERE id = 1")[0][0].isNull());
}

TEST(qualifiesColumnsByTableAndAlias)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    CHECK_EQ(tst::only(db.exec("SELECT staff.name FROM staff WHERE staff.id = 1")),
             std::string("ada"));
    CHECK_EQ(tst::only(db.exec("SELECT s.name FROM staff s WHERE s.id = 2")),
             std::string("grace"));
    CHECK_EQ(tst::only(db.exec("SELECT s.name FROM staff AS s WHERE s.id = 2")),
             std::string("grace"));
    CHECK_THROWS(db.exec("SELECT nope FROM staff"), ErrorCode::NoSuchColumn);
    CHECK_THROWS(db.exec("SELECT x.name FROM staff s"), ErrorCode::NoSuchColumn);
}

TEST(reportsSyntaxProblems)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    CHECK_THROWS(db.exec("SELECT FROM"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT * FROM staff WHERE"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT * FROM staff WHERE id = "), ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT * FROM staff staff2 extra"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("HELLO"), ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT no_such_function(name) FROM staff"), ErrorCode::Unsupported);
    CHECK_THROWS(db.exec("SELECT lower(name, 1) FROM staff"), ErrorCode::InvalidArgument);
}

TEST(quotedIdentifiersEscapeKeywords)
{
    tst::Scratch s("query");
    Database db = s.open();
    db.exec("CREATE TABLE \"order\" (id INTEGER PRIMARY KEY, \"select\" TEXT)");
    db.exec("INSERT INTO \"order\" (\"select\") VALUES ('ok')");
    CHECK_EQ(tst::only(db.exec("SELECT \"select\" FROM \"order\"")), std::string("ok"));
}

TEST(commentsAreIgnored)
{
    tst::Scratch s("query");
    Database db = seeded(s);
    Result r = db.exec("-- a leading remark\n"
                       "SELECT name /* inline */ FROM staff WHERE id = 1 -- trailing\n");
    CHECK_EQ(tst::only(r), std::string("ada"));
}

TEST(numericFiltersTestStoredRowsLikeDecodedOnes)
{
    tst::Scratch s("query");
    Database db = s.open();
    db.exec(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, a, p REAL, tag TEXT);"
        "INSERT INTO t VALUES (1, 1, 1.5, 'x'), (2, 2.5, 2, 'y'), (3, NULL, 3, 'z'),"
        " (4, 'abc', NULL, 'w'), (5, 7, 0.5, 'v');"
        "CREATE INDEX t_tag ON t (tag)");
    auto ids = [&](const std::string& where, const Params& params = {}) {
        return tst::flatten(db.exec("SELECT id FROM t WHERE " + where + " ORDER BY id", params));
    };
    CHECK_EQ(ids("a < 3"), std::string("1;2"));
    // Text sorts above every number, so the text row passes from the other side.
    CHECK_EQ(ids("2 <= a"), std::string("2;4;5"));
    CHECK_EQ(ids("a = 2.5"), std::string("2"));
    CHECK_EQ(ids("a <> 1"), std::string("2;4;5"));
    CHECK_EQ(ids("p > ?", Params{Value(1)}), std::string("1;2;3"));
    CHECK_EQ(ids("p > ?", Params{Value("2")}), std::string("3"));
    CHECK_EQ(ids("a >= 1 AND p < 2"), std::string("1;5"));
    CHECK_EQ(ids("tag >= 'v' AND p < 1"), std::string("5"));
    CHECK_EQ(ids("id = 5 AND a > 10"), std::string(""));
    CHECK_EQ(ids("id = 5 AND a > 6"), std::string("5"));
    CHECK_EQ(
        tst::flatten(db.exec("SELECT u.id, v.id FROM t u LEFT JOIN t v ON v.a > 5 AND v.id = u.id"
                             " WHERE u.id <= 2 ORDER BY u.id")),
        std::string("1|NULL;2|NULL"));
    // Rows written before ADD COLUMN read its default.
    db.exec("ALTER TABLE t ADD COLUMN q INTEGER DEFAULT 9; INSERT INTO t VALUES (6, 0, 0, 'u', 1)");
    CHECK_EQ(ids("q > 5"), std::string("1;2;3;4;5"));
    db.exec(
        "CREATE TABLE w (k TEXT PRIMARY KEY, n INTEGER) WITHOUT ROWID;"
        "INSERT INTO w VALUES ('a', 1), ('b', 5), ('c', NULL)");
    CHECK_EQ(tst::flatten(db.exec("SELECT k FROM w WHERE n > 2")), std::string("b"));
}

TEST(order_by_sorts_every_storage_class_nulls_placement_and_collation)
{
    tst::Scratch s("query");
    Database db = s.open();
    db.exec("CREATE TABLE m (id INTEGER PRIMARY KEY, v)");
    db.exec("INSERT INTO m VALUES (1, NULL), (2, 1), (3, 'b'), (4, 1.0), (5, -5), (6, x'0001'), (7, 2.5), (8, 'a'),"
            " (9, 9007199254740993), (10, 'B'), (11, x'00'), (12, 9007199254740992.0), (13, NULL), (14, ''),"
            " (15, 'a' || char(0) || 'b')");
    auto ids = [&](const std::string& order) { return tst::flatten(db.exec("SELECT id FROM m ORDER BY " + order)); };
    // NULL, numbers (an integer and a real compared exactly), text, blobs; ties in arrival order
    CHECK_EQ(ids("v"), std::string("1;13;5;2;4;7;12;9;14;10;8;15;3;11;6"));
    CHECK_EQ(ids("v DESC"), std::string("6;11;3;15;8;10;14;9;12;7;2;4;5;1;13"));
    CHECK_EQ(ids("v DESC NULLS FIRST"), std::string("1;13;6;11;3;15;8;10;14;9;12;7;2;4;5"));
    CHECK_EQ(ids("v NULLS LAST"), std::string("5;2;4;7;12;9;14;10;8;15;3;11;6;1;13"));
    CHECK_EQ(ids("v COLLATE NOCASE"), std::string("1;13;5;2;4;7;12;9;14;8;15;3;10;11;6"));
    // two terms, the second deciding within the first; a LIMIT settles only the head
    CHECK_EQ(ids("typeof(v) DESC, v DESC LIMIT 4"), std::string("3;15;8;10"));
    CHECK_EQ(ids("typeof(v), id DESC LIMIT 3 OFFSET 1"), std::string("6;9;5"));
    // many rows, keys longer than the eight bytes compared first
    db.exec("CREATE TABLE w (id INTEGER PRIMARY KEY, a TEXT, b INTEGER)");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 5000) "
            "INSERT INTO w SELECT i, 'common-prefix-' || (i * 7919 % 5000), i * 31 % 97 FROM c");
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM (SELECT a, lag(a) OVER () AS p FROM (SELECT a FROM w ORDER BY a)) "
                               "WHERE p > a")),
             std::string("0"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM (SELECT b, id, lag(b) OVER () AS pb, lag(id) OVER () AS pid FROM "
                               "(SELECT b, id FROM w ORDER BY b DESC, id)) WHERE pb < b OR (pb = b AND pid > id)")),
             std::string("0"));
    CHECK_EQ(tst::flatten(db.exec("SELECT a FROM w ORDER BY a DESC LIMIT 2")),
             std::string("common-prefix-999;common-prefix-998"));
}

TEST(order_by_with_limit_returns_the_head_of_the_full_order)
{
    tst::Scratch s("query");
    Database db = s.open();
    // k repeats, and is NULL, text or a real in places; 6000 rows so the collector fills and prunes many times
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, k, g INTEGER, pad TEXT)");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 6000) "
            "INSERT INTO t SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 'x' || (i % 7) "
            "WHEN i % 5 = 0 THEN (i * 7919 % 1000) + 0.5 ELSE i * 7919 % 1000 END, i % 13, 'row ' || i FROM c");
    db.exec("CREATE TABLE u (g INTEGER PRIMARY KEY, w INTEGER)");
    db.exec("INSERT INTO u SELECT DISTINCT g, 100 - g FROM t");
    auto head = [&](const std::string& select, const std::string& order, int limit, int offset = 0) {
        const Result all = db.exec(select + " ORDER BY " + order);
        const Result some = db.exec(select + " ORDER BY " + order + " LIMIT " + std::to_string(limit) +
                                    (offset ? " OFFSET " + std::to_string(offset) : std::string()));
        CHECK_EQ(some.size(), std::size_t(limit));
        bool same = some.size() == std::size_t(limit);
        for (std::size_t r = 0; same && r < some.size(); ++r) {
            for (std::size_t col = 0; same && col < some[r].size(); ++col)
                same = some[r][col] == all[r + std::size_t(offset)][col];
        }
        CHECK(same);
    };
    head("SELECT id, k, pad FROM t", "k", 50);
    head("SELECT id, k, pad FROM t", "k DESC", 50);
    head("SELECT id, k FROM t", "k NULLS LAST", 200);
    head("SELECT id, k FROM t", "k DESC NULLS FIRST", 200);
    head("SELECT id, k, g FROM t", "k, g DESC, id", 75);
    head("SELECT id, k, g FROM t", "g DESC, k", 75, 30);
    head("SELECT id, k FROM t WHERE g <> 3", "k", 10, 5);
    head("SELECT id, k + 1 FROM t", "k + 1 DESC", 40);
    // the sorted column in the inner table of a join, and in the outer one
    head("SELECT t.id, u.w, t.k FROM u JOIN t ON t.g = u.g", "t.k, t.id", 60);
    head("SELECT t.id, u.w FROM t JOIN u ON u.g = t.g", "u.w DESC, t.id", 60);
    head("SELECT t.id, u.w FROM t LEFT JOIN u ON u.g = t.g AND u.w > 95", "u.w, t.id", 60);
    // through an index, and index-only
    db.exec("CREATE INDEX t_g ON t (g)");
    head("SELECT id, k FROM t WHERE g = 4", "k DESC", 25);
    head("SELECT g FROM t", "g DESC", 25);
}

int main()
{
    return tst::runAll("query");
}
