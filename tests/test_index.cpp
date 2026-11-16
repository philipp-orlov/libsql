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

TEST(failingIndexExpressionLeavesNoRowBehind)
{
    tst::Scratch s("index-expr-fail");
    Database db = s.open();
    db.exec("CREATE TABLE t (x INTEGER)");
    db.exec("CREATE INDEX t_abs ON t (abs(x))");
    db.exec("CREATE TABLE p (x INTEGER, y INTEGER)");
    db.exec("CREATE INDEX p_part ON p (y) WHERE abs(x) > 0");
    Statement insertT = db.prepare("INSERT INTO t (x) VALUES (?)");
    Statement insertP = db.prepare("INSERT INTO p (x, y) VALUES (?, 1)");
    {
        // A single-row insert inside a larger transaction: abs() of INT64_MIN
        // fails while the index entry is computed.
        Transaction txn = db.begin();
        CHECK_THROWS(insertT.exec(txn, {Value(INT64_MIN)}), ErrorCode::Runtime);
        CHECK_THROWS(insertP.exec(txn, {Value(INT64_MIN)}), ErrorCode::Runtime);
        insertT.exec(txn, {Value(5)});
        txn.commit();
    }
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM t")), std::string("1"));
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM p")), std::string("0"));
    CHECK_EQ(tst::only(db.exec("PRAGMA integrity_check")), std::string("ok"));
}

TEST(keyOrderNeedsNoSort)
{
    tst::Scratch s("index-key-order");
    Database db = s.open();
    db.exec(
        "CREATE TABLE e (record_id INTEGER, slot INTEGER, v BLOB, PRIMARY KEY (record_id, slot))");
    db.exec("CREATE TABLE r (id INTEGER PRIMARY KEY, k INTEGER)");
    db.exec("CREATE INDEX r_k ON r (k)");
    db.exec("INSERT INTO e VALUES (1, 3, x'03'), (1, 1, x'01'), (2, 0, x'00'), (1, 2, x'02')");
    db.exec("INSERT INTO r VALUES (5, 2), (6, 1), (7, 2)");
    auto sorts = [&](const std::string& query) {
        bool temp = false;
        for (const Row& row : db.exec("EXPLAIN QUERY PLAN " + query))
            temp = temp || row[3].text() == "USE TEMP B-TREE FOR ORDER BY";
        return temp;
    };
    CHECK(!sorts("SELECT v FROM e WHERE record_id = 1 ORDER BY slot"));
    CHECK(!sorts("SELECT v FROM e WHERE record_id = 1 ORDER BY slot DESC LIMIT 1"));
    CHECK(!sorts("SELECT v FROM e WHERE record_id = 1 ORDER BY record_id, slot"));
    CHECK(!sorts("SELECT id FROM r WHERE k = 2 ORDER BY id"));
    CHECK(!sorts("SELECT id FROM r WHERE id > 5 ORDER BY id DESC"));
    CHECK(!sorts("SELECT k FROM r WHERE k > 0 ORDER BY k"));
    // These the key order does not deliver: a sort stays.
    CHECK(sorts("SELECT v FROM e WHERE record_id = 1 ORDER BY v"));
    CHECK(sorts("SELECT v FROM e WHERE record_id = 1 ORDER BY slot, v"));
    CHECK(sorts("SELECT v FROM e WHERE record_id IN (1, 2) ORDER BY slot"));
    CHECK(sorts("SELECT k FROM r WHERE k > 0 ORDER BY k DESC"));  // ties under k
    CHECK(sorts("SELECT v FROM e ORDER BY slot"));
    CHECK(sorts("SELECT slot FROM e WHERE record_id = 1 ORDER BY slot COLLATE NOCASE"));

    CHECK_EQ(tst::flatten(db.exec("SELECT hex(v) FROM e WHERE record_id = 1 ORDER BY slot")),
             std::string("01;02;03"));
    CHECK_EQ(tst::flatten(db.exec("SELECT slot FROM e WHERE record_id = 1 ORDER BY slot DESC")),
             std::string("3;2;1"));
    CHECK_EQ(tst::flatten(db.exec(
                 "SELECT slot FROM e WHERE record_id = 1 ORDER BY slot DESC LIMIT 1 OFFSET 1")),
             std::string("2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT id FROM r WHERE id > 5 ORDER BY id DESC")),
             std::string("7;6"));
    CHECK_EQ(tst::only(db.exec("SELECT MAX(slot) FROM e WHERE record_id = 1")), std::string("3"));
    CHECK_EQ(tst::only(db.exec("SELECT MIN(slot) FROM e WHERE record_id = 1")), std::string("1"));
    CHECK(db.exec("SELECT MAX(slot) FROM e WHERE record_id = 9")[0][0].isNull());
    CHECK_EQ(tst::only(db.exec("SELECT MAX(k) FROM r WHERE k < 2")), std::string("1"));
}

