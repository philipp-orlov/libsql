// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sqlgen.hpp"

#include <array>
#include <sstream>

namespace conformance {
namespace {

const std::array<const char*, 3> kTables = {"t0", "t1", "t2"};
const std::array<const char*, 5> kColumns = {"id", "a", "b", "c", "d"};

std::string quoteText(const std::string& text)
{
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'')
            out += "''";
        else
            out += ch;
    }
    out += "'";
    return out;
}

}  // namespace

SqlGenerator::SqlGenerator(std::uint64_t seed) : rng_(seed)
{
    setup_.push_back("CREATE TABLE t0 (id INTEGER PRIMARY KEY, a INTEGER, b REAL, c TEXT, d BLOB)");
    setup_.push_back("CREATE TABLE t1 (id INTEGER PRIMARY KEY, a INTEGER, b REAL, c TEXT, d BLOB)");
    setup_.push_back("CREATE TABLE t2 (id INTEGER PRIMARY KEY, a INTEGER, b REAL, c TEXT, d BLOB)");
    setup_.push_back("CREATE INDEX idx_t0_a ON t0(a)");
    setup_.push_back("CREATE INDEX idx_t1_c ON t1(c)");
    setup_.push_back("CREATE UNIQUE INDEX idx_t2_a_c ON t2(a, c)");

    const std::array<const char*, 8> values = {
        "(1, 12, 12.0, '12', x'31')",      "(2, 12, 12.5, ' 12 ', x'3132')",
        "(3, NULL, NULL, NULL, NULL)",      "(4, -1, -1.25, 'abc', x'00ff')",
        "(5, 0, 0.0, '', x'')",             "(6, 7, 7.5, 'AbC', x'414243')",
        "(7, 99, 99.125, 'z', x'7a')",      "(8, 12, 12.0, 'abc', x'616263')"};
    for (const char* table : kTables) {
        for (const char* value : values)
            setup_.push_back(std::string("INSERT INTO ") + table + " VALUES " + value);
    }
}

int SqlGenerator::pick(int upper)
{
    return static_cast<int>(rng_() % static_cast<std::uint64_t>(upper));
}

bool SqlGenerator::chance(int numerator, int denominator)
{
    return pick(denominator) < numerator;
}

std::string SqlGenerator::tableName()
{
    return kTables[static_cast<std::size_t>(pick(static_cast<int>(kTables.size())))];
}

std::string SqlGenerator::column(const std::string& table)
{
    return table + "." + kColumns[static_cast<std::size_t>(pick(static_cast<int>(kColumns.size())))];
}

std::string SqlGenerator::literal()
{
    switch (pick(12)) {
    case 0:
        return "NULL";
    case 1:
        return "12";
    case 2:
        return "12.0";
    case 3:
        return "'12'";
    case 4:
        return "' 12 '";
    case 5:
        return "'abc'";
    case 6:
        return "'AbC'";
    case 7:
        return "x'31'";
    case 8:
        return "0";
    case 9:
        return "-1";
    case 10:
        return "99.125";
    default:
        return "''";
    }
}

std::string SqlGenerator::expression(const std::string& table, int depth)
{
    if (depth <= 0) {
        if (chance(2, 3))
            return column(table);
        return literal();
    }

    const std::string e1 = expression(table, depth - 1);
    const std::string e2 = expression(table, depth - 1);
    switch (pick(20)) {
    case 0:
        return "(" + e1 + " + " + e2 + ")";
    case 1:
        return "(" + e1 + " - " + e2 + ")";
    case 2:
        return "(" + e1 + " * " + e2 + ")";
    case 3:
        return "(" + e1 + " || " + e2 + ")";
    case 4:
        return "CAST(" + e1 + " AS TEXT)";
    case 5:
        return "CAST(" + e1 + " AS INTEGER)";
    case 6:
        return "coalesce(" + e1 + ", " + e2 + ")";
    case 7:
        return "ifnull(" + e1 + ", " + e2 + ")";
    case 8:
        return "nullif(" + e1 + ", " + e2 + ")";
    case 9:
        return "abs(" + e1 + ")";
    case 10:
        return "length(" + e1 + ")";
    case 11:
        return "lower(" + e1 + ")";
    case 12:
        return "upper(" + e1 + ")";
    case 13:
        return "substr(" + e1 + ", 1, 2)";
    case 14:
        return "instr(" + e1 + ", 'a')";
    case 15:
        return "replace(" + e1 + ", 'a', 'A')";
    case 16:
        return "trim(" + e1 + ")";
    case 17:
        return "round(" + e1 + ", 1)";
    case 18:
        return "typeof(" + e1 + ")";
    default:
        return "CASE WHEN " + predicate(table) + " THEN " + e1 + " ELSE " + e2 + " END";
    }
}

std::string SqlGenerator::predicate(const std::string& table)
{
    const std::string lhs = expression(table, 0);
    const std::string rhs = literal();
    switch (pick(14)) {
    case 0:
        return lhs + " = " + rhs;
    case 1:
        return lhs + " <> " + rhs;
    case 2:
        return lhs + " < " + rhs;
    case 3:
        return lhs + " IS " + rhs;
    case 4:
        return lhs + " IS NOT " + rhs;
    case 5:
        return lhs + " IN (NULL, 12, 'abc')";
    case 6:
        return lhs + " BETWEEN -5 AND 20";
    case 7:
        return lhs + " LIKE 'a%' ESCAPE '\\'";
    case 8:
        return lhs + " GLOB '*b*'";
    case 9:
        return "(" + lhs + " = " + rhs + " OR " + lhs + " IS NULL)";
    case 10:
        return "EXISTS (SELECT 1 FROM t1 WHERE t1.a = " + table + ".a)";
    case 11:
        return lhs + " IN (SELECT a FROM t2)";
    case 12:
        return "NOT (" + lhs + " = " + rhs + ")";
    default:
        return "1";
    }
}

