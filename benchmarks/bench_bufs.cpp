// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Binary results against JSON: bytes on the wire, server time, memory, and the
// time a C++ reader needs to touch every cell. Run in Release:
//
//   ./bench_bufs [rows...]        (default: 10000 100000)
//
// "JSON" is sql::toJson over a Result, which is what a JSON reply costs apart
// from the web API's own cell encoding. gzip sizes need zlib (skipped without).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "sql/bufs.hpp"
#include "sql/json.hpp"
#include "sql/wire/result_view.hpp"

#if __has_include(<zlib.h>)
#include <zlib.h>
#define BENCH_HAVE_ZLIB 1
#endif

#include "bench_util.hpp"

namespace {

using Clock = std::chrono::steady_clock;

template <class F>
double medianMs(int reps, F&& fn)
{
    std::vector<double> t;
    for (int i = 0; i < reps; ++i) {
        const auto a = Clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

std::size_t gzipSize(const void* data, std::size_t size)
{
#ifdef BENCH_HAVE_ZLIB
    uLongf out = compressBound(uLong(size));
    std::vector<Bytef> buffer(out);
    if (compress2(buffer.data(), &out, static_cast<const Bytef*>(data), uLong(size), 6) == Z_OK)
        return out;
#else
    (void)data;
    (void)size;
#endif
    return 0;
}

void populate(sql::Database& db, int rows)
{
    db.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, name TEXT, status TEXT, price REAL, qty INTEGER,"
            " created DATETIME, note TEXT)");
    std::mt19937_64 rng(42);
    const char* statuses[] = {"new", "paid", "packed", "shipped", "returned"};
    sql::Transaction txn = db.begin();
    sql::Statement st = db.prepare("INSERT INTO orders VALUES (?, ?, ?, ?, ?, ?, ?)");
    for (int i = 0; i < rows; ++i) {
        st.exec(txn, sql::Params{sql::Value(i + 1), sql::Value("customer-" + std::to_string(rng() % 100000)),
                                 sql::Value(statuses[rng() % 5]), sql::Value(double(rng() % 10000) / 100.0),
                                 sql::Value(std::int64_t(rng() % 1001)),
                                 sql::Value(sql::Datetime{(1700000000LL + std::int64_t(rng() % 31536000)) * 1000000}),
                                 rng() % 5 == 0 ? sql::Value("note " + std::to_string(rng() % 1000)) : sql::Value()});
    }
    txn.commit();
}

void measure(const char* label, sql::Database& db, const std::string& query)
{
    sql::BufsWriter writer;
    sql::BufsBytes bytes = writer.exec(db, query);
    const sql::Result result = db.exec(query);
    const std::string json = sql::toJson(result);
    std::printf("%s: %zu rows\n", label, result.size());
    std::printf("  wire bytes   json %10zu   bufs %10zu   (%.2fx smaller)\n", json.size(), bytes.size(),
                double(json.size()) / double(bytes.size()));
    const std::size_t gj = gzipSize(json.data(), json.size()), gb = gzipSize(bytes.data(), bytes.size());
    if (gj)
        std::printf("  gzip -6      json %10zu   bufs %10zu   (%.2fx smaller)\n", gj, gb, double(gj) / double(gb));

    const double tJson = medianMs(20, [&] { std::string s = sql::toJson(db.exec(query)); (void)s; });
    const double tBufs = medianMs(20, [&] { (void)writer.exec(db, query); });
    sql::BufsWriter viaResult;
    const double tWrite = medianMs(20, [&] { (void)viaResult.write(db.exec(query)); });
    std::printf("  server ms    exec+json %8.2f   BufsWriter::exec %8.2f   exec+write(Result) %8.2f\n", tJson, tBufs,
                tWrite);

    sql::BufsWriter batched(sql::BufsOptions().batchRows(10000));
    std::size_t largest = 0;
    batched.onBatch([&](sql::BufsBytes b) { largest = std::max(largest, b.size()); });
    (void)batched.exec(db, query);
    std::printf("  memory       Result+json ~%zu (json bytes)   writer %zu   batched writer %zu (largest message %zu)\n",
                json.size(), writer.capacity(), batched.capacity(), largest);

    const sql::wire::ResultView view = sql::wire::ResultView::open(bytes.data(), bytes.size());
    std::int64_t sink = 0;
    const double tRead = medianMs(20, [&] {
        for (std::size_t b = 0; b < view.batchCount(); ++b) {
            const auto& batch = view.batch(b);
            for (std::size_t c = 0; c < batch.columnCount(); ++c) {
                const auto col = batch.column(c);
                for (std::size_t r = 0; r < col.size(); ++r) {
                    switch (col.type(r)) {
                        case sql::Type::Integer: sink += col.integer(r); break;
                        case sql::Type::Text: sink += std::int64_t(col.text(r).size()); break;
                        case sql::Type::Real: sink += std::int64_t(col.real(r)); break;
                        case sql::Type::Datetime: sink += col.datetime(r).usec; break;
                        default: break;
                    }
                }
            }
        }
    });
    std::printf("  C++ read every cell %.2f ms (%lld)\n\n", tRead, static_cast<long long>(sink));
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<int> sizes;
    for (int i = 1; i < argc; ++i)
        sizes.push_back(std::atoi(argv[i]));
    if (sizes.empty())
        sizes = {10000, 100000};

    bench::Scratch dir("bufs");
    for (int rows : sizes) {
        sql::Database db(dir.file("orders" + std::to_string(rows) + ".db"));
        populate(db, rows);
        measure(("orders, " + std::to_string(rows) + " rows x 7 columns").c_str(), db,
                "SELECT id, name, status, price, qty, created, note FROM orders");
    }
    sql::Database db(dir.file("ints.db"));
    db.exec("CREATE TABLE t (n INTEGER PRIMARY KEY)");
    db.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000) INSERT INTO t SELECT n FROM c");
    measure("one integer column, 1000000 rows", db, "SELECT n FROM t");
    return 0;
}
