// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// WebApi: the JSON contract of the browser front end, tested without a socket.

#include <stdexcept>
#include <string>

#include "sql_test.hpp"
#include "web_api.hpp"

namespace {

using json::Json;
using sqlweb::Reply;
using sqlweb::WebApi;

Json parse(const std::string& text)
{
    Json out;
    std::string error;
    if (!Json::parse(text, out, error))
        throw std::runtime_error("bad test JSON: " + error);
    return out;
}

const Json& member(const Json& object, const char* key)
{
    const Json* found = object.find(key);
    if (!found)
        tst::fail(__FILE__, __LINE__, std::string("no member \"") + key + "\" in " + object.dump());
    return *found;
}

std::string at(const Reply& reply, const char* key)
{
    return member(reply.body, key).dump();
}

std::string error(const Reply& reply)
{
    return reply.body.find("error") ? reply.body.find("error")->asString() : std::string();
}

/// A database with a few tables, the API on top of it.
struct Fixture
{
    tst::Scratch scratch{"web-api"};
    sql::Database db = scratch.open();
    WebApi api{db, false};

    Fixture()
    {
        db.exec(
            "CREATE TABLE person(id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, note "
            "TEXT);"
            "CREATE TABLE tag(label TEXT, weight REAL);"
            "CREATE TABLE pair(a TEXT, b INTEGER, c TEXT, PRIMARY KEY (a, b)) WITHOUT ROWID;"
            "CREATE INDEX person_name ON person(name);"
            "CREATE VIEW adults AS SELECT id, name FROM person WHERE age >= 18;"
            "CREATE TRIGGER person_added AFTER INSERT ON person BEGIN SELECT 1; END;"
            "INSERT INTO person(name, age, note) VALUES ('Ada', 36, 'first'), ('Bob', 17, NULL), "
            "('Cyd', 52, 'third'), ('Dee', 18, 'fourth');"
            "INSERT INTO tag VALUES ('red', 1.5), ('green', 2.5);"
            "INSERT INTO pair VALUES ('x', 1, 'one'), ('x', 2, 'two');");
    }

    Reply browse(const std::string& request) { return api.browse(parse(request)); }
    Reply run(const std::string& text) { return api.execute(Json{{"sql", text}}); }
    std::string names(const std::string& request)
    {
        const Reply reply = browse(request);
        CHECK_EQ(reply.status, 200);
        std::string out;
        for (const Json& row : member(reply.body, "rows").asArray())
            out += (out.empty() ? "" : ",") + row[1].asString();
        return out;
    }
};

}  // namespace

TEST(info_describes_the_database)
{
    Fixture f;
    const Reply reply = f.api.info();
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(member(reply.body, "path").asString(), f.db.path().string());
    CHECK_EQ(at(reply, "readOnly"), std::string("false"));
    CHECK_EQ(at(reply, "inTransaction"), std::string("false"));
}

TEST(info_lists_the_functions_the_editor_offers)
{
    Fixture f;
    bool sawLower = false;
    bool sawCount = false;
    const Reply reply = f.api.info();
    for (const Json& name : member(reply.body, "functions").asArray()) {
        sawLower = sawLower || name.asString() == "lower";
        sawCount = sawCount || name.asString() == "count";
    }
    CHECK(sawLower);
    CHECK(sawCount);
}

TEST(schema_lists_tables_views_indexes_and_triggers)
{
    Fixture f;
    const Reply reply = f.api.schema();
    CHECK_EQ(reply.status, 200);

    const auto& tables = member(reply.body, "tables").asArray();
    CHECK_EQ(tables.size(), std::size_t(3));
    CHECK_EQ(member(tables[0], "name").asString(), std::string("person"));
    const auto& columns = member(tables[0], "columns").asArray();
    CHECK_EQ(columns.size(), std::size_t(4));
    CHECK_EQ(member(columns[1], "name").asString(), std::string("name"));
    CHECK_EQ(member(columns[1], "type").asString(), std::string("TEXT"));
    CHECK_EQ(member(columns[1], "notNull").dump(), std::string("true"));
    CHECK_EQ(member(columns[0], "primaryKey").dump(), std::string("true"));
    CHECK_EQ(member(tables[2], "withoutRowid").dump(), std::string("true"));

    const auto& views = member(reply.body, "views").asArray();
    CHECK_EQ(views.size(), std::size_t(1));
    CHECK_EQ(member(views[0], "name").asString(), std::string("adults"));
    CHECK_EQ(member(views[0], "columns").asArray().size(), std::size_t(2));

    const auto& indexes = member(reply.body, "indexes").asArray();
    CHECK_EQ(indexes.size(), std::size_t(1));
    CHECK_EQ(member(indexes[0], "name").asString(), std::string("person_name"));
    CHECK_EQ(member(indexes[0], "table").asString(), std::string("person"));
    CHECK_EQ(member(indexes[0], "columns").dump(), std::string("[\"name\"]"));

    const auto& triggers = member(reply.body, "triggers").asArray();
    CHECK_EQ(triggers.size(), std::size_t(1));
    CHECK_EQ(member(triggers[0], "table").asString(), std::string("person"));
}

