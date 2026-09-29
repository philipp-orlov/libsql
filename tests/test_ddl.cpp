// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sql_test.hpp"

#include "nosql/nosql.hpp"

using namespace sql;

TEST(createsAndDropsTables)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL)");
    CHECK_EQ(db.tables().size(), std::size_t(1));
    CHECK_EQ(db.tables()[0], std::string("people"));

    CHECK_THROWS(db.exec("CREATE TABLE people (id INTEGER)"), ErrorCode::TableExists);
    db.exec("CREATE TABLE IF NOT EXISTS people (id INTEGER)");
    CHECK_EQ(db.tables().size(), std::size_t(1));

    db.exec("DROP TABLE people");
    CHECK(db.tables().empty());
    CHECK_THROWS(db.exec("DROP TABLE people"), ErrorCode::NoSuchTable);
    db.exec("DROP TABLE IF EXISTS people");
}

TEST(reportsTheDeclaredSchema)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(40) NOT NULL, "
            "score REAL DEFAULT 1.5, tag TEXT UNIQUE, at TIMESTAMP, raw BLOB)");

    const TableInfo info = db.table("T");  // names are case-insensitive
    CHECK_EQ(info.name, std::string("t"));
    CHECK_EQ(info.primaryKey, std::vector<int>{0});
    CHECK_EQ(info.columns.size(), std::size_t(6));
    CHECK_EQ(info.columns[1].type, Type::Text);
    CHECK(info.columns[1].notNull);
    CHECK_EQ(info.columns[2].type, Type::Real);
    CHECK(info.columns[2].hasDefault);
    CHECK_EQ(info.columns[2].defaultValue.real(), 1.5);
    CHECK(info.columns[3].unique);
    CHECK_EQ(info.columns[4].type, Type::Datetime);
    CHECK_EQ(info.columns[5].type, Type::Blob);
}

TEST(schemaSurvivesReopening)
{
    tst::Scratch s("ddl");
    {
        Database db = s.open();
        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
        db.exec("CREATE INDEX by_name ON t (name)");
        db.exec("INSERT INTO t VALUES (4, 'four')");
    }
    Database db = s.open();
    CHECK_EQ(db.tables().size(), std::size_t(1));
    CHECK_EQ(db.indexes().size(), std::size_t(1));
    CHECK_EQ(db.indexes()[0].columns[0], std::string("name"));
    CHECK_EQ(tst::only(db.exec("SELECT name FROM t WHERE id = 4")), std::string("four"));
}

/// Catalog table records once carried a single key position; they now carry a
/// list. Databases written before the change must still open, so rewrite a
/// fresh record in the old form and read it back. In that form the trailing
/// varint is `position + 1`, where the new form has `count` then positions;
/// for one key at position 0 that is `01` against `01 00`, and for a rowid
/// table `00` either way, so the old record is the new one with its version
/// byte turned down and, for the keyed table, its last byte dropped.
TEST(readsTableRecordsInTheOldKeyFormat)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE keyed (id INTEGER PRIMARY KEY, name TEXT)");
    db.exec("CREATE TABLE plain (name TEXT)");
    db.exec("INSERT INTO keyed VALUES (4, 'four')");
    db.exec("INSERT INTO plain VALUES ('p')");

    db.env().write([](nosql::Txn& t) {
        nosql::Db meta = t.db(nosql::Slice("sql_catalog", 11));
        for (const char* name : {"tkeyed", "tplain"}) {
            const nosql::Slice key(name, 6);
            std::string record(meta.get(key)->view());
            CHECK_EQ(int(record[0]), 2);
            record[0] = 1;
            if (name[1] == 'k') {
                CHECK_EQ(record.substr(record.size() - 2), std::string("\x01\x00", 2));
                record.pop_back();
            } else {
                CHECK_EQ(record.back(), '\0');
            }
            meta.put(key, nosql::Slice(record.data(), record.size()));
        }
    });

    CHECK_EQ(db.table("keyed").primaryKey, std::vector<int>{0});
    CHECK(db.table("keyed").columns[0].primaryKey);
    CHECK(db.table("plain").primaryKey.empty());
    CHECK_EQ(tst::only(db.exec("SELECT name FROM keyed WHERE id = 4")), std::string("four"));
    CHECK_EQ(tst::only(db.exec("SELECT rowid FROM plain")), std::string("1"));
    // The next write of the schema brings the record forward again.
    db.exec("INSERT INTO keyed (name) VALUES ('five')");
    CHECK_EQ(db.exec("INSERT INTO plain VALUES ('q')").changes(), std::uint64_t(1));
    CHECK_EQ(tst::only(db.exec("SELECT id FROM keyed WHERE name = 'five'")), std::string("5"));
}

