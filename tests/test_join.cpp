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
    CHECK_EQ(r.columns()[0], std::string("s.name"));
    CHECK_EQ(r.columns()[1], std::string("d.name"));
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
    CHECK_EQ(r.columns()[0], std::string("s.id"));
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
    CHECK_THROWS(db.exec("SELECT s.name FROM staff s JOIN dept s ON 1 = 1"),
                 ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("SELECT s.name FROM staff s JOIN nothing d ON 1 = 1"),
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

int main()
{
    return tst::runAll("join");
}
