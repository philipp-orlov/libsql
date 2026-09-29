// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sql_test.hpp"

#include <string>

using namespace sql;

namespace {

/// The same rows in two tables, one indexed and one not, so that every query
/// has a scan-only answer to be checked against.
Database twins(const tst::Scratch& s)
{
    Database db = s.open();
    for (const char* name : {"plain", "fancy"}) {
        db.exec(std::string("CREATE TABLE ") + name +
                " (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, c REAL)");
    }
    db.exec("CREATE INDEX fancy_a ON fancy (a)");
    db.exec("CREATE INDEX fancy_ab ON fancy (a, b)");
    db.exec("CREATE INDEX fancy_c ON fancy (c)");

    Transaction t = db.begin();
    for (int i = 0; i < 200; ++i) {
        const Params row{Value(i), Value(i % 13), Value("s" + std::to_string(i % 7)),
                         Value(double(i) / 4.0)};
        t.exec("INSERT INTO plain VALUES (?, ?, ?, ?)", row);
        t.exec("INSERT INTO fancy VALUES (?, ?, ?, ?)", row);
    }
    t.commit();
    return db;
}

void agree(Database& db, const std::string& tail)
{
    const std::string plain = tst::flatten(db.exec("SELECT id FROM plain WHERE " + tail));
    const std::string fancy = tst::flatten(db.exec("SELECT id FROM fancy WHERE " + tail));
    if (plain != fancy)
        throw std::runtime_error("indexed and scanned answers differ for: " + tail +
                                 "\n  plain = " + plain + "\n  fancy = " + fancy);
    if (plain.empty())
        throw std::runtime_error("query matched nothing, so it proves nothing: " + tail);
}

}  // namespace

TEST(indexedLookupsMatchScans)
{
    tst::Scratch s("index");
    Database db = twins(s);
    agree(db, "a = 5 ORDER BY id");
    agree(db, "a = 5 AND b = 's3' ORDER BY id");
    agree(db, "a > 10 ORDER BY id");
    agree(db, "a >= 11 ORDER BY id");
    agree(db, "a < 2 ORDER BY id");
    agree(db, "a <= 1 ORDER BY id");
    agree(db, "a BETWEEN 3 AND 5 ORDER BY id");
    agree(db, "a = 4 AND id > 100 ORDER BY id");
    agree(db, "c > 20.0 AND c < 30.0 ORDER BY id");
    agree(db, "a = 3 OR b = 's6' ORDER BY id");
    agree(db, "id = 42 ORDER BY id");
    agree(db, "id BETWEEN 10 AND 14 ORDER BY id");
    agree(db, "b = 's2' ORDER BY id");
}

TEST(indexedLookupsSurviveEdits)
{
    tst::Scratch s("index");
    Database db = twins(s);
    for (const char* t : {"plain", "fancy"}) {
        db.exec(std::string("UPDATE ") + t + " SET a = a + 100 WHERE id % 3 = 0");
        db.exec(std::string("DELETE FROM ") + t + " WHERE id > 150");
        db.exec(std::string("INSERT INTO ") + t + " VALUES (500, 7, 'late', 0.5)");
    }
    agree(db, "a = 7 ORDER BY id");
    agree(db, "a > 100 ORDER BY id");
    agree(db, "a = 100 ORDER BY id");
    agree(db, "b = 'late' ORDER BY id");
}

TEST(indexLookupsIgnoreMistypedBounds)
{
    tst::Scratch s("index");
    Database db = twins(s);
    // 'x' cannot be an INTEGER, so the plan has to widen instead of guessing.
    CHECK(db.exec("SELECT id FROM fancy WHERE a = 'x'").empty());
    CHECK_EQ(db.exec("SELECT id FROM fancy WHERE a < 'x'").size(),
             db.exec("SELECT id FROM plain WHERE a < 'x'").size());
    CHECK(db.exec("SELECT id FROM fancy WHERE a = NULL").empty());
}

TEST(uniqueIndexesRejectDuplicates)
{
    tst::Scratch s("index");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER)");
    db.exec("CREATE UNIQUE INDEX pair ON t (a, b)");
    db.exec("INSERT INTO t (a, b) VALUES (1, 1), (1, 2), (2, 1)");

    CHECK_THROWS(db.exec("INSERT INTO t (a, b) VALUES (1, 2)"), ErrorCode::ConstraintViolation);
    CHECK_THROWS(db.exec("UPDATE t SET b = 1 WHERE a = 1 AND b = 2"),
                 ErrorCode::ConstraintViolation);
    // Updating a row to its own value is not a collision with itself.
    db.exec("UPDATE t SET a = 1 WHERE a = 1 AND b = 2");
    // NULLs are exempt.
    db.exec("INSERT INTO t (a, b) VALUES (NULL, 1), (NULL, 1)");
    CHECK_EQ(db.exec("SELECT id FROM t").size(), std::size_t(5));
}

TEST(indexesRebuildAfterBeingDropped)
{
    tst::Scratch s("index");
    Database db = twins(s);
    db.exec("DROP INDEX fancy_a");
    db.exec("DROP INDEX fancy_ab");
    agree(db, "a = 5 ORDER BY id");
    db.exec("CREATE INDEX fancy_a ON fancy (a)");
    agree(db, "a = 5 ORDER BY id");
}

TEST(compositeIndexesAcceptAPrefix)
{
    tst::Scratch s("index");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b INTEGER, c TEXT)");
    db.exec("CREATE INDEX abc ON t (a, b, c)");
    db.exec("INSERT INTO t (a, b, c) VALUES"
            " ('x', 1, 'p'), ('x', 1, 'q'), ('x', 2, 'p'), ('y', 1, 'p')");

    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t WHERE a = 'x' ORDER BY id")),
             std::string("1;2;3"));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t WHERE a = 'x' AND b = 1 ORDER BY id")),
             std::string("1;2"));
    CHECK_EQ(
        tst::flatten(db.exec("SELECT id FROM t WHERE a = 'x' AND b = 1 AND c = 'q' ORDER BY id")),
        std::string("2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t WHERE a = 'x' AND b >= 2 ORDER BY id")),
             std::string("3"));
    // A trailing column alone cannot use the index, and must still be right.
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM t WHERE c = 'p' ORDER BY id")),
             std::string("1;3;4"));
}

int main()
{
    return tst::runAll("index");
}