namespace {

/// The statement with its table read as written and with `NOT INDEXED`
/// after the table name `t`: the same rows, by probes and by a scan.
void probedAsScanned(Database& db, const std::string& sql, bool expectSearch = true)
{
    std::string scan = sql;
    const std::size_t at = scan.find(" t ");
    CHECK(at != std::string::npos);
    scan.insert(at + 2, " NOT INDEXED");
    const std::string a = tst::flatten(db.exec(sql + " ORDER BY 1, 2"));
    const std::string b = tst::flatten(db.exec(scan + " ORDER BY 1, 2"));
    CHECK(a == b);
    const std::string plan = tst::flatten(db.exec("EXPLAIN QUERY PLAN " + sql));
    CHECK_EQ(plan.find("SEARCH t") != std::string::npos, expectSearch);
}

}  // namespace

TEST(an_or_of_equalities_on_one_column_probes_like_an_in_list)
{
    tst::Scratch s("index");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, k, name TEXT COLLATE NOCASE, n INTEGER)");
    db.exec("CREATE INDEX t_k ON t (k)");
    db.exec("CREATE INDEX t_name_n ON t (name, n)");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 3000) "
            "INSERT INTO t SELECT i, CASE WHEN i % 13 = 0 THEN NULL WHEN i % 7 = 0 THEN 'k' || (i % 5) "
            "WHEN i % 5 = 0 THEN (i % 40) + 0.5 ELSE i % 40 END, CASE i % 3 WHEN 0 THEN 'Ada' WHEN 1 THEN 'ada' "
            "ELSE 'Bo' || (i % 4) END, i % 9 FROM c");
    probedAsScanned(db, "SELECT id, k FROM t WHERE k = 3 OR k = 17");
    probedAsScanned(db, "SELECT id, k FROM t WHERE k = 3 OR 17 = k OR k = 3.0 OR k = 'k2' OR k = NULL OR k = 10.5");
    probedAsScanned(db, "SELECT id, k FROM t WHERE (k = 3 OR k = 4) AND n > 2");
    probedAsScanned(db, "SELECT id, n FROM t WHERE id = 5 OR id = 2999 OR id = 5000 OR id = 7.0");
    probedAsScanned(db, "SELECT id, n FROM t WHERE name = 'ADA' AND (n = 1 OR n = 4)");
    probedAsScanned(db, "SELECT id, n FROM t WHERE name = 'ada' OR name = 'bo1'");
    // not one column, or not only equalities: every row is read, and the answer is the same
    probedAsScanned(db, "SELECT id, k FROM t WHERE k = 3 OR n = 4", false);
    probedAsScanned(db, "SELECT id, k FROM t WHERE k = 3 OR k > 38", false);
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t WHERE k = ?1 OR k = ?2", {Value(3), Value("k2")})),
             tst::only(db.exec("SELECT count(*) FROM t NOT INDEXED WHERE k = 3 OR k = 'k2'")));
}

