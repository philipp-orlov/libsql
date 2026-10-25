// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Binary results: the same query as JSON and as a libbufs message, and what
// each column of the message looks like on the wire.
//
// sql::BufsWriter runs plain SQL and returns bytes ready to send; the reader
// side (sql::wire::ResultView) wraps them without parsing. Columns are narrowed:
// small integer deltas, float32 reals, whole-second datetimes and dictionary
// coded text.

#include <cstdio>
#include <filesystem>
#include <string>

#include "sql/bufs.hpp"
#include "sql/json.hpp"
#include "sql/wire/result_view.hpp"

int main()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "libsql-bufs.db";
    std::filesystem::remove(path);

    sql::Database db(path);
    db.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, name TEXT, status TEXT, price REAL, qty INTEGER,"
            " created DATETIME)");
    const char* statuses[] = {"new", "paid", "shipped"};
    {
        sql::Transaction txn = db.begin();
        for (int i = 0; i < 1000; ++i)
            txn.exec("INSERT INTO orders VALUES (?, ?, ?, ?, ?, ?)",
                     sql::Params{sql::Value(i), sql::Value("order-" + std::to_string(i)), sql::Value(statuses[i % 3]),
                                 sql::Value(i % 40 * 0.5), sql::Value(i % 7),
                                 sql::Value(sql::Datetime{(1700000000LL + i * 60) * 1000000})});
        txn.commit();
    }

    const std::string query = "SELECT id, name, status, price, qty, created FROM orders WHERE price < ?";
    const sql::Params params{sql::Value(10.0)};

    sql::BufsWriter writer;  // keep one and reuse it: a warm writer allocates nothing
    const sql::BufsBytes bytes = writer.exec(db, query, params);
    const sql::wire::ResultView view = sql::wire::ResultView::open(bytes.data(), bytes.size());

    static const char* const kEncoding[] = {"Empty", "Int", "Real", "Datetime", "Text", "TextDict", "Blob", "Mixed"};
    static const char* const kWidth[] = {"-", "u8", "u16", "u32", "i64"};
    std::printf("%llu rows, %zu bytes\n", static_cast<unsigned long long>(view.rowCount()), bytes.size());
    for (std::size_t c = 0; c < view.columns().size(); ++c) {
        const sql::wire::ColumnView col = view.batch(0).column(c);
        std::printf("  %-8s %-9s %s\n", view.columns()[c].c_str(), kEncoding[int(col.encoding())],
                    kWidth[int(col.width())]);
    }

    const std::string json = sql::toJson(db.exec(query, params));
    std::printf("the same result as JSON: %zu bytes (%.1fx)\n", json.size(), double(json.size()) / double(bytes.size()));

    std::printf("row 3: id=%lld name=%s status=%s\n", static_cast<long long>(view.value(3, 0).integer()),
                std::string(view.value(3, 1).text()).c_str(), std::string(view.value(3, 2).text()).c_str());
    std::filesystem::remove(path);
    return 0;
}
