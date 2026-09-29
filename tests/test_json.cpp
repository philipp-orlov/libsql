// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// FOR JSON AUTO / PATH, the modifiers, and the C++ JsonWriter.

#include "sql/json.hpp"
#include "sql_test.hpp"

namespace {

/// Two tables in a one-to-many shape, which is what AUTO nesting is about.
sql::Database shop(const tst::Scratch& scratch)
{
    sql::Database db = scratch.open();
    db.exec("CREATE TABLE customer (id INTEGER PRIMARY KEY, name TEXT NOT NULL, note TEXT)");
    db.exec("CREATE TABLE ordered (id INTEGER PRIMARY KEY, customer_id INTEGER NOT NULL,"
            " item TEXT NOT NULL, qty INTEGER)");
    db.exec("INSERT INTO customer (id, name, note) VALUES (1, 'Ada', NULL), (2, 'Bo', 'vip')");
    db.exec("INSERT INTO ordered (id, customer_id, item, qty) VALUES"
            " (1, 1, 'anvil', 2), (2, 1, 'rope', 5), (3, 2, 'kite', 1)");
    return db;
}

std::string json(sql::Database& db, const char* text)
{
    return tst::only(db.exec(text));
}

}  // namespace

TEST(json_auto_flat)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT id, name FROM customer ORDER BY id FOR JSON AUTO"),
             R"([{"id":1,"name":"Ada"},{"id":2,"name":"Bo"}])");
}

TEST(json_auto_nests_joined_tables)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    // One element per customer, with that customer's rows gathered under the
    // joined table's alias.
    CHECK_EQ(json(db, "SELECT c.name, o.item, o.qty"
                      " FROM customer c INNER JOIN ordered o ON o.customer_id = c.id"
                      " ORDER BY c.id, o.id FOR JSON AUTO"),
             R"([{"name":"Ada","o":[{"item":"anvil","qty":2},{"item":"rope","qty":5}]},)"
             R"({"name":"Bo","o":[{"item":"kite","qty":1}]}])");
}

TEST(json_auto_star_nests_too)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT c.id, o.* FROM customer c"
                      " INNER JOIN ordered o ON o.customer_id = c.id"
                      " WHERE c.id = 2 FOR JSON AUTO"),
             R"([{"id":2,"o":[{"id":3,"customer_id":2,"item":"kite","qty":1}]}])");
}

TEST(json_path_uses_dotted_aliases)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT o.id, c.name AS \"customer.name\", o.item AS \"line.item\","
                      " o.qty AS \"line.qty\""
                      " FROM customer c INNER JOIN ordered o ON o.customer_id = c.id"
                      " WHERE o.id = 1 FOR JSON PATH"),
             R"([{"id":1,"customer":{"name":"Ada"},"line":{"item":"anvil","qty":2}}])");
}

TEST(json_path_gives_one_object_per_row)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    // No collapsing: PATH repeats the customer on every line.
    CHECK_EQ(json(db, "SELECT c.name, o.item"
                      " FROM customer c INNER JOIN ordered o ON o.customer_id = c.id"
                      " WHERE c.id = 1 ORDER BY o.id FOR JSON PATH"),
             R"([{"name":"Ada","item":"anvil"},{"name":"Ada","item":"rope"}])");
}

TEST(json_nulls_are_dropped_unless_asked_for)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT id, note FROM customer WHERE id = 1 FOR JSON PATH"),
             R"([{"id":1}])");
    CHECK_EQ(json(db, "SELECT id, note FROM customer WHERE id = 1"
                      " FOR JSON PATH, INCLUDE_NULL_VALUES"),
             R"([{"id":1,"note":null}])");
}

TEST(json_empty_nested_object_is_dropped)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT id, note AS \"extra.note\" FROM customer WHERE id = 1"
                      " FOR JSON PATH"),
             R"([{"id":1}])");
    CHECK_EQ(json(db, "SELECT id, note AS \"extra.note\" FROM customer WHERE id = 1"
                      " FOR JSON PATH, INCLUDE_NULL_VALUES"),
             R"([{"id":1,"extra":{"note":null}}])");
}