TEST(in_with_a_sub_query_probes_with_the_values_it_returns)
{
    tst::Scratch s("index");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, k, grp INTEGER, label TEXT)");
    db.exec("CREATE INDEX t_k ON t (k)");
    db.exec("CREATE INDEX t_grp_k ON t (grp, k)");
    db.exec("CREATE TABLE pick (v, w INTEGER)");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 6000) "
            "INSERT INTO t SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 6 = 0 THEN 'k' || (i % 50) "
            "ELSE i % 500 END, i % 4, 'row ' || i FROM c");
    db.exec("INSERT INTO pick VALUES (7, 1), (7, 1), (499, 1), ('k12', 1), (NULL, 1), (12.0, 2), ('7', 2), (900, 2), (x'07', 2)");
    probedAsScanned(db, "SELECT id, k FROM t WHERE k IN (SELECT v FROM pick WHERE w = 1)");
    probedAsScanned(db, "SELECT id, k FROM t WHERE k IN (SELECT v FROM pick)");
    probedAsScanned(db, "SELECT id, k FROM t WHERE id IN (SELECT v FROM pick)");
    probedAsScanned(db, "SELECT id, k FROM t WHERE id IN (SELECT v * 10 FROM pick WHERE w = 1) AND grp <> 2");
    probedAsScanned(db, "SELECT id, k FROM t WHERE grp = 3 AND k IN (SELECT v FROM pick)");
    probedAsScanned(db, "SELECT id, k FROM t WHERE k IN (SELECT v FROM pick WHERE w = 99)");
    probedAsScanned(db, "SELECT id, k FROM t WHERE k IN (SELECT v FROM pick WHERE w = 1) OR id = 3", false);
    // NOT IN, and a sub-query that depends on the row, select nothing by probing
    probedAsScanned(db, "SELECT id, k FROM t WHERE k NOT IN (SELECT v FROM pick WHERE v IS NOT NULL) AND id < 50", true);
    probedAsScanned(db, "SELECT id, k FROM t WHERE k IN (SELECT v FROM pick WHERE w = t.grp) AND id < 900", true);
    // more keys than probing is worth: the table is read instead, to the same answer
    db.exec("CREATE TABLE many (v INTEGER)");
    db.exec("INSERT INTO many SELECT id FROM t WHERE id % 3 <> 0");
    probedAsScanned(db, "SELECT id, grp FROM t WHERE id IN (SELECT v FROM many)");
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM t WHERE id IN (SELECT v FROM many)")), std::string("4000"));
    // a prepared statement sees the sub-query's rows as they are at each run
    Statement st = db.prepare("SELECT count(*) FROM t WHERE k IN (SELECT v FROM pick WHERE w = ?)");
    st.bind(1, Value(1));
    const std::string before = tst::only(st.exec());
    db.exec("INSERT INTO pick VALUES (123, 1)");
    const std::string after = tst::only(st.exec());
    CHECK_EQ(std::stoi(after) - std::stoi(before), std::stoi(tst::only(db.exec("SELECT count(*) FROM t NOT INDEXED WHERE k = 123"))));
    CHECK(after != before);
}