TEST(browse_returns_rows_keys_and_the_total)
{
    Fixture f;
    const Reply reply = f.browse(R"({"table": "person", "limit": 2})");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(at(reply, "total"), std::string("4"));
    CHECK_EQ(at(reply, "rows"), std::string("[[1,\"Ada\",36,\"first\"],[2,\"Bob\",17,null]]"));
    CHECK_EQ(at(reply, "keys"), std::string("[[1],[2]]"));
    CHECK_EQ(at(reply, "editable"), std::string("true"));
}

TEST(browse_pages_and_sorts)
{
    Fixture f;
    CHECK_EQ(f.names(R"({"table": "person", "offset": 2, "limit": 10})"), std::string("Cyd,Dee"));
    CHECK_EQ(f.names(R"({"table": "person", "sort": [{"column": "age", "descending": true}]})"),
             std::string("Cyd,Ada,Dee,Bob"));
    CHECK_EQ(
        f.names(
            R"({"table": "person", "sort": [{"column": "name", "descending": true}], "limit": 1})"),
        std::string("Dee"));
    // The page size is capped, so a client cannot ask for a whole table at once.
    CHECK_EQ(at(f.browse(R"({"table": "person", "limit": 99999})"), "limit"), std::string("1000"));
}

TEST(browse_filters_like_the_column_boxes_of_the_grid)
{
    Fixture f;
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"name": "b"}})"), std::string("Bob"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"note": "IR"}})"), std::string("Ada,Cyd"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"age": ">=36"}})"), std::string("Ada,Cyd"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"age": "<18"}})"), std::string("Bob"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"age": "=18"}})"), std::string("Dee"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"age": "<>18", "name": "d"}})"),
             std::string("Ada,Cyd"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"note": "=NULL"}})"), std::string("Bob"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"note": "<>NULL"}})"),
             std::string("Ada,Cyd,Dee"));
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"name": "  "}})"),
             std::string("Ada,Bob,Cyd,Dee"));
    // The total follows the filter, not the table.
    CHECK_EQ(at(f.browse(R"({"table": "person", "filters": {"age": ">=36"}})"), "total"),
             std::string("2"));
}

TEST(a_filter_is_text_to_match_and_never_sql)
{
    Fixture f;
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"name": "' OR 1=1 --"}})"), std::string());
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"name": "%"}})"), std::string());
    CHECK_EQ(f.names(R"({"table": "person", "filters": {"name": "="}})"), std::string());
}

TEST(browse_rejects_names_that_do_not_exist)
{
    Fixture f;
    CHECK_EQ(f.browse(R"({"table": "nope"})").status, 400);
    CHECK_EQ(f.browse(R"({"table": "person", "sort": [{"column": "nope"}]})").status, 400);
    CHECK_EQ(f.browse(R"({"table": "person", "filters": {"nope": "x"}})").status, 400);
    CHECK_EQ(f.browse(R"({"table": "person; DROP TABLE person"})").status, 400);
    CHECK_EQ(f.browse(R"({"offset": 1})").status, 400);
    CHECK_EQ(f.names(R"({"table": "person"})"), std::string("Ada,Bob,Cyd,Dee"));
}

TEST(browse_reads_a_view_but_does_not_edit_it)
{
    Fixture f;
    const Reply reply = f.browse(R"({"table": "adults"})");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(at(reply, "editable"), std::string("false"));
    CHECK_EQ(at(reply, "view"), std::string("true"));
    CHECK_EQ(at(reply, "total"), std::string("3"));
    CHECK_EQ(f.api.deleteRows(parse(R"({"table": "adults", "keys": [[1]]})")).status, 400);
}

TEST(browse_addresses_the_rows_of_a_table_without_rowid_by_primary_key)
{
    Fixture f;
    const Reply reply = f.browse(R"({"table": "pair"})");
    CHECK_EQ(at(reply, "keys"), std::string("[[\"x\",1],[\"x\",2]]"));
    CHECK_EQ(at(reply, "editable"), std::string("true"));
}

