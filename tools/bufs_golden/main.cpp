// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Prints tools/sql/ui/src/app/bufs/golden.ts: for a set of queries covering every encoding of the wire format, the
// bytes sql::BufsWriter produces (base64) and the same result as the front end reads it. The TypeScript reader's
// spec decodes the bytes and compares every cell, which proves the C++ writer and the TypeScript reader agree.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sql/bufs.hpp"
#include "json/json.hpp"
#include "json/utf8.hpp"

namespace {

// The form the front end's result reader gives a value (utils/cell.ts): null, a number or a string stand for
// themselves; an INTEGER a JavaScript number cannot hold is {"type":"integer"}; a BLOB, or TEXT that is not
// UTF-8, is {"type":"blob"} with a hex preview.
constexpr std::int64_t kMaxSafeInteger = 9007199254740991;
constexpr std::size_t kBlobPreviewBytes = 32;

json::Json blobCell(const unsigned char* bytes, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (std::size_t i = 0; i < std::min(size, kBlobPreviewBytes); ++i) {
        hex += digits[bytes[i] >> 4];
        hex += digits[bytes[i] & 0x0F];
    }
    return json::Json{{"type", "blob"}, {"size", size}, {"value", hex}};
}

json::Json encodeCell(const sql::Value& value)
{
    using json::Json;
    switch (value.type()) {
        case sql::Type::Null: return Json();
        case sql::Type::Integer: {
            const std::int64_t i = value.integer();
            if (i >= -kMaxSafeInteger && i <= kMaxSafeInteger)
                return Json(static_cast<long long>(i));
            return Json{{"type", "integer"}, {"value", std::to_string(i)}};
        }
        case sql::Type::Real: {
            const double d = value.real();
            if (std::isfinite(d))
                return Json(d);
            return Json(std::isnan(d) ? "NaN" : (d > 0 ? "Inf" : "-Inf"));
        }
        case sql::Type::Datetime: return Json(sql::formatDatetime(value.datetime()));
        case sql::Type::Text: {
            const std::string_view text = value.text();
            if (json::isValidUtf8(text))
                return Json(text);
            return blobCell(reinterpret_cast<const unsigned char*>(text.data()), text.size());
        }
        case sql::Type::Blob: {
            const sql::BlobView blob = value.blob();
            return blobCell(reinterpret_cast<const unsigned char*>(blob.data()), blob.size());
        }
    }
    return Json();
}

std::string base64(const sql::BufsBytes& bytes)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t left = bytes.size() - i;
        const unsigned v = (unsigned(bytes[i]) << 16) | (left > 1 ? unsigned(bytes[i + 1]) << 8 : 0) | (left > 2 ? bytes[i + 2] : 0);
        out += digits[(v >> 18) & 63];
        out += digits[(v >> 12) & 63];
        out += left > 1 ? digits[(v >> 6) & 63] : '=';
        out += left > 2 ? digits[v & 63] : '=';
    }
    return out;
}

