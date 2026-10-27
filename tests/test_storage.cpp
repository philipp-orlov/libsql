// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// PRAGMA storage_*: the store underneath, as rows (src/internal/pragma_storage.cpp).

#include <cstdio>
#include <filesystem>

#include "sql_test.hpp"

using namespace sql;

namespace {

void fill(Database& db)
{
    db.exec("CREATE TABLE orders(id INTEGER PRIMARY KEY, customer TEXT, note TEXT)");
    db.exec("CREATE INDEX orders_customer ON orders(customer)");
    db.exec("CREATE TABLE tags(name TEXT PRIMARY KEY, n INTEGER) WITHOUT ROWID");
    db.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 2000) "
            "INSERT INTO orders SELECT n, 'customer ' || (n % 50), 'note ' || n FROM c");
    db.exec("INSERT INTO orders VALUES (5000, 'big', hex(randomblob(9000)))");  // a value on an overflow run
    db.exec("INSERT INTO tags VALUES ('a', 1), ('b', 2)");
    db.exec("DELETE FROM orders WHERE id % 3 = 0");
}

}  // namespace

TEST(storage_trees_name_the_tables_and_indexes)
{
    tst::Scratch s("storage");
    Database db = s.open();
    fill(db);
    const Result trees = db.exec("SELECT kind, object, parent, entries FROM pragma_storage_trees() "
                                 "WHERE kind IN ('table', 'index') ORDER BY kind, object");
    CHECK_EQ(tst::flatten(trees), std::string("index|orders_customer|orders|1335;table|orders|NULL|1335;table|tags|NULL|2"));
    // the sub-database names and roles of the store itself
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM pragma_storage_trees() WHERE role IN ('free', 'main', 'catalog')")),
             std::string("3"));
    CHECK_EQ(tst::only(db.exec("SELECT sum(leaf_pages + branch_pages + overflow_pages) > 10 FROM pragma_storage_trees()")),
             std::string("1"));
}

TEST(storage_map_covers_every_page_once)
{
    tst::Scratch s("storage");
    Database db = s.open();
    fill(db);
    const std::int64_t used = db.exec("PRAGMA page_count")[0][0].toInteger();
    CHECK_EQ(tst::only(db.exec("SELECT sum(count) FROM pragma_storage_map()")), std::to_string(used));
    CHECK_EQ(tst::only(db.exec("SELECT first FROM pragma_storage_map() LIMIT 1")), std::string("0"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM pragma_storage_map() WHERE kind = 'unknown'")), std::string("0"));
    CHECK_EQ(tst::only(db.exec("SELECT sum(count) FROM pragma_storage_map() WHERE kind = 'free'")),
             tst::only(db.exec("PRAGMA freelist_count")));
    // each tree's pages in the map are the pages its row says it has
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM pragma_storage_trees() t WHERE t.leaf_pages <> "
                               "(SELECT coalesce(sum(count), 0) FROM pragma_storage_map() m WHERE m.tree = t.id AND m.kind = 'leaf')")),
             std::string("0"));
    CHECK(db.exec("SELECT 1 FROM pragma_storage_map() WHERE kind = 'overflow'").size() > 0);
}

TEST(storage_pages_and_nodes_walk_a_tree)
{
    tst::Scratch s("storage");
    Database db = s.open();
    fill(db);
    const std::int64_t root = db.exec("SELECT root FROM pragma_storage_trees() WHERE object = 'orders'")[0][0].toInteger();
    const Result page = db.exec("SELECT kind, nkeys, entries, hash_ok, header_ok, stored_hash = computed_hash FROM pragma_storage_page(" +
                                std::to_string(root) + ")");
    CHECK_EQ(page[0][0].toText(), std::string("branch"));
    CHECK_EQ(page[0][2].toInteger(), std::int64_t(1335));
    CHECK_EQ(page[0][3].toInteger(), std::int64_t(1));
    CHECK_EQ(page[0][4].toInteger(), std::int64_t(1));
    CHECK_EQ(page[0][5].toInteger(), std::int64_t(1));
    // children add up to the entries
    CHECK_EQ(tst::only(db.exec("SELECT sum(count) FROM pragma_storage_nodes(" + std::to_string(root) + ")")), std::string("1335"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM pragma_storage_nodes(" + std::to_string(root) + ") WHERE child < 2")),
             std::string("0"));
    // a leaf: rowid keys and decoded records
    const Result leaf = db.exec(
        "SELECT storage_rowid(key), storage_row(value) FROM pragma_storage_nodes((SELECT child FROM pragma_storage_nodes(" +
        std::to_string(root) + ") LIMIT 1)) LIMIT 1");
    CHECK_EQ(leaf[0][0].toInteger(), std::int64_t(1));
    CHECK_EQ(leaf[0][1].toText(), std::string("(NULL, 'customer 1', 'note 1')"));
    // a big value is flagged and its run is found
    const Result big = db.exec("SELECT value_bytes, overflow FROM pragma_storage_nodes((SELECT first FROM pragma_storage_map() "
                               "WHERE kind = 'leaf' AND tree = (SELECT id FROM pragma_storage_trees() WHERE object = 'orders') "
                               "ORDER BY first DESC LIMIT 1)) WHERE big ORDER BY idx DESC LIMIT 1");
    CHECK_EQ(big.size(), std::size_t(1));
    CHECK(big[0][0].toInteger() >= 9000);
    CHECK_EQ(db.exec("SELECT kind, hash_ok FROM pragma_storage_page(" + std::to_string(big[0][1].toInteger()) + ")")[0][0].toText(),
             std::string("overflow"));
    // raw bytes are one page
    CHECK_EQ(tst::only(db.exec("SELECT length(data) FROM pragma_storage_bytes(" + std::to_string(root) + ")")), std::string("4096"));
}

TEST(storage_meta_stats_and_free_list)
{
    tst::Scratch s("storage");
    Database db = s.open();
    fill(db);
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM pragma_storage_meta()")), std::string("2"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM pragma_storage_meta() WHERE valid AND checksum = computed_checksum")), std::string("2"));
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM pragma_storage_meta() WHERE current")), std::string("1"));
    CHECK_EQ(tst::only(db.exec("SELECT value FROM pragma_storage_stats() WHERE name = 'page_size'")), std::string("4096"));
    CHECK_EQ(tst::only(db.exec("SELECT value FROM pragma_storage_stats() WHERE name = 'free_pages'")),
             tst::only(db.exec("PRAGMA freelist_count")));
    CHECK_EQ(tst::only(db.exec("SELECT sum(pages) FROM pragma_storage_free()")), tst::only(db.exec("PRAGMA freelist_count")));
    CHECK(db.exec("PRAGMA pragma_list").size() > 60);
}