TEST(an_edit_stays_pending_until_it_is_committed)
{
    Fixture f;
    const Reply update =
        f.api.updateRow(parse(R"({"table": "person", "key": [1], "set": {"name": "Ann"}})"));
    CHECK_EQ(update.status, 200);
    CHECK_EQ(at(update, "inTransaction"), std::string("true"));
    CHECK(f.db.inTransaction());
    CHECK_EQ(f.names(R"({"table": "person", "limit": 1})"), std::string("Ann"));

    const Reply commit = f.api.commit();
    CHECK_EQ(commit.status, 200);
    CHECK_EQ(at(commit, "inTransaction"), std::string("false"));
    CHECK_EQ(tst::only(f.db.exec("SELECT name FROM person WHERE id = 1")), std::string("Ann"));
}

TEST(a_reverted_edit_leaves_no_trace)
{
    Fixture f;
    CHECK_EQ(f.api
                 .updateRow(
                     parse(R"({"table": "person", "key": [1], "set": {"name": "Ann", "age": 1}})"))
                 .status,
             200);
    CHECK_EQ(f.api.insertRow(parse(R"({"table": "person", "values": {"name": "Eve"}})")).status,
             200);
    CHECK_EQ(f.api.deleteRows(parse(R"({"table": "person", "keys": [[2], [3]]})")).status, 200);
    CHECK_EQ(f.names(R"({"table": "person"})"), std::string("Ann,Dee,Eve"));

    const Reply revert = f.api.revert();
    CHECK_EQ(at(revert, "inTransaction"), std::string("false"));
    CHECK_EQ(f.names(R"({"table": "person"})"), std::string("Ada,Bob,Cyd,Dee"));
    CHECK_EQ(tst::only(f.db.exec("SELECT age FROM person WHERE id = 1")), std::string("36"));
}

TEST(insert_takes_the_given_columns_and_the_defaults_for_the_rest)
{
    Fixture f;
    const Reply reply =
        f.api.insertRow(parse(R"({"table": "person", "values": {"name": "Eve", "age": 29}})"));
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(at(reply, "changes"), std::string("1"));
    CHECK_EQ(at(reply, "lastInsertId"), std::string("5"));
    CHECK_EQ(tst::flatten(f.db.exec("SELECT name, age, note FROM person WHERE id = 5")),
             std::string("Eve|29|NULL"));

    // No values at all: every column takes its default, so a NOT NULL column without one refuses.
    CHECK_EQ(f.api.insertRow(parse(R"({"table": "tag", "values": {}})")).status, 200);
    CHECK_EQ(f.api.insertRow(parse(R"({"table": "person", "values": {}})")).status, 400);
    CHECK_EQ(tst::only(f.db.exec("SELECT count(*) FROM tag")), std::string("3"));
}

TEST(a_failed_edit_opens_no_transaction)
{
    Fixture f;
    const Reply reply = f.api.insertRow(parse(R"({"table": "person", "values": {"age": 3}})"));
    CHECK_EQ(reply.status, 400);
    CHECK_EQ(at(reply, "code"), std::string("\"constraint violation\""));
    CHECK_EQ(at(reply, "inTransaction"), std::string("false"));
    CHECK(!f.db.inTransaction());
}

TEST(update_sets_null_and_other_types_and_checks_the_key)
{
    Fixture f;
    CHECK_EQ(f.api
                 .updateRow(parse(
                     R"({"table": "person", "key": [1], "set": {"note": null, "age": "40"}})"))
                 .status,
             200);
    CHECK_EQ(tst::flatten(f.db.exec("SELECT age, note FROM person WHERE id = 1")),
             std::string("40|NULL"));

    // A row that is gone is an error, not a silent no-op; the failed call keeps the earlier edit.
    const Reply gone =
        f.api.updateRow(parse(R"({"table": "person", "key": [99], "set": {"age": 1}})"));
    CHECK_EQ(gone.status, 400);
    CHECK_EQ(at(gone, "inTransaction"), std::string("true"));

    CHECK_EQ(f.api.updateRow(parse(R"({"table": "person", "key": [1], "set": {}})")).status, 400);
    CHECK_EQ(
        f.api.updateRow(parse(R"({"table": "person", "key": [1, 2], "set": {"age": 1}})")).status,
        400);
    CHECK_EQ(
        f.api.updateRow(parse(R"({"table": "person", "key": [1], "set": {"nope": 1}})")).status,
        400);
    CHECK_EQ(
        f.api.updateRow(parse(R"({"table": "person", "key": [1], "set": {"age": [1]}})")).status,
        400);
}

