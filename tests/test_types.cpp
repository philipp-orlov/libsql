// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sql_test.hpp"

using namespace sql;

TEST(datetimeRoundTrips)
{
    CHECK_EQ(formatDatetime(parseDatetime("2024-03-01 12:34:56")),
             std::string("2024-03-01 12:34:56"));
    CHECK_EQ(formatDatetime(parseDatetime("2024-03-01")), std::string("2024-03-01 00:00:00"));
    CHECK_EQ(formatDatetime(parseDatetime("2024-03-01T12:34:56Z")),
             std::string("2024-03-01 12:34:56"));
    CHECK_EQ(formatDatetime(parseDatetime("1969-07-20 20:17:40")),
             std::string("1969-07-20 20:17:40"));
    CHECK_EQ(formatDatetime(parseDatetime("2024-03-01 12:34:56.000250")),
             std::string("2024-03-01 12:34:56.000250"));
    CHECK_EQ(parseDatetime("1970-01-01 00:00:00").usec, std::int64_t(0));
    CHECK_THROWS(parseDatetime("not a time"), ErrorCode::TypeMismatch);
    CHECK_THROWS(parseDatetime("2024-13-01"), ErrorCode::TypeMismatch);
}

TEST(valuesKnowTheirType)
{
    CHECK_EQ(Value().type(), Type::Null);
    CHECK_EQ(Value(7).type(), Type::Integer);
    CHECK_EQ(Value(7.5).type(), Type::Real);
    CHECK_EQ(Value("hi").type(), Type::Text);
    CHECK_EQ(Value(Datetime{5}).type(), Type::Datetime);
    CHECK_EQ(Value::blob("ab", 2).type(), Type::Blob);
    CHECK(Value().isNull());
    CHECK(!Value(0).isNull());
}

TEST(castsFollowTheDeclaredType)
{
    CHECK_EQ(Value("42").cast(Type::Integer).integer(), std::int64_t(42));
    CHECK_EQ(Value(42).cast(Type::Text).text(), std::string("42"));
    CHECK_EQ(Value(3).cast(Type::Real).real(), 3.0);
    CHECK_EQ(Value("2024-01-02").cast(Type::Datetime).datetime().usec,
             parseDatetime("2024-01-02").usec);
    CHECK(Value().cast(Type::Integer).isNull());
    CHECK_THROWS(Value("banana").cast(Type::Integer), ErrorCode::TypeMismatch);
    CHECK_THROWS(Value::blob("ab", 2).cast(Type::Integer), ErrorCode::TypeMismatch);
}

TEST(orderingRanksTheStorageClasses)
{
    CHECK(compare(Value(), Value(0)) < 0);
    CHECK(compare(Value(1), Value(2.5)) < 0);   // numbers interleave
    CHECK(compare(Value(3.5), Value(2)) > 0);
    CHECK(compare(Value(9999), Value(Datetime{0})) < 0);
    CHECK(compare(Value(Datetime{0}), Value("a")) < 0);
    CHECK(compare(Value("a"), Value::blob("", 0)) < 0);
    CHECK_EQ(compare(Value("abc"), Value("abc")), 0);
}

TEST(storesEveryTypeAndReadsItBack)
{
    tst::Scratch s("types");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER, r REAL, s TEXT, "
            "d DATETIME, b BLOB)");
    db.exec("INSERT INTO t VALUES (1, -5, 2.5, 'hello', '2024-03-01 12:00:00', x'00ff10')");
    db.exec("INSERT INTO t VALUES (2, NULL, NULL, NULL, NULL, NULL)");

    Result r = db.exec("SELECT n, r, s, d, b FROM t WHERE id = 1");
    CHECK_EQ(r.size(), std::size_t(1));
    CHECK_EQ(r[0]["n"].integer(), std::int64_t(-5));
    CHECK_EQ(r[0]["r"].real(), 2.5);
    CHECK_EQ(r[0]["s"].text(), std::string("hello"));
    CHECK_EQ(r[0]["d"].datetime().usec, parseDatetime("2024-03-01 12:00:00").usec);
    CHECK_EQ(r[0]["b"].blob().size(), std::size_t(3));
    CHECK_EQ(r[0]["b"].toText(), std::string("00ff10"));

    r = db.exec("SELECT n, r, s, d, b FROM t WHERE id = 2");
    for (const Value& v : r[0])
        CHECK(v.isNull());
}

TEST(textIsCoercedIntoTheDeclaredType)
{
    tst::Scratch s("types");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER, d DATETIME)");
    db.exec("INSERT INTO t VALUES (1, '17', '2020-02-29 01:02:03')");

    Result r = db.exec("SELECT n, d FROM t");
    CHECK_EQ(r[0]["n"].type(), Type::Integer);
    CHECK_EQ(r[0]["n"].integer(), std::int64_t(17));
    CHECK_EQ(r[0]["d"].type(), Type::Datetime);
    CHECK_EQ(r[0]["d"].toText(), std::string("2020-02-29 01:02:03"));

    CHECK_THROWS(db.exec("INSERT INTO t VALUES (2, 'nope', NULL)"), ErrorCode::TypeMismatch);
}

TEST(blobsSurviveEmbeddedZeros)
{
    tst::Scratch s("types");
    Database db = s.open();
    db.exec("CREATE TABLE t (k BLOB PRIMARY KEY, v BLOB)");
    db.exec("INSERT INTO t VALUES (x'000102', x'00'), (x'0001', x'ff00ff')");

    Result r = db.exec("SELECT k, v FROM t ORDER BY k");
    CHECK_EQ(tst::flatten(r), std::string("0001|ff00ff;000102|00"));
    CHECK_EQ(tst::only(db.exec("SELECT v FROM t WHERE k = x'000102'")), std::string("00"));
}

TEST(largeBlobsRoundTrip)
{
    tst::Scratch s("types");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, bytes BLOB)");

    Blob payload(1u << 20);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = std::byte((i * 31 + 7) & 0xff);
    db.exec("INSERT INTO t (bytes) VALUES (?)", {Value(payload)});

    const Value read = db.exec("SELECT bytes FROM t")[0][0];
    CHECK_EQ(read.blob().size(), payload.size());
    CHECK(read.blob() == payload);
    // An empty blob is a value; a missing one is not.
    db.exec("INSERT INTO t (bytes) VALUES (x''), (NULL)");
    CHECK_EQ(db.exec("SELECT id FROM t WHERE bytes IS NULL").size(), std::size_t(1));
    CHECK_EQ(db.exec("SELECT id FROM t WHERE bytes = x''").size(), std::size_t(1));
}

TEST(datetimesSortChronologically)
{
    tst::Scratch s("types");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, at DATETIME)");
    db.exec("INSERT INTO t (at) VALUES ('2024-01-02'), ('1930-05-06 07:08:09'), "
            "('2024-01-01 23:59:59')");

    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t ORDER BY at")), std::string("2;3;1"));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t WHERE at > '2024-01-01' ORDER BY id")),
             std::string("1;3"));
}

int main()
{
    return tst::runAll("types");
}
