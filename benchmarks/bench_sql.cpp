// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The SQL engine under a fixed set of statement shapes, reported as ns/op with
// the allocations each repetition made. Rows, parameters and keys are
// generated ahead of time so the loops time the engine and nothing else.
//
//   bench_sql [--rows N] [--reps R] [--batch B] [--durable] [--cache]
//             [--json out.json] [--csv out.csv] [--quick] [--only name,name]

#define BENCH_DEFINE_ALLOC_HOOKS
#include "bench_util.hpp"

#include "sql/json.hpp"
#include "sql/sql.hpp"

using namespace sql;

namespace {

struct Config
{
    std::uint64_t rows = 200000;
    std::uint64_t batch = 1000;
    int reps = 3;
    bool durable = false;
    bool cache = false;
    std::vector<std::string> only;

    bool wants(const char* name) const
    {
        if (only.empty())
            return true;
        for (const std::string& o : only)
            if (o == name)
                return true;
        return false;
    }
};

Database openDb(const Config& c, const std::filesystem::path& path)
{
    return Database::configure().durable(c.durable).cacheReadChecksums(c.cache).open(path);
}

void createSchema(Database& db)
{
    db.exec("CREATE TABLE item ("
            "  id INTEGER PRIMARY KEY,"
            "  cat INTEGER NOT NULL,"
            "  name TEXT NOT NULL,"
            "  price REAL,"
            "  ts DATETIME,"
            "  payload BLOB)");
    db.exec("CREATE INDEX item_by_cat ON item (cat)");
    db.exec("CREATE INDEX item_by_name ON item (name)");
    db.exec("CREATE TABLE sale (id INTEGER PRIMARY KEY, item_id INTEGER NOT NULL, qty INTEGER, ts DATETIME)");
    db.exec("CREATE INDEX sale_by_item ON sale (item_id)");
}

struct ItemRow
{
    Value id, cat, name, price, ts, payload;
};

std::vector<ItemRow> makeItems(std::uint64_t n)
{
    std::vector<ItemRow> rows;
    rows.reserve(n);
    std::mt19937_64 rng(11);
    const Datetime base = parseDatetime("2024-01-01 00:00:00");
    for (std::uint64_t i = 0; i < n; ++i) {
        ItemRow r;
        r.id = Value(std::int64_t(i + 1));
        r.cat = Value(std::int64_t(i % 1000));
        char name[32];
        std::snprintf(name, sizeof name, "item-%08llu", (unsigned long long)(rng() % 100000000));
        r.name = Value(std::string(name));
        r.price = Value(double(rng() % 100000) / 100.0);
        r.ts = Value(Datetime{base.usec + std::int64_t(i) * 1000000});
        std::string blob(24, char('a' + i % 26));
        r.payload = Value::blob(blob.data(), blob.size());
        rows.push_back(std::move(r));
    }
    return rows;
}

void loadItems(Database& db, const std::vector<ItemRow>& rows, std::uint64_t batch)
{
    Statement insert = db.prepare(
        "INSERT INTO item (id, cat, name, price, ts, payload) VALUES (?, ?, ?, ?, ?, ?)");
    for (std::uint64_t at = 0; at < rows.size(); at += batch) {
        Transaction t = db.begin();
        const std::uint64_t end = std::min<std::uint64_t>(rows.size(), at + batch);
        for (std::uint64_t i = at; i < end; ++i) {
            const ItemRow& r = rows[i];
            insert.bind(1, r.id).bind(2, r.cat).bind(3, r.name).bind(4, r.price).bind(5, r.ts)
                .bind(6, r.payload);
            insert.exec(t);
        }
        t.commit();
    }
}

void loadSales(Database& db, std::uint64_t items, std::uint64_t batch)
{
    Statement insert = db.prepare("INSERT INTO sale (item_id, qty, ts) VALUES (?, ?, ?)");
    const Datetime base = parseDatetime("2024-06-01 00:00:00");
    const std::uint64_t n = items * 2;
    for (std::uint64_t at = 0; at < n; at += batch) {
        Transaction t = db.begin();
        const std::uint64_t end = std::min(n, at + batch);
        for (std::uint64_t i = at; i < end; ++i) {
            insert.bind(1, Value(std::int64_t(i % items + 1)))
                .bind(2, Value(std::int64_t(i % 7 + 1)))
                .bind(3, Value(Datetime{base.usec + std::int64_t(i) * 500000}));
            insert.exec(t);
        }
        t.commit();
    }
}

}  // namespace

