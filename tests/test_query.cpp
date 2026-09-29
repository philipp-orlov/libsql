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
    CHECK_THROWS(db.exec("SELECT lower(name) FROM staff"), ErrorCode::Unsupported);
    CHECK_THROWS(db.exec("SELECT * FROM staff LEFT JOIN staff s2 ON 1"), ErrorCode::Unsupported);
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

int main()
{
    return tst::runAll("query");
}