TEST(a_filter_on_a_view_or_sub_query_is_applied_inside_it)
{
    tst::Scratch s("index");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, k, code TEXT COLLATE NOCASE, n INTEGER, note TEXT)");
    db.exec("CREATE INDEX t_k ON t (k)");
    db.exec("CREATE INDEX t_code ON t (code)");
    db.exec("CREATE TABLE u (id INTEGER PRIMARY KEY, t_id INTEGER, w)");
    db.exec("CREATE INDEX u_t ON u (t_id)");
    db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 4000) "
            "INSERT INTO t SELECT i, CASE WHEN i % 19 = 0 THEN NULL WHEN i % 8 = 0 THEN 'k' || (i % 30) "
            "WHEN i % 5 = 0 THEN '' || (i % 60) ELSE i % 60 END, CASE i % 3 WHEN 0 THEN 'Ab' WHEN 1 THEN 'aB' ELSE 'c' END "
            "|| (i % 7), i % 11, 'note ' || i FROM c");
    db.exec("INSERT INTO u SELECT id * 2, id, CASE WHEN id % 4 = 0 THEN NULL ELSE id % 9 END FROM t WHERE id % 3 <> 0");
    const std::string plain = "SELECT id, k, code, n FROM t WHERE n <> 4";
    const std::string joined = "SELECT t.id AS id, t.k AS k, u.w AS w, u.id AS uid FROM t JOIN u ON u.t_id = t.id";
    const std::string outer = "SELECT t.id AS id, t.code AS code, u.w AS w FROM t LEFT JOIN u ON u.t_id = t.id AND u.w > 2";
    db.exec("CREATE VIEW v_plain AS " + plain);
    db.exec("CREATE VIEW v_joined AS " + joined);
    db.exec("CREATE VIEW v_outer AS " + outer);
    db.exec("CREATE VIEW v_nested AS SELECT id, k, w FROM v_joined WHERE w IS NOT NULL");
    db.exec("CREATE VIEW v_named (a, b) AS SELECT k, id FROM t");
    db.exec("CREATE VIEW v_grouped AS SELECT n, count(*) AS c, max(id) AS top FROM t GROUP BY n");
    // The view named in `sql`, the same select written in FROM, and a CTE of
    // it -- which is materialized whole and filtered afterwards -- agree.
    auto same = [&](const std::string& view, const std::string& select, const std::string& rest, bool searches = true) {
        const std::string viaView = tst::flatten(db.exec("SELECT * FROM " + view + " x " + rest + " ORDER BY 1, 2"));
        const std::string viaSub = tst::flatten(db.exec("SELECT * FROM (" + select + ") x " + rest + " ORDER BY 1, 2"));
        const std::string viaCte =
            tst::flatten(db.exec("WITH q AS (" + select + ") SELECT * FROM q x " + rest + " ORDER BY 1, 2"));
        CHECK(viaView == viaCte);
        CHECK(viaSub == viaCte);
        const std::string plan = tst::flatten(db.exec("EXPLAIN QUERY PLAN SELECT * FROM " + view + " x " + rest));
        CHECK_EQ(plan.find("SEARCH t") != std::string::npos, searches);
    };
    same("v_plain", plain, "WHERE x.id = 77");
    same("v_plain", plain, "WHERE x.id BETWEEN 100 AND 140 AND x.n < 5");
    same("v_plain", plain, "WHERE x.k = 12");
    same("v_plain", plain, "WHERE x.k = '12'");
    same("v_plain", plain, "WHERE x.k IN (3, '7', 'k10', NULL)");
    same("v_plain", plain, "WHERE x.code = 'AB3'");
    same("v_plain", plain, "WHERE x.code = 'AB3' COLLATE BINARY", false);
    same("v_plain", plain, "WHERE x.k = 12 OR x.k = 15");
    same("v_plain", plain, "WHERE x.k IS NULL AND x.id < 500");
    same("v_plain", plain, "WHERE x.id + 0 = 77", false);
    same("v_joined", joined, "WHERE x.id = 500");
    same("v_joined", joined, "WHERE x.uid = 1000");
    same("v_joined", joined, "WHERE x.k = 7 AND x.w IS NULL");
    same("v_outer", outer, "WHERE x.id = 6");
    same("v_outer", outer, "WHERE x.id BETWEEN 20 AND 60 AND x.w IS NULL");
    same("v_outer", outer, "WHERE x.id < 100 AND x.w = 5");
    same("v_nested", "SELECT id, k, w FROM (" + joined + ") WHERE w IS NOT NULL", "WHERE x.id = 500");
    same("v_named", "SELECT k AS a, id AS b FROM t", "WHERE x.b = 9");
    same("v_named", "SELECT k AS a, id AS b FROM t", "WHERE x.a = 9 AND x.b > 1000");
    // not a plain select: what is filtered is what it returned
    same("v_grouped", "SELECT n, count(*) AS c, max(id) AS top FROM t GROUP BY n", "WHERE x.n = 3", false);
    CHECK_EQ(tst::flatten(db.exec("SELECT * FROM (SELECT id FROM t ORDER BY id DESC LIMIT 3) x WHERE x.id < 3999")),
             std::string("3998"));
    CHECK_EQ(tst::flatten(db.exec("SELECT * FROM (SELECT DISTINCT n FROM t) x WHERE x.n = 3")), std::string("3"));
    CHECK_EQ(tst::flatten(db.exec("SELECT * FROM (SELECT id FROM t WHERE id < 3 UNION ALL SELECT id FROM t WHERE id > 3998) x "
                                  "WHERE x.id > 1 ORDER BY 1")),
             std::string("2;3999;4000"));
    // the null-supplying side of an outer join is filtered after the join, not inside
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM u LEFT JOIN v_plain x ON x.id = u.t_id AND x.id = 77 WHERE u.t_id IN (76, 77)")),
             std::string("2"));
    CHECK_EQ(tst::flatten(db.exec("SELECT u.t_id, x.id FROM u LEFT JOIN v_plain x ON x.id = u.t_id WHERE u.t_id IN (76, 77) "
                                  "AND x.id IS NULL")),
             tst::flatten(db.exec("WITH q AS (" + plain + ") SELECT u.t_id, x.id FROM u LEFT JOIN q x ON x.id = u.t_id "
                                  "WHERE u.t_id IN (76, 77) AND x.id IS NULL")));
    // Joined on a key of what it reads, a view runs once per outer row with the join term inside it; a CTE
    // of the same select is materialized once and joined to. Both ways, the same rows.
    auto joinSame = [&](const std::string& sql, bool searches) {
        std::string viaCte = sql;
        const std::size_t at = viaCte.find("v_plain");
        CHECK(at != std::string::npos);
        viaCte.replace(at, 7, "q");
        CHECK(tst::flatten(db.exec(sql + " ORDER BY 1, 2")) ==
              tst::flatten(db.exec("WITH q AS (" + plain + ") " + viaCte + " ORDER BY 1, 2")));
        const std::string plan = tst::flatten(db.exec("EXPLAIN QUERY PLAN " + sql));
        CHECK_EQ(plan.find("SEARCH t") != std::string::npos, searches);
    };
    joinSame("SELECT u.id, x.k FROM u JOIN v_plain x ON x.id = u.t_id WHERE u.id < 400", true);
    joinSame("SELECT u.id, x.k FROM u JOIN v_plain x ON x.id = u.t_id + 1 AND x.n > u.w", true);
    joinSame("SELECT u.id, x.id FROM u JOIN v_plain x ON x.k = u.w WHERE u.id < 60", true);
    joinSame("SELECT u.id, x.id FROM u LEFT JOIN v_plain x ON x.id = u.t_id AND x.n = 3 WHERE u.id < 400", true);
    joinSame("SELECT u.id, x.id FROM u LEFT JOIN v_plain x ON x.id = u.t_id WHERE u.id < 400 AND x.id IS NULL", true);
    joinSame("SELECT u.id, x.id FROM u, v_plain x WHERE x.id = u.t_id AND u.w = 2 AND x.n = 5", true);
    // no key or index of the view's table to join by: it is read once, as before
    joinSame("SELECT u.id, x.id FROM u JOIN v_plain x ON x.n = u.w WHERE u.id < 40", false);
    // a prepared statement binds the pushed value anew at each run
    Statement st = db.prepare("SELECT note FROM (SELECT id, note FROM t) WHERE id = ?");
    st.bind(1, Value(5));
    CHECK_EQ(tst::only(st.exec()), std::string("note 5"));
    st.bind(1, Value(3999));
    CHECK_EQ(tst::only(st.exec()), std::string("note 3999"));
    st.bind(1, Value("nothing"));
    CHECK_EQ(st.exec().size(), std::size_t(0));
}

int main()
{
    return tst::runAll("index");
}