GeneratedStatement SqlGenerator::next()
{
    GeneratedStatement out;
    const std::string t = tableName();
    switch (pick(24)) {
    case 0:
        out.sql = "SELECT " + expression(t, 2) + ", " + expression(t, 1) + " FROM " + t +
                  " WHERE " + predicate(t) + " ORDER BY 1, 2";
        break;
    case 1:
        out.sql = "SELECT DISTINCT a, c FROM " + t + " WHERE " + predicate(t) +
                  " ORDER BY a, c LIMIT 5 OFFSET " + std::to_string(pick(3));
        break;
    case 2:
        out.sql = "SELECT a, count(*), sum(a), avg(b), min(c), max(c), total(b), "
                  "group_concat(c ORDER BY c) FROM " +
                  t + " GROUP BY a HAVING count(*) >= 1 ORDER BY a";
        break;
    case 3:
        out.sql = "SELECT t0.id, t1.c FROM t0 INNER JOIN t1 ON t0.a = t1.a ORDER BY t0.id, t1.c";
        break;
    case 4:
        out.sql = "SELECT t0.id, t1.c FROM t0 LEFT JOIN t1 USING(a) ORDER BY t0.id, t1.c";
        break;
    case 5:
        out.sql = "SELECT t0.id, t2.id FROM t0 CROSS JOIN t2 WHERE t0.id < 3 AND t2.id < 3 "
                  "ORDER BY t0.id, t2.id";
        break;
    case 6:
        out.sql = "SELECT id, (SELECT max(a) FROM t1 WHERE t1.c = " + t + ".c) FROM " + t +
                  " ORDER BY id";
        break;
    case 7:
        out.sql = "WITH x(v) AS (SELECT a FROM " + t + " WHERE a IS NOT NULL) "
                  "SELECT v, count(*) FROM x GROUP BY v ORDER BY v";
        break;
    case 8:
        out.sql = "WITH RECURSIVE cnt(x) AS (VALUES(1) UNION ALL SELECT x + 1 FROM cnt WHERE x < 4) "
                  "SELECT x FROM cnt ORDER BY x";
        break;
    case 9:
        out.sql = "SELECT a FROM t0 UNION SELECT a FROM t1 ORDER BY a";
        break;
    case 10:
        out.sql = "SELECT a FROM t0 UNION ALL SELECT a FROM t1 ORDER BY a";
        break;
    case 11:
        out.sql = "SELECT a FROM t0 INTERSECT SELECT a FROM t1 ORDER BY a";
        break;
    case 12:
        out.sql = "SELECT a FROM t0 EXCEPT SELECT a FROM t2 ORDER BY a";
        break;
    case 13:
        out.sql = "SELECT id, row_number() OVER (ORDER BY a, id), rank() OVER (ORDER BY a), "
                  "dense_rank() OVER (ORDER BY a) FROM " +
                  t + " ORDER BY id";
        break;
    case 14:
        out.sql = "SELECT id, sum(a) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW), "
                  "lag(a) OVER (ORDER BY id), lead(a) OVER (ORDER BY id) FROM " +
                  t + " ORDER BY id";
        break;
    case 15:
        out.sql = "INSERT OR IGNORE INTO " + t + " (id, a, b, c, d) VALUES (" +
                  std::to_string(1000 + pick(200)) + ", " + literal() + ", " + literal() +
                  ", " + quoteText("gen" + std::to_string(pick(10))) + ", x'aa')";
        out.checkState = true;
        out.table = t;
        break;
    case 16:
        out.sql = "INSERT OR REPLACE INTO " + t + " (id, a, b, c, d) VALUES (" +
                  std::to_string(1 + pick(8)) + ", " + literal() + ", " + literal() +
                  ", 'replace', x'bb') RETURNING id, a, c";
        out.checkState = true;
        out.table = t;
        break;
    case 17:
        out.sql = "INSERT INTO " + t + " (id, a, c) VALUES (" + std::to_string(1200 + pick(200)) +
                  ", 12, 'upsert') ON CONFLICT(id) DO UPDATE SET c = excluded.c RETURNING id, c";
        out.checkState = true;
        out.table = t;
        break;
    case 18:
        out.sql = "UPDATE " + t + " SET a = a + 1 WHERE id IN (1, 2, 3)";
        out.checkState = true;
        out.table = t;
        break;
    case 19:
        out.sql = "DELETE FROM " + t + " WHERE id > " + std::to_string(1100 + pick(200));
        out.checkState = true;
        out.table = t;
        break;
    case 20:
        out.sql =
            "CREATE INDEX IF NOT EXISTS fuzz_expr_" + std::to_string(pick(100000)) + " ON " + t +
            " (lower(c), a DESC) WHERE c IS NOT NULL";
        break;
    case 21:
        out.sql = "CREATE TABLE IF NOT EXISTS strict_" + std::to_string(pick(100000)) +
                  " (id INTEGER PRIMARY KEY, v TEXT) STRICT";
        break;
    case 22:
        out.sql = "CREATE TABLE IF NOT EXISTS wr_" + std::to_string(pick(100000)) +
                  " (id INTEGER PRIMARY KEY, v TEXT) WITHOUT ROWID";
        break;
    default:
        out.sql = "SELECT hex(d), quote(c), min(a, 12), max(a, 12) FROM " + t + " ORDER BY id";
        break;
    }
    return out;
}

std::string SqlGenerator::stateCheck(const std::string& table) const
{
    return "SELECT * FROM " + table + " ORDER BY id, a, b, c, d";
}

}  // namespace conformance