TEST(json_root_and_without_array_wrapper)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT id FROM customer ORDER BY id FOR JSON PATH, ROOT('customers')"),
             R"({"customers":[{"id":1},{"id":2}]})");
    CHECK_EQ(json(db, "SELECT id FROM customer ORDER BY id FOR JSON PATH, ROOT"),
             R"({"root":[{"id":1},{"id":2}]})");
    CHECK_EQ(json(db, "SELECT id FROM customer WHERE id = 2"
                      " FOR JSON PATH, WITHOUT_ARRAY_WRAPPER"),
             R"({"id":2})");
    CHECK_THROWS(db.exec("SELECT id FROM customer FOR JSON PATH, ROOT, WITHOUT_ARRAY_WRAPPER"),
                 sql::ErrorCode::InvalidArgument);
}

TEST(json_empty_result)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT id FROM customer WHERE id = 99 FOR JSON AUTO"), "[]");
    CHECK_EQ(json(db, "SELECT id FROM customer WHERE id = 99 FOR JSON PATH, ROOT('c')"),
             R"({"c":[]})");
    CHECK_EQ(json(db, "SELECT id FROM customer WHERE id = 99"
                      " FOR JSON PATH, WITHOUT_ARRAY_WRAPPER"),
             "");
}

TEST(json_respects_order_limit_and_offset)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT item FROM ordered ORDER BY item DESC LIMIT 2 FOR JSON AUTO"),
             R"([{"item":"rope"},{"item":"kite"}])");
    CHECK_EQ(json(db, "SELECT item FROM ordered ORDER BY item LIMIT 1 OFFSET 1 FOR JSON AUTO"),
             R"([{"item":"kite"}])");
}

TEST(json_aggregates_and_expressions_stay_at_the_top)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_EQ(json(db, "SELECT c.name, SUM(o.qty) AS total"
                      " FROM customer c INNER JOIN ordered o ON o.customer_id = c.id"
                      " GROUP BY c.name ORDER BY c.name FOR JSON PATH"),
             R"([{"name":"Ada","total":7},{"name":"Bo","total":1}])");
}

TEST(json_escapes_and_encodes_every_type)
{
    tst::Scratch scratch("json");
    sql::Database db = scratch.open();
    db.exec("CREATE TABLE t (i INTEGER, r REAL, d DATETIME, s TEXT, b BLOB)");
    db.exec("INSERT INTO t VALUES (?, ?, ?, ?, ?)",
            {sql::Value(-7), sql::Value(0.1), sql::Value(sql::parseDatetime("2024-01-02 03:04:05")),
             sql::Value(std::string("a\"b\\c\nd\te\x01?")), sql::Value::blob("hi!", 3)});

    CHECK_EQ(json(db, "SELECT i, r, d, s, b FROM t FOR JSON AUTO"),
             R"([{"i":-7,"r":0.1,"d":"2024-01-02T03:04:05Z",)"
             R"("s":"a\"b\\c\nd\te\u0001?","b":"aGkh"}])");
}

TEST(json_datetime_keeps_microseconds)
{
    tst::Scratch scratch("json");
    sql::Database db = scratch.open();
    db.exec("CREATE TABLE t (d DATETIME)");
    db.exec("INSERT INTO t VALUES ('2024-06-05T01:02:03.000450')");

    CHECK_EQ(json(db, "SELECT d FROM t FOR JSON AUTO"),
             R"([{"d":"2024-06-05T01:02:03.000450Z"}])");
}

TEST(json_blob_padding)
{
    tst::Scratch scratch("json");
    sql::Database db = scratch.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, b BLOB)");
    db.exec("INSERT INTO t VALUES (1, x''), (2, x'61'), (3, x'6162'), (4, x'616263')");

    CHECK_EQ(json(db, "SELECT b FROM t ORDER BY id FOR JSON AUTO"),
             R"([{"b":""},{"b":"YQ=="},{"b":"YWI="},{"b":"YWJj"}])");
}