TEST(update_and_delete_find_rows_of_a_table_without_rowid_by_key)
{
    Fixture f;
    CHECK_EQ(f.api.updateRow(parse(R"({"table": "pair", "key": ["x", 2], "set": {"c": "deux"}})"))
                 .status,
             200);
    CHECK_EQ(tst::flatten(f.db.exec("SELECT a, b, c FROM pair ORDER BY b")),
             std::string("x|1|one;x|2|deux"));
    const Reply deleted = f.api.deleteRows(parse(R"({"table": "pair", "keys": [["x", 1]]})"));
    CHECK_EQ(at(deleted, "changes"), std::string("1"));
    CHECK_EQ(tst::only(f.db.exec("SELECT count(*) FROM pair")), std::string("1"));
}

TEST(delete_counts_the_rows_it_removed)
{
    Fixture f;
    const Reply reply = f.api.deleteRows(parse(R"({"table": "person", "keys": [[1], [3], [42]]})"));
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(at(reply, "changes"), std::string("2"));
    CHECK_EQ(f.api.deleteRows(parse(R"({"table": "person", "keys": "all"})")).status, 400);
}

TEST(execute_returns_columns_and_rows)
{
    Fixture f;
    const Reply reply = f.run("SELECT name, age FROM person WHERE age > 30 ORDER BY id");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(at(reply, "columns"), std::string("[\"name\",\"age\"]"));
    CHECK_EQ(at(reply, "rows"), std::string("[[\"Ada\",36],[\"Cyd\",52]]"));
    CHECK_EQ(at(reply, "rowCount"), std::string("2"));
    CHECK_EQ(at(reply, "truncated"), std::string("false"));
    CHECK(member(reply.body, "elapsedMs").asDouble() >= 0);
}

TEST(execute_runs_a_batch_and_answers_with_its_last_statement)
{
    Fixture f;
    const Reply reply = f.run("INSERT INTO tag VALUES ('blue', 3); SELECT count(*) AS n FROM tag");
    CHECK_EQ(at(reply, "columns"), std::string("[\"n\"]"));
    CHECK_EQ(at(reply, "rows"), std::string("[[3]]"));
}

TEST(execute_stops_the_rows_at_max_rows)
{
    Fixture f;
    const Reply reply = f.api.execute(parse(R"({"sql": "SELECT id FROM person", "maxRows": 3})"));
    CHECK_EQ(at(reply, "rows"), std::string("[[1],[2],[3]]"));
    CHECK_EQ(at(reply, "truncated"), std::string("true"));
    CHECK_EQ(at(reply, "rowCount"), std::string("4"));
}

TEST(execute_leaves_a_write_pending_and_a_read_not)
{
    Fixture f;
    const Reply read = f.run("SELECT * FROM person");
    CHECK_EQ(at(read, "inTransaction"), std::string("false"));

    const Reply write = f.run("UPDATE person SET age = age + 1");
    CHECK_EQ(at(write, "changes"), std::string("4"));
    CHECK_EQ(at(write, "inTransaction"), std::string("true"));
    CHECK_EQ(at(f.api.revert(), "inTransaction"), std::string("false"));
    CHECK_EQ(tst::only(f.db.exec("SELECT age FROM person WHERE id = 1")), std::string("36"));

    const Reply schema = f.run("CREATE TABLE extra(x)");
    CHECK_EQ(at(schema, "inTransaction"), std::string("true"));
    f.api.revert();
    CHECK_EQ(member(f.api.schema().body, "tables").asArray().size(), std::size_t(3));
}

TEST(execute_that_fails_undoes_what_it_began)
{
    Fixture f;
    const Reply reply = f.run("INSERT INTO tag VALUES ('blue', 3); SELECT * FROM missing");
    CHECK_EQ(reply.status, 400);
    CHECK_EQ(at(reply, "inTransaction"), std::string("false"));
    CHECK_EQ(tst::only(f.db.exec("SELECT count(*) FROM tag")), std::string("2"));
    CHECK_EQ(f.run("SELEKT 1").status, 400);
    CHECK_EQ(f.api.execute(parse(R"({"query": "SELECT 1"})")).status, 400);
    CHECK_EQ(f.api.execute(parse(R"({"sql": 5})")).status, 400);
}

