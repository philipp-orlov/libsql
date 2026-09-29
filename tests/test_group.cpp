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
            " (1, 'ada',     'eng', 100),"
            " (2, 'grace',   'eng', 120),"
            " (3, 'edsger',  'ops',  90),"
            " (4, 'barbara', 'ops', 130),"
            " (5, 'alan',    NULL,  NULL)");
    return db;
}

}  // namespace

TEST(aggregatesTheWholeTable)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    Result r = db.exec("SELECT COUNT(*), COUNT(pay), SUM(pay), AVG(pay), MIN(pay), MAX(pay)"
                       " FROM staff");
    CHECK_EQ(r.size(), std::size_t(1));
    CHECK_EQ(tst::flatten(r), std::string("5|4|440|110|90|130"));
    CHECK_EQ(r.columns()[0], std::string("COUNT(*)"));
    CHECK_EQ(r.columns()[3], std::string("AVG(pay)"));
    CHECK_EQ(r[0]["SUM(pay)"].type(), Type::Integer);
    CHECK_EQ(r[0]["AVG(pay)"].type(), Type::Real);

    CHECK_EQ(tst::only(db.exec("SELECT MIN(name) FROM staff")), std::string("ada"));
    CHECK_EQ(tst::only(db.exec("SELECT MAX(name) FROM staff")), std::string("grace"));
}

TEST(aggregatesOverNothing)
{
    tst::Scratch s("group");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
    // No rows is still one row: a count of zero, and nothing else to report.
    CHECK_EQ(tst::flatten(db.exec("SELECT COUNT(*), SUM(v), AVG(v), MIN(v), MAX(v) FROM t")),
             std::string("0|NULL|NULL|NULL|NULL"));
    // With GROUP BY there is no key to report it under, so there are no rows.
    CHECK(db.exec("SELECT v, COUNT(*) FROM t GROUP BY v").empty());
}

TEST(sumsKeepTheirType)
{
    tst::Scratch s("group");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER, r REAL)");
    db.exec("INSERT INTO t (n, r) VALUES (1, 0.5), (2, 0.25)");

    Result k = db.exec("SELECT SUM(n), SUM(r), AVG(n) FROM t");
    CHECK_EQ(k[0][0].type(), Type::Integer);
    CHECK_EQ(k[0][0].integer(), std::int64_t(3));
    CHECK_EQ(k[0][1].type(), Type::Real);
    CHECK_EQ(k[0][1].real(), 0.75);
    CHECK_EQ(k[0][2].real(), 1.5);
}

TEST(groupsByOneColumn)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, COUNT(*) FROM staff GROUP BY dept")),
             std::string("NULL|1;eng|2;ops|2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, SUM(pay) FROM staff GROUP BY dept")),
             std::string("NULL|NULL;eng|220;ops|220"));
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, MIN(pay), MAX(pay) FROM staff"
                                  " WHERE dept IS NOT NULL GROUP BY dept")),
             std::string("eng|100|120;ops|90|130"));
}

TEST(groupsByAnExpressionAndBySeveralColumns)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_EQ(tst::flatten(db.exec("SELECT pay > 100, COUNT(*) FROM staff"
                                  " WHERE pay IS NOT NULL GROUP BY pay > 100")),
             std::string("0|2;1|2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, pay > 100, COUNT(*) FROM staff"
                                  " WHERE dept IS NOT NULL GROUP BY dept, pay > 100")),
             std::string("eng|0|1;eng|1|1;ops|0|1;ops|1|1"));
}

TEST(groupByWithoutAggregatesActsLikeDistinct)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_EQ(tst::flatten(db.exec("SELECT dept FROM staff GROUP BY dept")),
             std::string("NULL;eng;ops"));
}

TEST(havingFiltersGroups)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, COUNT(*) FROM staff GROUP BY dept"
                                  " HAVING COUNT(*) > 1")),
             std::string("eng|2;ops|2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, MAX(pay) FROM staff GROUP BY dept"
                                  " HAVING MAX(pay) >= 130")),
             std::string("ops|130"));
    CHECK(db.exec("SELECT dept FROM staff GROUP BY dept HAVING COUNT(*) > 99").empty());
    // HAVING on its own treats the table as a single group.
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM staff HAVING COUNT(*) > 4")),
             std::string("5"));
}