std::string quote(const std::string& text)
{
    std::string out = "'";
    for (char c : text) {
        if (c == '\'' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "'";
}

struct Case
{
    const char* name;
    const char* sql;
    std::size_t batchRows = 0;
};

}  // namespace

int printRemoteGolden();

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--remote")
        return printRemoteGolden();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "libsql-bufs-golden.db";
    std::filesystem::remove(path);
    sql::Database db(path);
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, small INTEGER, medium INTEGER, big INTEGER, huge INTEGER,"
            " ratio REAL, price REAL, born DATETIME, name TEXT, status TEXT, data BLOB, anything)");
    const char* statuses[] = {"open", "closed", "pending"};
    for (int i = 0; i < 12; ++i) {
        db.exec("INSERT INTO t VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                sql::Params{sql::Value(i + 1), i % 4 == 3 ? sql::Value() : sql::Value(100 + i),
                            sql::Value(1000 * i), sql::Value(std::int64_t(70000) * i),
                            i == 2 ? sql::Value(std::int64_t(9007199254740993LL)) : sql::Value(std::int64_t(i) - 5),
                            sql::Value(i * 0.5), sql::Value(i * 0.01),
                            sql::Value(sql::Datetime{(1700000000LL + i * 3600) * 1000000 + (i == 5 ? 250 : 0)}),
                            i % 5 == 4 ? sql::Value() : sql::Value("name-" + std::to_string(i)),
                            sql::Value(statuses[i % 3]),
                            i % 2 ? sql::Value::blob("0123456789abcdefghijklmnopqrstuvwxyz0123456789", 46) : sql::Value(),
                            i == 0 ? sql::Value(1) : i == 1 ? sql::Value("text") : i == 2 ? sql::Value(2.5) : i == 3 ? sql::Value() : sql::Value::blob("\x01\x02", 2)});
    }
    db.exec("CREATE TABLE d (day DATETIME, secs DATETIME)");
    db.exec("INSERT INTO d VALUES (?, ?), (?, ?)",
            sql::Params{sql::Value(sql::Datetime{1704067200LL * 1000000}), sql::Value(sql::Datetime{1704067201LL * 1000000}),
                        sql::Value(sql::Datetime{1706745600LL * 1000000}), sql::Value(sql::Datetime{1706745700LL * 1000000})});

    const std::vector<Case> cases = {
        {"int_u8", "SELECT small FROM t WHERE id < 4 ORDER BY id"},
        {"int_u16_nulls", "SELECT small, medium FROM t ORDER BY id"},
        {"int_u32", "SELECT big FROM t ORDER BY id"},
        {"int_i64_and_big", "SELECT huge FROM t ORDER BY id"},
        {"int_i64_extremes", "SELECT -9223372036854775807 - 1 AS lo, 9223372036854775807 AS hi"},
        {"real_f32", "SELECT ratio FROM t ORDER BY id"},
        {"real_f64", "SELECT price FROM t ORDER BY id"},
        {"real_special", "SELECT -0.0 AS negative_zero, 1e308 * 10 AS inf, 0.1 AS tenth"},
        {"datetime_micro", "SELECT born FROM t ORDER BY id"},
        {"datetime_day_second", "SELECT day, secs FROM d ORDER BY day"},
        {"text_plain", "SELECT name FROM t ORDER BY id"},
        {"text_dict", "SELECT status FROM t ORDER BY id"},
        {"text_invalid_utf8", "SELECT CAST(x'c328ff' AS TEXT) AS bad, 'fine' AS good"},
        {"text_empty_and_unicode", "SELECT '' AS empty, 'h\xc3\xa9llo \xe2\x82\xac' AS words"},
        {"blob_prefix", "SELECT data FROM t ORDER BY id"},
        {"mixed", "SELECT anything FROM t ORDER BY id"},
        {"all_null", "SELECT NULL AS nothing FROM t WHERE id < 3"},
        {"zero_rows", "SELECT id, name FROM t WHERE 0"},
        {"no_columns", "UPDATE t SET small = small WHERE id = 1"},
        {"batches", "SELECT id, status, price FROM t ORDER BY id", 5},
        {"wide", "SELECT * FROM t ORDER BY id"},
    };

    std::printf("// Generated by tools/bufs_golden. Do not edit.\n");
    std::printf("export interface GoldenCase {\n  name: string;\n  sql: string;\n  /** base64 of the BufsWriter output */\n  bufs: string;\n  columns: string[];\n  /** the same rows as the front end reads them (utils/cell.ts) */\n  rows: unknown[][];\n}\n\n");
    std::printf("export const GOLDEN: GoldenCase[] = [\n");
    for (const Case& c : cases) {
        sql::BufsWriter writer(sql::BufsOptions().blobPrefix(32).batchRows(c.batchRows));
        const sql::BufsBytes bytes = writer.exec(db, c.sql);
        const sql::Result result = db.exec(c.sql);
        json::Json rows = json::Json::array();
        for (const sql::Row& row : result) {
            json::Json cells = json::Json::array();
            for (const sql::Value& v : row)
                cells.push_back(encodeCell(v));
            rows.push_back(std::move(cells));
        }
        json::Json columns = json::Json::array();
        for (const std::string& name : result.columns())
            columns.push_back(name);
        std::printf("  {\n    name: %s,\n    sql: %s,\n    bufs: %s,\n    columns: %s,\n    rows: %s,\n  },\n",
                    quote(c.name).c_str(), quote(c.sql).c_str(), quote(base64(bytes)).c_str(), columns.dump().c_str(),
                    rows.dump().c_str());
    }
    std::printf("];\n");
    std::filesystem::remove(path);
    return 0;
}