TEST(execute_obeys_transaction_statements_and_pragmas_as_written)
{
    Fixture f;
    CHECK_EQ(at(f.run("BEGIN"), "inTransaction"), std::string("true"));
    f.run("DELETE FROM tag");
    CHECK_EQ(at(f.run("ROLLBACK"), "inTransaction"), std::string("false"));
    CHECK_EQ(tst::only(f.db.exec("SELECT count(*) FROM tag")), std::string("2"));

    const Reply pragma = f.run("  -- comment\n PRAGMA foreign_keys = 1");
    CHECK_EQ(pragma.status, 200);
    CHECK_EQ(at(pragma, "inTransaction"), std::string("false"));
}

TEST(cells_keep_their_type_across_json)
{
    Fixture f;
    const Reply reply = f.run(
        "SELECT NULL, 7, 1.5, 'text', x'00ff10', 9007199254740993, -9007199254740993, "
        "'caf\xc3\xa9'");
    CHECK_EQ(
        at(reply, "rows"),
        std::string("[[null,7,1.5,\"text\",{\"type\":\"blob\",\"size\":3,\"value\":\"00ff10\"},"
                    "{\"type\":\"integer\",\"value\":\"9007199254740993\"},"
                    "{\"type\":\"integer\",\"value\":\"-9007199254740993\"},\"caf\xc3\xa9\"]]"));
}

TEST(a_blob_cell_is_previewed_not_sent_whole)
{
    Fixture f;
    const Reply reply = f.run("SELECT zeroblob(1000)");
    const Json& cell = member(reply.body, "rows")[0][0];
    CHECK_EQ(member(cell, "size").dump(), std::string("1000"));
    CHECK_EQ(member(cell, "value").asString().size(), std::size_t(64));
}

TEST(text_that_is_not_utf8_is_sent_as_a_blob)
{
    Fixture f;
    CHECK_EQ(f.api.insertRow(parse(R"({"table": "tag", "values": {"label": "x"}})")).status, 200);
    f.db.exec("UPDATE tag SET label = CAST(x'80ff' AS TEXT) WHERE label = 'x'");
    const Reply reply = f.browse(R"({"table": "tag", "filters": {"weight": "=NULL"}})");
    CHECK_EQ(at(reply, "rows"),
             std::string("[[{\"type\":\"blob\",\"size\":2,\"value\":\"80ff\"},null]]"));
}

TEST(an_integer_beyond_double_precision_survives_as_a_key)
{
    Fixture f;
    f.db.exec("INSERT INTO person(id, name) VALUES (9007199254740993, 'Big')");
    const Reply reply = f.browse(R"({"table": "person", "filters": {"name": "Big"}})");
    CHECK_EQ(at(reply, "keys"),
             std::string("[[{\"type\":\"integer\",\"value\":\"9007199254740993\"}]]"));

    const Reply update = f.api.updateRow(
        parse(R"({"table": "person", "key": [{"type": "integer", "value": "9007199254740993"}],)"
              R"( "set": {"name": "Huge"}})"));
    CHECK_EQ(update.status, 200);
    CHECK_EQ(tst::only(f.db.exec("SELECT name FROM person WHERE id = 9007199254740993")),
             std::string("Huge"));
}

TEST(a_read_only_database_refuses_edits)
{
    tst::Scratch scratch("web-api-ro");
    {
        sql::Database db = scratch.open();
        db.exec("CREATE TABLE t(x); INSERT INTO t VALUES (1)");
    }
    sql::Database db = sql::Database::configure().readOnly().open(scratch.file());
    WebApi api(db, true);

    CHECK_EQ(at(api.info(), "readOnly"), std::string("true"));
    const Reply page = api.browse(parse(R"({"table": "t"})"));
    CHECK_EQ(page.status, 200);
    CHECK_EQ(at(page, "editable"), std::string("false"));
    CHECK_EQ(api.insertRow(parse(R"({"table": "t", "values": {"x": 2}})")).status, 400);
    CHECK_EQ(api.updateRow(parse(R"({"table": "t", "key": [1], "set": {"x": 2}})")).status, 400);
    CHECK_EQ(api.deleteRows(parse(R"({"table": "t", "keys": [[1]]})")).status, 400);
    CHECK_EQ(api.setPragma(parse(R"({"name": "foreign_keys", "value": 1})")).status, 400);
    CHECK_EQ(api.execute(Json{{"sql", "INSERT INTO t VALUES (3)"}}).status, 400);
    CHECK(!db.inTransaction());
}