TEST(ordersAndPagesGroups)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, COUNT(*) FROM staff GROUP BY dept"
                                  " ORDER BY COUNT(*) DESC, dept LIMIT 1")),
             std::string("eng|2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, SUM(pay) FROM staff WHERE dept IS NOT NULL"
                                  " GROUP BY dept ORDER BY dept DESC")),
             std::string("ops|220;eng|220"));
    CHECK_EQ(tst::flatten(db.exec("SELECT dept FROM staff GROUP BY dept"
                                  " ORDER BY dept LIMIT 1 OFFSET 1")),
             std::string("eng"));
}

TEST(ordersByResultAliasAndPosition)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, COUNT(*) AS n FROM staff GROUP BY dept"
                                  " ORDER BY n DESC, dept")),
             std::string("eng|2;ops|2;NULL|1"));
    CHECK_EQ(tst::flatten(db.exec("SELECT dept, SUM(pay) AS total FROM staff"
                                  " WHERE dept IS NOT NULL GROUP BY dept ORDER BY 1 DESC")),
             std::string("ops|220;eng|220"));
    CHECK_EQ(tst::flatten(db.exec("SELECT name FROM staff ORDER BY 1 LIMIT 2")),
             std::string("ada;alan"));
    CHECK_THROWS(db.exec("SELECT name FROM staff ORDER BY 7"), ErrorCode::InvalidArgument);
}

TEST(aggregatesCombineWithOtherExpressions)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_EQ(tst::only(db.exec("SELECT SUM(pay) / COUNT(pay) FROM staff")), std::string("110"));
    CHECK_EQ(tst::only(db.exec("SELECT MAX(pay) - MIN(pay) FROM staff")), std::string("40"));
    CHECK_EQ(tst::only(db.exec("SELECT 'n=' || COUNT(*) FROM staff")), std::string("n=5"));
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM staff WHERE pay > ?", {Value(95)})),
             std::string("3"));
}

TEST(barecolumnsReportTheFirstRowOfTheirGroup)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    Result r = db.exec("SELECT dept, name, COUNT(*) FROM staff WHERE dept IS NOT NULL"
                       " GROUP BY dept ORDER BY dept");
    CHECK_EQ(tst::flatten(r), std::string("eng|ada|2;ops|edsger|2"));
}

TEST(aggregatesOverAJoin)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    db.exec("CREATE TABLE site (dept TEXT PRIMARY KEY, city TEXT)");
    db.exec("INSERT INTO site VALUES ('eng', 'oslo'), ('ops', 'lima')");

    CHECK_EQ(tst::flatten(db.exec("SELECT t.city, COUNT(*), SUM(s.pay)"
                                  " FROM staff s JOIN site t ON s.dept = t.dept"
                                  " GROUP BY t.city ORDER BY t.city")),
             std::string("lima|2|220;oslo|2|220"));
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM staff s JOIN site t ON s.dept = t.dept")),
             std::string("4"));
}

TEST(rejectsAggregatesWhereTheyCannotWork)
{
    tst::Scratch s("group");
    Database db = seeded(s);
    CHECK_THROWS(db.exec("SELECT name FROM staff WHERE COUNT(*) > 1"),
                 ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("SELECT dept FROM staff GROUP BY COUNT(*)"), ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("SELECT SUM(COUNT(pay)) FROM staff"), ErrorCode::Unsupported);
    CHECK_THROWS(db.exec("SELECT COUNT(DISTINCT dept) FROM staff"), ErrorCode::Unsupported);
    CHECK_THROWS(db.exec("SELECT upper(name) FROM staff"), ErrorCode::Unsupported);
    CHECK_THROWS(db.exec("SELECT COUNT(*) FROM staff GROUP BY nope"), ErrorCode::NoSuchColumn);
    CHECK_THROWS(db.exec("SELECT SUM(name) FROM staff"), ErrorCode::TypeMismatch);
    CHECK_THROWS(db.exec("SELECT COUNT(*"), ErrorCode::SyntaxError);
}

int main()
{
    return tst::runAll("group");
}