int main(int argc, char** argv)
{
    bench::Args args(argc, argv);
    Config c;
    if (args.has("--quick")) {
        c.rows = 50000;
        c.reps = 2;
    }
    c.rows = args.getU("--rows", c.rows);
    c.batch = args.getU("--batch", c.batch);
    c.reps = int(args.getU("--reps", std::uint64_t(c.reps)));
    c.durable = args.has("--durable");
    c.cache = args.has("--cache");
    {
        std::string list = args.get("--only", "");
        while (!list.empty()) {
            const std::size_t comma = list.find(',');
            c.only.push_back(list.substr(0, comma));
            list = comma == std::string::npos ? std::string() : list.substr(comma + 1);
        }
    }

    std::printf("libsql %s bench_sql: %llu rows, batch %llu, reps %d, durable %s, read cache %s\n\n",
                version(), (unsigned long long)c.rows, (unsigned long long)c.batch, c.reps,
                c.durable ? "on" : "off", c.cache ? "on" : "off");

    bench::Report report("sql");
    if (const std::string j = args.get("--json", ""); !j.empty())
        report.jsonPath(j);
    if (const std::string v = args.get("--csv", ""); !v.empty())
        report.csvPath(v);
    report.header();

    bench::Scratch scratch("sql-bench");
    const std::vector<ItemRow> items = makeItems(c.rows);
    std::vector<std::uint32_t> order(c.rows);
    for (std::uint64_t i = 0; i < c.rows; ++i)
        order[i] = std::uint32_t(i);
    std::shuffle(order.begin(), order.end(), std::mt19937_64(5));
    const std::uint64_t n = c.rows;
    const std::uint64_t m = std::min<std::uint64_t>(n, 50000);  // per-statement workloads

    if (c.wants("load")) {
        Database db;
        report.run("insert prepared (batched, 2 indexes)", n, 0, c.reps,
                   [&] {
                       db.close();
                       std::filesystem::remove(scratch.file("load.db"));
                       db = openDb(c, scratch.file("load.db"));
                       createSchema(db);
                   },
                   [&] { loadItems(db, items, c.batch); });
    }

    Database db = openDb(c, scratch.file("main.db"));
    createSchema(db);
    loadItems(db, items, c.batch);
    loadSales(db, n, c.batch);

    if (c.wants("insert")) {
        Database scratchDb;
        std::uint64_t next = 0;
        const std::uint64_t k = std::min<std::uint64_t>(m, 20000);
        report.run("insert prepared (autocommit, 2 indexes)", k, 0, c.reps,
                   [&] {
                       scratchDb.close();
                       std::filesystem::remove(scratch.file("ins.db"));
                       scratchDb = openDb(c, scratch.file("ins.db"));
                       createSchema(scratchDb);
                       next = 0;
                   },
                   [&] {
                       Statement insert = scratchDb.prepare(
                           "INSERT INTO item (cat, name, price, ts, payload) VALUES (?, ?, ?, ?, ?)");
                       for (std::uint64_t i = 0; i < k; ++i, ++next) {
                           const ItemRow& r = items[next % n];
                           insert.bind(1, r.cat).bind(2, r.name).bind(3, r.price).bind(4, r.ts)
                               .bind(5, r.payload);
                           insert.exec();
                       }
                   });
        report.run("insert exec one-off (autocommit)", k, 0, c.reps,
                   [&] {
                       scratchDb.close();
                       std::filesystem::remove(scratch.file("ins2.db"));
                       scratchDb = openDb(c, scratch.file("ins2.db"));
                       createSchema(scratchDb);
                       next = 0;
                   },
                   [&] {
                       for (std::uint64_t i = 0; i < k; ++i, ++next) {
                           const ItemRow& r = items[next % n];
                           scratchDb.exec(
                               "INSERT INTO item (cat, name, price, ts, payload) VALUES (?, ?, ?, ?, ?)",
                               {r.cat, r.name, r.price, r.ts, r.payload});
                       }
                   });
    }

    if (c.wants("point")) {
        Statement byId = db.prepare("SELECT id, cat, name, price FROM item WHERE id = ?");
        report.run("select by pk (prepared, txn each)", m, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            for (std::uint64_t i = 0; i < m; ++i)
                seen += byId.exec({items[order[i]].id}).size();
            if (seen != m)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
        report.run("select by pk (prepared, one read txn)", m, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            Transaction t = db.beginRead();
            for (std::uint64_t i = 0; i < m; ++i)
                seen += byId.exec(t, {items[order[i]].id}).size();
            if (seen != m)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
        const std::uint64_t k = m / 4;
        report.run("select by pk (exec one-off)", k, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            for (std::uint64_t i = 0; i < k; ++i)
                seen += db.exec("SELECT id, cat, name, price FROM item WHERE id = ?",
                                {items[order[i]].id})
                            .size();
            if (seen != k)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
        Statement byName = db.prepare("SELECT id FROM item WHERE name = ?");
        report.run("select by unique-ish index (name = ?)", m, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            for (std::uint64_t i = 0; i < m; ++i)
                seen += byName.exec({items[order[i]].name}).size();
            if (seen < m)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
    }

    if (c.wants("range")) {
        Statement byCat = db.prepare("SELECT id, name, price FROM item WHERE cat = ?");
        const std::uint64_t q = std::min<std::uint64_t>(m / 10, 5000);
        const std::uint64_t perCat = n / 1000;
        report.run("select by index eq (~" + std::to_string(perCat) + " rows)", q, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            for (std::uint64_t i = 0; i < q; ++i)
                seen += byCat.exec({Value(std::int64_t(i % 1000))}).size();
            if (seen != q * perCat)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
        Statement window = db.prepare("SELECT id, name FROM item WHERE id >= ? AND id < ?");
        report.run("select pk range (100 rows)", q, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            for (std::uint64_t i = 0; i < q; ++i) {
                const std::int64_t lo = std::int64_t(order[i] % (n - 100)) + 1;
                seen += window.exec({Value(lo), Value(lo + 100)}).size();
            }
            if (seen != q * 100)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
        Statement top = db.prepare("SELECT id, name FROM item WHERE cat = ? ORDER BY name LIMIT 10");
        report.run("select index eq + order by + limit", q, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            for (std::uint64_t i = 0; i < q; ++i)
                seen += top.exec({Value(std::int64_t(i % 1000))}).size();
            if (seen != q * 10)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
    }

    if (c.wants("scan")) {
        Statement count = db.prepare("SELECT COUNT(*) FROM item");
        report.run("count(*) full scan", n, 0, c.reps, [&] {
            if (count.exec()[0][0].integer() != std::int64_t(n))
                std::printf("  !! wrong count\n");
        });
        Statement filtered = db.prepare("SELECT id FROM item WHERE price < ?");
        report.run("full scan with predicate", n, 0, c.reps, [&] {
            (void)filtered.exec({Value(5.0)}).size();
        });
        Statement grouped = db.prepare("SELECT cat, COUNT(*), SUM(price), MAX(name) FROM item GROUP BY cat");
        report.run("group by over full scan", n, 0, c.reps, [&] {
            if (grouped.exec().size() != std::min<std::uint64_t>(n, 1000))
                std::printf("  !! wrong group count\n");
        });
        Statement sorted = db.prepare("SELECT id, name FROM item ORDER BY name LIMIT 100");
        report.run("order by over full scan, limit 100", n, 0, c.reps, [&] {
            (void)sorted.exec().size();
        });
    }

    if (c.wants("join")) {
        Statement join = db.prepare("SELECT i.name, s.qty, s.ts FROM item i"
                                    " INNER JOIN sale s ON s.item_id = i.id WHERE i.cat = ?");
        const std::uint64_t q = std::min<std::uint64_t>(m / 10, 5000);
        report.run("join via index (cat -> items -> sales)", q, 0, c.reps, [&] {
            std::uint64_t seen = 0;
            for (std::uint64_t i = 0; i < q; ++i)
                seen += join.exec({Value(std::int64_t(i % 1000))}).size();
            if (seen != q * (n / 1000) * 2)
                std::printf("  !! %llu rows\n", (unsigned long long)seen);
        });
        Statement rolled = db.prepare("SELECT i.cat, COUNT(*), SUM(s.qty) FROM item i"
                                      " INNER JOIN sale s ON s.item_id = i.id"
                                      " WHERE i.id >= ? AND i.id < ? GROUP BY i.cat");
        report.run("join + group by (1000 items)", q, 0, c.reps, [&] {
            for (std::uint64_t i = 0; i < q; ++i) {
                const std::int64_t lo = std::int64_t(order[i] % (n - 1000)) + 1;
                (void)rolled.exec({Value(lo), Value(lo + 1000)}).size();
            }
        });
        Statement json = db.prepare("SELECT i.id, i.name, s.qty FROM item i"
                                    " INNER JOIN sale s ON s.item_id = i.id WHERE i.cat = ?"
                                    " ORDER BY i.id FOR JSON AUTO");
        report.run("join FOR JSON AUTO", q, 0, c.reps, [&] {
            std::size_t bytes = 0;
            for (std::uint64_t i = 0; i < q; ++i)
                bytes += json.exec({Value(std::int64_t(i % 1000))})[0][0].text().size();
            if (!bytes)
                std::printf("  !! empty json\n");
        });
    }

    if (c.wants("update")) {
        Statement bump = db.prepare("UPDATE item SET price = price + 1 WHERE id = ?");
        report.run("update by pk (prepared, batched)", m, 0, c.reps, [&] {
            for (std::uint64_t at = 0; at < m; at += c.batch) {
                Transaction t = db.begin();
                for (std::uint64_t i = at; i < std::min(m, at + c.batch); ++i)
                    bump.exec(t, {items[order[i]].id});
                t.commit();
            }
        });
        Statement rename = db.prepare("UPDATE item SET name = ? WHERE id = ?");
        report.run("update indexed column (batched)", m, 0, c.reps, [&] {
            for (std::uint64_t at = 0; at < m; at += c.batch) {
                Transaction t = db.begin();
                for (std::uint64_t i = at; i < std::min(m, at + c.batch); ++i)
                    rename.exec(t, {items[order[i]].name, items[order[i]].id});
                t.commit();
            }
        });
        const std::uint64_t k = std::min<std::uint64_t>(m, 10000);
        report.run("update by pk (autocommit)", k, 0, c.reps, [&] {
            for (std::uint64_t i = 0; i < k; ++i)
                bump.exec({items[order[i]].id});
        });
    }

    if (c.wants("delete")) {
        Statement remove = db.prepare("DELETE FROM item WHERE id = ?");
        Statement insert = db.prepare(
            "INSERT INTO item (id, cat, name, price, ts, payload) VALUES (?, ?, ?, ?, ?, ?)");
        report.run("delete + reinsert by pk (batched)", m, 0, c.reps, [&] {
            for (std::uint64_t at = 0; at < m; at += c.batch) {
                Transaction t = db.begin();
                for (std::uint64_t i = at; i < std::min(m, at + c.batch); ++i)
                    remove.exec(t, {items[order[i]].id});
                t.commit();
            }
            for (std::uint64_t at = 0; at < m; at += c.batch) {
                Transaction t = db.begin();
                for (std::uint64_t i = at; i < std::min(m, at + c.batch); ++i) {
                    const ItemRow& r = items[order[i]];
                    insert.exec(t, {r.id, r.cat, r.name, r.price, r.ts, r.payload});
                }
                t.commit();
            }
        });
    }

    if (c.wants("prepare")) {
        const std::uint64_t k = 20000;
        report.run("prepare + discard (select)", k, 0, c.reps, [&] {
            for (std::uint64_t i = 0; i < k; ++i)
                (void)db.prepare("SELECT id, cat, name, price FROM item WHERE id = ? AND cat > 0");
        });
        report.run("prepare + discard (join)", k, 0, c.reps, [&] {
            for (std::uint64_t i = 0; i < k; ++i)
                (void)db.prepare("SELECT i.name, s.qty FROM item i INNER JOIN sale s"
                                 " ON s.item_id = i.id WHERE i.cat = ? ORDER BY i.name LIMIT 10");
        });
        report.run("exec trivial statement (txn + catalog)", k, 0, c.reps, [&] {
            for (std::uint64_t i = 0; i < k; ++i)
                (void)db.exec("SELECT COUNT(*) FROM sale WHERE id = 1");
        });
    }

    std::printf("\n  process: rss %s, peak rss %s, allocator footprint %s, file %s\n",
                bench::humanBytes(bench::residentBytes()).c_str(),
                bench::humanBytes(bench::peakResidentBytes()).c_str(),
                bench::humanBytes(bench::heapBytes()).c_str(),
                bench::humanBytes(static_cast<long long>(scratch.sizeOf("main.db"))).c_str());
    report.finish();
    return 0;
}