TEST(createsAndDropsIndexes)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)");
    db.exec("CREATE INDEX by_a ON t (a)");
    db.exec("CREATE UNIQUE INDEX by_b ON t (b)");

    std::vector<IndexInfo> all = db.indexes("t");
    CHECK_EQ(all.size(), std::size_t(2));
    CHECK_THROWS(db.exec("CREATE INDEX by_a ON t (b)"), ErrorCode::IndexExists);
    db.exec("CREATE INDEX IF NOT EXISTS by_a ON t (b)");
    CHECK_THROWS(db.exec("CREATE INDEX by_c ON t (nope)"), ErrorCode::NoSuchColumn);
    CHECK_THROWS(db.exec("CREATE INDEX by_c ON missing (a)"), ErrorCode::NoSuchTable);

    db.exec("DROP INDEX by_a");
    CHECK_EQ(db.indexes("t").size(), std::size_t(1));
    CHECK_THROWS(db.exec("DROP INDEX by_a"), ErrorCode::NoSuchIndex);
    db.exec("DROP INDEX IF EXISTS by_a");

    db.exec("DROP TABLE t");
    CHECK(db.indexes().empty());
}

TEST(indexesAreBackfilledOverExistingRows)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
    for (int i = 0; i < 50; ++i)
        db.exec("INSERT INTO t (a) VALUES (?)", {Value(i % 7)});
    db.exec("CREATE INDEX by_a ON t (a)");

    Result r = db.exec("SELECT id FROM t WHERE a = 3 ORDER BY id");
    CHECK_EQ(r.size(), std::size_t(7));
    CHECK_EQ(r[0]["id"].integer(), std::int64_t(4));
}

TEST(backfillRefusesToBreakUniqueness)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
    db.exec("INSERT INTO t (a) VALUES (1), (1)");
    CHECK_THROWS(db.exec("CREATE UNIQUE INDEX by_a ON t (a)"), ErrorCode::ConstraintViolation);
}

TEST(rejectsMalformedSchemas)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER, a TEXT)"), ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER PRIMARY KEY, b TEXT PRIMARY KEY)"),
                 ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("CREATE TABLE t (rowid INTEGER)"), ErrorCode::InvalidArgument);
    CHECK_THROWS(db.exec("CREATE TABLE t (a WIDGET)"), ErrorCode::Unsupported);
    CHECK_THROWS(db.exec("CREATE TABLE t (a INTEGER, PRIMARY KEY (missing))"),
                 ErrorCode::NoSuchColumn);
    CHECK_THROWS(db.exec("CREATE TABLE t"), ErrorCode::SyntaxError);
}

TEST(tableLevelPrimaryKeyWorks)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE t (code TEXT, label TEXT, PRIMARY KEY (code))");
    CHECK_EQ(db.table("t").primaryKey, std::vector<int>{0});
    db.exec("INSERT INTO t VALUES ('a', 'first')");
    CHECK_THROWS(db.exec("INSERT INTO t VALUES ('a', 'again')"), ErrorCode::ConstraintViolation);
}

TEST(schemaChangesRollBackWithTheirTransaction)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE kept (id INTEGER PRIMARY KEY, v TEXT)");
    {
        Transaction t = db.begin();
        t.exec("CREATE TABLE gone (id INTEGER PRIMARY KEY)");
        t.exec("CREATE INDEX kept_by_v ON kept (v)");
        t.exec("INSERT INTO gone (id) VALUES (1)");
        CHECK_EQ(tst::only(t.exec("SELECT COUNT(*) FROM gone")), std::string("1"));
        t.rollback();
    }
    // Neither the table nor the index survived, and the cached schema agrees
    // with the store rather than with the transaction that never committed.
    CHECK_THROWS(db.exec("SELECT * FROM gone"), ErrorCode::NoSuchTable);
    CHECK(db.indexes("kept").empty());
    db.exec("CREATE TABLE gone (id INTEGER PRIMARY KEY, w TEXT)");
    CHECK_EQ(db.table("gone").columns.size(), std::size_t(2));
    db.exec("CREATE INDEX kept_by_v ON kept (v)");
    CHECK_EQ(db.indexes("kept").size(), std::size_t(1));
}

TEST(readSnapshotKeepsTheSchemaItStartedWith)
{
    tst::Scratch s("ddl");
    Database db = s.open();
    db.exec("CREATE TABLE a (id INTEGER PRIMARY KEY)");
    db.exec("INSERT INTO a (id) VALUES (1)");

    Transaction older = db.beginRead();
    CHECK_EQ(tst::only(older.exec("SELECT COUNT(*) FROM a")), std::string("1"));

    // A commit in the meantime: a new table, a dropped one, and more rows.
    db.exec("CREATE TABLE b (id INTEGER PRIMARY KEY)");
    db.exec("INSERT INTO a (id) VALUES (2)");
    Statement countA = db.prepare("SELECT COUNT(*) FROM a");
    CHECK_EQ(countA.exec()[0][0].integer(), std::int64_t(2));
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM b")), std::string("0"));

    // The old snapshot sees neither the table nor the row, and a statement
    // planned against the newer schema still runs correctly against it.
    CHECK_THROWS(older.exec("SELECT * FROM b"), ErrorCode::NoSuchTable);
    CHECK_EQ(tst::only(older.exec("SELECT COUNT(*) FROM a")), std::string("1"));
    CHECK_EQ(countA.exec(older)[0][0].integer(), std::int64_t(1));
    older.rollback();

    db.exec("DROP TABLE a");
    CHECK_THROWS(countA.exec(), ErrorCode::NoSuchTable);
}

int main()
{
    return tst::runAll("ddl");
}