TEST(storage_sees_only_what_is_committed)
{
    tst::Scratch s("storage");
    Database db = s.open();
    fill(db);
    const std::string before = tst::only(db.exec("SELECT entries FROM pragma_storage_trees() WHERE object = 'tags'"));
    db.exec("BEGIN");
    db.exec("INSERT INTO tags VALUES ('c', 3)");
    CHECK_EQ(tst::only(db.exec("SELECT entries FROM pragma_storage_trees() WHERE object = 'tags'")), before);
    db.exec("COMMIT");
    CHECK_EQ(tst::only(db.exec("SELECT entries FROM pragma_storage_trees() WHERE object = 'tags'")), std::string("3"));
}

TEST(storage_check_finds_a_damaged_page)
{
    tst::Scratch s("storage");
    std::int64_t victim = 0;
    {
        Database db = s.open();
        fill(db);
        CHECK_EQ(tst::only(db.exec("SELECT result FROM pragma_storage_check()")), std::string("ok"));
        victim = db.exec("SELECT first FROM pragma_storage_map() WHERE kind = 'leaf' AND tree = "
                         "(SELECT id FROM pragma_storage_trees() WHERE object = 'orders') ORDER BY first LIMIT 1")[0][0].toInteger();
    }
    {
        std::FILE* f = std::fopen(s.file().string().c_str(), "r+b");
        CHECK(f != nullptr);
        std::fseek(f, long(victim * 4096 + 2000), SEEK_SET);
        const char junk[4] = {'\xde', '\xad', '\xbe', '\xef'};
        std::fwrite(junk, 1, 4, f);
        std::fclose(f);
    }
    Database db = s.open();
    CHECK_EQ(tst::only(db.exec("SELECT hash_ok FROM pragma_storage_page(" + std::to_string(victim) + ")")), std::string("0"));
    const Result r = db.exec("SELECT result, page FROM pragma_storage_check() WHERE result = 'page'");
    CHECK(r.size() >= 1);
    CHECK_EQ(r[0][1].toInteger(), victim);
    CHECK(db.exec("SELECT 1 FROM pragma_storage_check() WHERE result = 'integrity'").size() == 1);
}

TEST(storage_arguments_are_checked)
{
    tst::Scratch s("storage");
    Database db = s.open();
    fill(db);
    CHECK_THROWS(db.exec("PRAGMA storage_page"), ErrorCode::Runtime);
    CHECK_THROWS(db.exec("SELECT * FROM pragma_storage_page(-1)"), ErrorCode::Runtime);
    CHECK_THROWS(db.exec("SELECT * FROM pragma_storage_page(99999999)"), ErrorCode::Runtime);
    CHECK_THROWS(db.exec("SELECT * FROM pragma_storage_nodes(0)"), ErrorCode::Runtime);
    CHECK_EQ(tst::only(db.exec("SELECT storage_row(x'ff') IS NULL")), std::string("1"));
    CHECK_EQ(tst::only(db.exec("SELECT storage_rowid(x'0102') IS NULL")), std::string("1"));
    CHECK_EQ(tst::only(db.exec("SELECT storage_row(NULL) IS NULL")), std::string("1"));
}

int main()
{
    return tst::runAll("storage");
}