TEST(json_bad_clause_is_a_syntax_error)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);

    CHECK_THROWS(db.exec("SELECT id FROM customer FOR JSON"), sql::ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT id FROM customer FOR JSON TREE"), sql::ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT id FROM customer FOR XML AUTO"), sql::ErrorCode::SyntaxError);
    CHECK_THROWS(db.exec("SELECT id FROM customer FOR JSON PATH, LOUD"),
                 sql::ErrorCode::SyntaxError);
}

TEST(json_writer_renders_a_plain_result)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);
    const sql::Result rows = db.exec("SELECT id, name FROM customer ORDER BY id");

    CHECK_EQ(sql::toJson(rows), R"([{"id":1,"name":"Ada"},{"id":2,"name":"Bo"}])");
    CHECK_EQ(sql::toJson(rows, sql::JsonOptions().mode(sql::JsonMode::Path).root("c")),
             R"({"c":[{"id":1,"name":"Ada"},{"id":2,"name":"Bo"}]})");
}

TEST(json_writer_takes_paths_instead_of_column_names)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);
    const sql::Result rows = db.exec("SELECT id, name FROM customer WHERE id = 1");

    sql::JsonWriter writer(sql::JsonOptions().mode(sql::JsonMode::Path));
    const std::vector<std::string> paths = {"key", "who.name"};
    CHECK_EQ(std::string(writer.write(rows, paths)), R"([{"key":1,"who":{"name":"Ada"}}])");
}

TEST(json_writer_measures_exactly_and_reuses_its_buffer)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);
    const sql::Result rows = db.exec("SELECT id, name, note FROM customer ORDER BY id");

    sql::JsonWriter writer;
    CHECK_EQ(writer.measure(rows), writer.write(rows).size());

    // The buffer settles after the first pass, which is the property a
    // long-running caller depends on.
    const std::string first(writer.write(rows));
    const std::size_t settled = writer.capacity();
    CHECK(settled >= first.size());
    for (int i = 0; i < 100; ++i)
        CHECK_EQ(std::string(writer.write(rows)), std::string(first));
    CHECK_EQ(writer.capacity(), settled);

    writer.compact();
    CHECK(writer.capacity() < settled || settled == 0);
}

TEST(json_writer_appends_without_clearing)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);
    const sql::Result rows = db.exec("SELECT id FROM customer WHERE id = 1");

    std::string out = "prefix:";
    sql::appendJson(rows, out);
    CHECK_EQ(out, std::string(R"(prefix:[{"id":1}])"));
}

TEST(json_writer_follows_a_change_of_shape)
{
    tst::Scratch scratch("json");
    sql::Database db = shop(scratch);
    sql::JsonWriter writer;

    // Same writer, different column names: the cached plan has to be dropped.
    CHECK_EQ(std::string(writer.write(db.exec("SELECT id FROM customer WHERE id = 1"))),
             R"([{"id":1}])");
    CHECK_EQ(std::string(writer.write(db.exec("SELECT name FROM customer WHERE id = 1"))),
             R"([{"name":"Ada"}])");
    CHECK_EQ(std::string(writer.write(db.exec("SELECT id, name FROM customer WHERE id = 1"))),
             R"([{"id":1,"name":"Ada"}])");
    CHECK_EQ(std::string(writer.write(db.exec("SELECT id FROM customer WHERE id = 1"))),
             R"([{"id":1}])");
}

TEST(json_for_is_reserved_but_quoting_still_works)
{
    tst::Scratch scratch("json");
    sql::Database db = scratch.open();
    db.exec("CREATE TABLE t (\"for\" INTEGER, \"json\" TEXT)");
    db.exec("INSERT INTO t VALUES (1, 'x')");

    CHECK_EQ(json(db, "SELECT \"for\", \"json\" FROM t FOR JSON AUTO"),
             R"([{"for":1,"json":"x"}])");
}

int main()
{
    return tst::runAll("json");
}