TEST(pragmas_are_listed_with_their_kind_and_value)
{
    Fixture f;
    const Reply reply = f.api.pragmas();
    CHECK_EQ(reply.status, 200);
    bool sawForeignKeys = false;
    bool sawPageSize = false;
    for (const Json& pragma : member(reply.body, "pragmas").asArray()) {
        const std::string name = member(pragma, "name").asString();
        if (name == "foreign_keys") {
            sawForeignKeys = true;
            CHECK_EQ(member(pragma, "kind").asString(), std::string("boolean"));
            CHECK_EQ(member(pragma, "readOnly").dump(), std::string("false"));
            CHECK_EQ(member(pragma, "value").dump(), std::string("0"));
        }
        if (name == "page_size") {
            sawPageSize = true;
            CHECK_EQ(member(pragma, "readOnly").dump(), std::string("true"));
        }
    }
    CHECK(sawForeignKeys);
    CHECK(sawPageSize);
}

TEST(every_pragma_says_where_it_lives_and_what_it_does)
{
    Fixture f;
    const Reply reply = f.api.pragmas();
    for (const Json& pragma : member(reply.body, "pragmas").asArray()) {
        const std::string name = member(pragma, "name").asString();
        CHECK(!member(pragma, "description").asString().empty());
        const std::string scope = member(pragma, "scope").asString();
        CHECK(scope == "file" || scope == "session" || scope == "store");
        if (name == "user_version")
            CHECK_EQ(scope, std::string("file"));
        if (name == "foreign_keys")
            CHECK_EQ(scope, std::string("session"));
        if (name == "journal_mode") {
            CHECK_EQ(scope, std::string("store"));
            CHECK(member(pragma, "readOnly").dump() == "true");
            CHECK(member(pragma, "description").asString().find("no WAL file") !=
                  std::string::npos);
        }
    }
}

TEST(the_dump_rebuilds_the_database)
{
    Fixture f;
    f.db.exec("INSERT INTO person(name, age, note) VALUES ('O''Neil', NULL, 'it''s')");
    const Reply reply = f.api.dump();
    CHECK_EQ(reply.status, 200);

    tst::Scratch other("web-api-dump");
    sql::Database copy = other.open();
    copy.exec(member(reply.body, "sql").asString());
    CHECK_EQ(tst::flatten(copy.exec("SELECT id, name, age, note FROM person ORDER BY id")),
             tst::flatten(f.db.exec("SELECT id, name, age, note FROM person ORDER BY id")));
    CHECK_EQ(tst::flatten(copy.exec("SELECT a, b, c FROM pair ORDER BY b")),
             std::string("x|1|one;x|2|two"));
    CHECK_EQ(tst::only(copy.exec(
                 "SELECT count(*) FROM sqlite_schema WHERE type IN ('index', 'view', 'trigger')")),
             std::string("3"));
}

TEST(a_pragma_can_be_set_and_read_back)
{
    Fixture f;
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "foreign_keys", "value": 1})")).status, 200);
    CHECK_EQ(tst::only(f.db.exec("PRAGMA foreign_keys")), std::string("1"));
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "user_version", "value": 7})")).status, 200);
    CHECK_EQ(tst::only(f.db.exec("PRAGMA user_version")), std::string("7"));
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "temp_store", "value": 2})")).status, 200);
}

TEST(a_pragma_change_is_checked)
{
    Fixture f;
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "page_size", "value": 8192})")).status, 400);
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "writable_schema", "value": 1})")).status, 400);
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "foreign_keys", "value": 2})")).status, 400);
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "foreign_keys", "value": "on"})")).status, 400);
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "temp_store", "value": 9})")).status, 400);
    CHECK_EQ(
        f.api.setPragma(parse(R"({"name": "foreign_keys; DROP TABLE tag", "value": 1})")).status,
        400);

    f.api.updateRow(parse(R"({"table": "person", "key": [1], "set": {"age": 1}})"));
    CHECK_EQ(f.api.setPragma(parse(R"({"name": "foreign_keys", "value": 1})")).status, 409);
}

TEST(a_request_that_is_not_an_object_is_refused)
{
    Fixture f;
    CHECK_EQ(f.api.browse(parse("[1]")).status, 400);
    CHECK_EQ(f.api.execute(parse("null")).status, 400);
    CHECK_EQ(f.api.updateRow(parse("\"x\"")).status, 400);
    CHECK(!error(f.api.browse(parse("[1]"))).empty());
}

int main()
{
    return tst::runAll("web_api");
}
