// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql against SQLite on the same statements, the same data and the same
// schema, reported side by side as ns/op.
//
//   bench_vs_sqlite [--rows N] [--reps R] [--batch B] [--durable] [--materialize]
//                   [--only name,name]
//
// Both engines get their fastest honest configuration. libsql: durable(false)
// unless --durable. SQLite (3.45.1, the conformance oracle, THREADSAFE=0):
// WAL, synchronous=OFF (FULL with --durable), a 256 MiB page cache and a
// 1 GiB mmap, so neither engine reads through a cache the other lacks.
// Both read every column of every row in place: SQLite with sqlite3_column_*,
// libsql through Statement::each. --materialize makes libsql build an owned
// Result per statement instead (the exec() path).

#include "bench_util.hpp"

#include <functional>
#include <map>
#include <memory>

#include <sqlite3.h>

#include "sql/sql.hpp"

using namespace sql;

namespace {

/// The few things a workload needs from an engine.
class Engine
{
public:
    virtual ~Engine() = default;
    virtual void open(const std::filesystem::path& path, bool durable) = 0;
    virtual void close() = 0;
    /// Statements without results (DDL).
    virtual void exec(const std::string& text) = 0;
    virtual int prepare(const std::string& text) = 0;
    virtual void bind(int h, int i, const Value& v) = 0;
    /// Runs statement `h` with its bindings, inside the open transaction if
    /// there is one; reads every column of every row; returns the row count.
    virtual std::size_t run(int h) = 0;
    /// Prepare, bind, run and throw away.
    virtual std::size_t oneOff(const std::string& text, const std::vector<Value>& params) = 0;
    virtual void prepareAndDiscard(const std::string& text) = 0;
    virtual void begin() = 0;
    virtual void beginRead() = 0;
    virtual void commit() = 0;
    virtual std::int64_t lastSum() const = 0;
};

// ------------------------------------------------------------- libsql ----

class LibsqlEngine final : public Engine
{
public:
    void open(const std::filesystem::path& path, bool durable) override
    {
        db_ = Database::configure().durable(durable).open(path);
    }
    void close() override
    {
        stmts_.clear();
        txn_ = Transaction();
        db_.close();
    }
    void exec(const std::string& text) override { db_.exec(text); }
    int prepare(const std::string& text) override
    {
        stmts_.push_back(db_.prepare(text));
        return int(stmts_.size() - 1);
    }
    void bind(int h, int i, const Value& v) override { stmts_[std::size_t(h)].bind(std::size_t(i), v); }
    std::size_t run(int h) override
    {
        Statement& s = stmts_[std::size_t(h)];
        if (materialize)
            return consume(txn_.valid() ? s.exec(txn_) : s.exec());
        std::size_t rows = 0;
        auto fn = [&](const RowView& r) {
            ++rows;
            for (const Value& v : r)
                add(v);
            return true;
        };
        if (txn_.valid())
            s.each(txn_, fn);
        else
            s.each(fn);
        return rows;
    }
    bool materialize = false;
    std::size_t oneOff(const std::string& text, const std::vector<Value>& params) override
    {
        return consume(txn_.valid() ? txn_.exec(text, params) : db_.exec(text, params));
    }
    void prepareAndDiscard(const std::string& text) override { (void)db_.prepare(text); }
    void begin() override { txn_ = db_.begin(); }
    void beginRead() override { txn_ = db_.beginRead(); }
    void commit() override { txn_.commit(); }
    std::int64_t lastSum() const override { return sum_; }

private:
    void add(const Value& v)
    {
        switch (v.type()) {
            case Type::Integer: sum_ += v.integer(); break;
            case Type::Real: sum_ += std::int64_t(v.real()); break;
            case Type::Text: sum_ += std::int64_t(v.text().size()); break;
            case Type::Blob: sum_ += std::int64_t(v.blob().size()); break;
            default: break;
        }
    }
    std::size_t consume(const Result& r)
    {
        for (const Row& row : r) {
            for (const Value& v : row)
                add(v);
        }
        return r.size();
    }

    Database db_;
    Transaction txn_;
    std::vector<Statement> stmts_;
    std::int64_t sum_ = 0;
};

// ------------------------------------------------------------- SQLite ----

class SqliteEngine final : public Engine
{
public:
    ~SqliteEngine() override { close(); }

    void open(const std::filesystem::path& path, bool durable) override
    {
        check(sqlite3_open(path.string().c_str(), &db_));
        exec("PRAGMA journal_mode = WAL");
        exec(durable ? "PRAGMA synchronous = FULL" : "PRAGMA synchronous = OFF");
        exec("PRAGMA cache_size = -262144");
        exec("PRAGMA mmap_size = 1073741824");
        exec("PRAGMA temp_store = MEMORY");
        begin_ = prepare("BEGIN");
        commit_ = prepare("COMMIT");
    }
    void close() override
    {
        for (sqlite3_stmt* s : stmts_)
            sqlite3_finalize(s);
        stmts_.clear();
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
    }
    void exec(const std::string& text) override
    {
        char* err = nullptr;
        if (sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::fprintf(stderr, "sqlite: %s\n", err ? err : "?");
            sqlite3_free(err);
            std::exit(2);
        }
    }
    int prepare(const std::string& text) override
    {
        sqlite3_stmt* s = nullptr;
        check(sqlite3_prepare_v3(db_, text.c_str(), int(text.size()), SQLITE_PREPARE_PERSISTENT, &s, nullptr));
        stmts_.push_back(s);
        return int(stmts_.size() - 1);
    }
    void bind(int h, int i, const Value& v) override { bindOne(stmts_[std::size_t(h)], i, v); }
    std::size_t run(int h) override
    {
        sqlite3_stmt* s = stmts_[std::size_t(h)];
        const std::size_t n = drain(s);
        sqlite3_reset(s);
        return n;
    }
    std::size_t oneOff(const std::string& text, const std::vector<Value>& params) override
    {
        sqlite3_stmt* s = nullptr;
        check(sqlite3_prepare_v2(db_, text.c_str(), int(text.size()), &s, nullptr));
        for (std::size_t i = 0; i < params.size(); ++i)
            bindOne(s, int(i + 1), params[i]);
        const std::size_t n = drain(s);
        sqlite3_finalize(s);
        return n;
    }
    void prepareAndDiscard(const std::string& text) override
    {
        sqlite3_stmt* s = nullptr;
        check(sqlite3_prepare_v2(db_, text.c_str(), int(text.size()), &s, nullptr));
        sqlite3_finalize(s);
    }
    void begin() override { run(begin_); }
    // A deferred BEGIN takes its read snapshot at the first statement.
    void beginRead() override { run(begin_); }
    void commit() override { run(commit_); }
    std::int64_t lastSum() const override { return sum_; }

private:
    void check(int rc)
    {
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(db_));
            std::exit(2);
        }
    }
    void bindOne(sqlite3_stmt* s, int i, const Value& v)
    {
        switch (v.type()) {
            case Type::Null: sqlite3_bind_null(s, i); break;
            case Type::Integer: sqlite3_bind_int64(s, i, v.integer()); break;
            case Type::Real: sqlite3_bind_double(s, i, v.real()); break;
            case Type::Datetime: sqlite3_bind_int64(s, i, v.datetime().usec); break;
            case Type::Text:
                sqlite3_bind_text(s, i, v.text().data(), int(v.text().size()), SQLITE_STATIC);
                break;
            case Type::Blob:
                sqlite3_bind_blob(s, i, v.blob().data(), int(v.blob().size()), SQLITE_STATIC);
                break;
        }
    }
    std::size_t drain(sqlite3_stmt* s)
    {
        std::size_t rows = 0;
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            ++rows;
            const int n = sqlite3_column_count(s);
            for (int c = 0; c < n; ++c) {
                switch (sqlite3_column_type(s, c)) {
                    case SQLITE_INTEGER: sum_ += sqlite3_column_int64(s, c); break;
                    case SQLITE_FLOAT: sum_ += std::int64_t(sqlite3_column_double(s, c)); break;
                    case SQLITE_TEXT:
                        (void)sqlite3_column_text(s, c);
                        sum_ += sqlite3_column_bytes(s, c);
                        break;
                    case SQLITE_BLOB:
                        (void)sqlite3_column_blob(s, c);
                        sum_ += sqlite3_column_bytes(s, c);
                        break;
                    default: break;
                }
            }
        }
        if (rc != SQLITE_DONE) {
            std::fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(db_));
            std::exit(2);
        }
        return rows;
    }

    sqlite3* db_ = nullptr;
    std::vector<sqlite3_stmt*> stmts_;
    int begin_ = -1, commit_ = -1;
    std::int64_t sum_ = 0;
};

// ----------------------------------------------------------- workloads ----

struct Config
{
    std::uint64_t rows = 200000;
    std::uint64_t batch = 1000;
    int reps = 3;
    bool durable = false;
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

const char* const kSchema[] = {
    "CREATE TABLE item (id INTEGER PRIMARY KEY, cat INTEGER NOT NULL, name TEXT NOT NULL,"
    " price REAL, ts INTEGER, payload BLOB)",
    "CREATE INDEX item_by_cat ON item (cat)",
    "CREATE INDEX item_by_name ON item (name)",
    "CREATE TABLE sale (id INTEGER PRIMARY KEY, item_id INTEGER NOT NULL, qty INTEGER, ts INTEGER)",
    "CREATE INDEX sale_by_item ON sale (item_id)",
};

struct ItemRow
{
    Value id, cat, name, rename, price, ts, payload;
};

std::vector<ItemRow> makeItems(std::uint64_t n)
{
    std::vector<ItemRow> rows;
    rows.reserve(n);
    std::mt19937_64 rng(11);
    for (std::uint64_t i = 0; i < n; ++i) {
        ItemRow r;
        r.id = Value(std::int64_t(i + 1));
        r.cat = Value(std::int64_t(i % 1000));
        char name[32];
        std::snprintf(name, sizeof name, "item-%08llu", (unsigned long long)(rng() % 100000000));
        r.name = Value(std::string(name));
        name[0] = 'I';
        r.rename = Value(std::string(name));
        r.price = Value(double(rng() % 100000) / 100.0);
        r.ts = Value(std::int64_t(1704067200 + std::int64_t(i)));
        std::string blob(24, char('a' + i % 26));
        r.payload = Value::blob(blob.data(), blob.size());
        rows.push_back(std::move(r));
    }
    return rows;
}

struct Workload
{
    std::string name;
    std::uint64_t ops;
    /// Unmeasured set-up before each repetition, then the measured part.
    std::function<void(Engine&)> before;
    std::function<void(Engine&)> body;
};

struct Fixture
{
    const Config& c;
    const std::vector<ItemRow>& items;
    const std::vector<std::uint32_t>& order;
    std::filesystem::path dir;
};

void createSchema(Engine& e)
{
    for (const char* s : kSchema)
        e.exec(s);
}

void loadItems(Engine& e, const std::vector<ItemRow>& items, std::uint64_t batch, int insert)
{
    for (std::uint64_t at = 0; at < items.size(); at += batch) {
        e.begin();
        for (std::uint64_t i = at; i < std::min<std::uint64_t>(items.size(), at + batch); ++i) {
            const ItemRow& r = items[i];
            e.bind(insert, 1, r.id);
            e.bind(insert, 2, r.cat);
            e.bind(insert, 3, r.name);
            e.bind(insert, 4, r.price);
            e.bind(insert, 5, r.ts);
            e.bind(insert, 6, r.payload);
            e.run(insert);
        }
        e.commit();
    }
}

void loadSales(Engine& e, std::uint64_t items, std::uint64_t batch)
{
    const int insert = e.prepare("INSERT INTO sale (item_id, qty, ts) VALUES (?, ?, ?)");
    const std::uint64_t n = items * 2;
    for (std::uint64_t at = 0; at < n; at += batch) {
        e.begin();
        for (std::uint64_t i = at; i < std::min(n, at + batch); ++i) {
            e.bind(insert, 1, Value(std::int64_t(i % items + 1)));
            e.bind(insert, 2, Value(std::int64_t(i % 7 + 1)));
            e.bind(insert, 3, Value(std::int64_t(1717200000 + std::int64_t(i))));
            e.run(insert);
        }
        e.commit();
    }
}

/// Times `w` on one engine; returns the median ns per op.
double measure(Engine& e, const Workload& w, int reps)
{
    std::vector<double> ns;
    for (int r = 0; r < reps; ++r) {
        if (w.before)
            w.before(e);
        bench::Timer t;
        w.body(e);
        ns.push_back(t.ns());
    }
    std::sort(ns.begin(), ns.end());
    return ns[ns.size() / 2] / double(w.ops);
}

}  // namespace

int main(int argc, char** argv)
{
    bench::Args args(argc, argv);
    Config c;
    c.rows = args.getU("--rows", c.rows);
    c.batch = args.getU("--batch", c.batch);
    c.reps = int(args.getU("--reps", std::uint64_t(c.reps)));
    c.durable = args.has("--durable");
    {
        std::string list = args.get("--only", "");
        while (!list.empty()) {
            const std::size_t comma = list.find(',');
            c.only.push_back(list.substr(0, comma));
            list = comma == std::string::npos ? std::string() : list.substr(comma + 1);
        }
    }
    std::printf("libsql %s vs SQLite %s: %llu rows, batch %llu, reps %d, durable %s\n\n", version(),
                sqlite3_libversion(), (unsigned long long)c.rows, (unsigned long long)c.batch, c.reps,
                c.durable ? "on" : "off");

    bench::Scratch scratch("sql-vs-sqlite");
    const std::vector<ItemRow> items = makeItems(c.rows);
    std::vector<std::uint32_t> order(c.rows);
    for (std::uint64_t i = 0; i < c.rows; ++i)
        order[i] = std::uint32_t(i);
    std::shuffle(order.begin(), order.end(), std::mt19937_64(5));
    const std::uint64_t n = c.rows;
    const std::uint64_t m = std::min<std::uint64_t>(n, 50000);
    const std::uint64_t q = std::min<std::uint64_t>(m / 10, 5000);
    const char* const kInsert = "INSERT INTO item (id, cat, name, price, ts, payload) VALUES (?, ?, ?, ?, ?, ?)";

    LibsqlEngine libsql;
    libsql.materialize = args.has("--materialize");
    SqliteEngine sqlite;
    Engine* engines[2] = {&libsql, &sqlite};
    const char* tags[2] = {"libsql", "sqlite"};

    std::printf("  %-42s %12s %12s %9s\n", "workload", "libsql ns", "sqlite ns", "ratio");
    auto report = [&](const Workload& w, double a, double b) {
        std::printf("  %-42s %12.1f %12.1f %8.2fx%s\n", w.name.c_str(), a, b, a / b,
                    a <= b ? "  libsql faster" : "");
        std::fflush(stdout);
    };

    // Bulk load into a fresh file, measured on its own.
    if (c.wants("load")) {
        Workload w{"insert prepared (batched, 2 indexes)", n, nullptr, nullptr};
        double ns[2];
        for (int k = 0; k < 2; ++k) {
            Engine& e = *engines[k];
            const std::filesystem::path p = scratch.file(std::string("load-") + tags[k] + ".db");
            int insert = -1;
            w.before = [&](Engine& en) {
                en.close();
                std::filesystem::remove(p);
                std::filesystem::remove(p.string() + "-wal");
                std::filesystem::remove(p.string() + "-shm");
                en.open(p, c.durable);
                createSchema(en);
                insert = en.prepare(kInsert);
            };
            w.body = [&](Engine& en) { loadItems(en, items, c.batch, insert); };
            ns[k] = measure(e, w, c.reps);
            e.close();
        }
        report(w, ns[0], ns[1]);
    }

    if (c.wants("insert")) {
        const std::uint64_t k = std::min<std::uint64_t>(m, 20000);
        for (int oneOff = 0; oneOff < 2; ++oneOff) {
            Workload w{oneOff ? "insert one-off (autocommit)" : "insert prepared (autocommit, 2 indexes)", k,
                       nullptr, nullptr};
            double ns[2];
            for (int x = 0; x < 2; ++x) {
                Engine& e = *engines[x];
                const std::filesystem::path p = scratch.file(std::string("ins-") + tags[x] + ".db");
                int insert = -1;
                w.before = [&](Engine& en) {
                    en.close();
                    std::filesystem::remove(p);
                    std::filesystem::remove(p.string() + "-wal");
                    std::filesystem::remove(p.string() + "-shm");
                    en.open(p, c.durable);
                    createSchema(en);
                    insert = en.prepare("INSERT INTO item (cat, name, price, ts, payload) VALUES (?, ?, ?, ?, ?)");
                };
                w.body = [&](Engine& en) {
                    for (std::uint64_t i = 0; i < k; ++i) {
                        const ItemRow& r = items[i % n];
                        if (oneOff) {
                            en.oneOff("INSERT INTO item (cat, name, price, ts, payload) VALUES (?, ?, ?, ?, ?)",
                                      {r.cat, r.name, r.price, r.ts, r.payload});
                        } else {
                            en.bind(insert, 1, r.cat);
                            en.bind(insert, 2, r.name);
                            en.bind(insert, 3, r.price);
                            en.bind(insert, 4, r.ts);
                            en.bind(insert, 5, r.payload);
                            en.run(insert);
                        }
                    }
                };
                ns[x] = measure(e, w, c.reps);
                e.close();
            }
            report(w, ns[0], ns[1]);
        }
    }

    // Everything else runs against one loaded database per engine.
    for (int x = 0; x < 2; ++x) {
        Engine& e = *engines[x];
        const std::filesystem::path p = scratch.file(std::string("main-") + tags[x] + ".db");
        e.open(p, c.durable);
        createSchema(e);
        loadItems(e, items, c.batch, e.prepare(kInsert));
        loadSales(e, n, c.batch);
        e.exec("ANALYZE");
    }

    std::vector<Workload> ws;
    // Statement handles are per engine, so each workload prepares lazily
    // through a small cache keyed by engine.
    struct Handles
    {
        std::map<std::pair<Engine*, std::string>, int> map;
        int get(Engine& e, const std::string& sql)
        {
            auto it = map.find({&e, sql});
            if (it != map.end())
                return it->second;
            const int h = e.prepare(sql);
            map[{&e, sql}] = h;
            return h;
        }
    } handles;
    auto pointRead = [&](const std::string& sql, std::uint64_t count, auto&& param, bool oneTxn) {
        return [&, sql, count, param, oneTxn](Engine& e) {
            const int h = handles.get(e, sql);
            if (oneTxn)
                e.beginRead();
            for (std::uint64_t i = 0; i < count; ++i) {
                param(e, h, i);
                e.run(h);
            }
            if (oneTxn)
                e.commit();
        };
    };

    if (c.wants("point")) {
        auto byId = [&](Engine& e, int h, std::uint64_t i) { e.bind(h, 1, items[order[i]].id); };
        ws.push_back({"select by pk (prepared)", m, nullptr,
                      pointRead("SELECT id, cat, name, price FROM item WHERE id = ?", m, byId, false)});
        ws.push_back({"select by pk (prepared, one read txn)", m, nullptr,
                      pointRead("SELECT id, cat, name, price FROM item WHERE id = ?", m, byId, true)});
        ws.push_back({"select by pk (one-off)", m / 4, nullptr, [&](Engine& e) {
                          for (std::uint64_t i = 0; i < m / 4; ++i)
                              e.oneOff("SELECT id, cat, name, price FROM item WHERE id = ?", {items[order[i]].id});
                      }});
        ws.push_back({"select by index (name = ?)", m, nullptr,
                      pointRead("SELECT id FROM item WHERE name = ?", m,
                                [&](Engine& e, int h, std::uint64_t i) { e.bind(h, 1, items[order[i]].name); },
                                false)});
    }
    if (c.wants("range")) {
        auto byCat = [&](Engine& e, int h, std::uint64_t i) { e.bind(h, 1, Value(std::int64_t(i % 1000))); };
        ws.push_back({"select by index eq (~200 rows)", q, nullptr,
                      pointRead("SELECT id, name, price FROM item WHERE cat = ?", q, byCat, false)});
        ws.push_back({"select pk range (100 rows)", q, nullptr,
                      pointRead("SELECT id, name FROM item WHERE id >= ? AND id < ?", q,
                                [&](Engine& e, int h, std::uint64_t i) {
                                    const std::int64_t lo = std::int64_t(order[i] % (n - 100)) + 1;
                                    e.bind(h, 1, Value(lo));
                                    e.bind(h, 2, Value(lo + 100));
                                },
                                false)});
        ws.push_back({"select index eq + order by + limit 10", q, nullptr,
                      pointRead("SELECT id, name FROM item WHERE cat = ? ORDER BY name LIMIT 10", q, byCat, false)});
        ws.push_back({"covering index eq (cat, count)", q, nullptr,
                      pointRead("SELECT count(*) FROM item WHERE cat = ?", q, byCat, false)});
    }
    if (c.wants("scan")) {
        auto once = [&](const std::string& sql, std::function<void(Engine&, int)> bindAll = nullptr) {
            return [&, sql, bindAll](Engine& e) {
                const int h = handles.get(e, sql);
                if (bindAll)
                    bindAll(e, h);
                e.run(h);
            };
        };
        ws.push_back({"count(*) full scan (per row)", n, nullptr, once("SELECT COUNT(*) FROM item")});
        ws.push_back({"full scan with predicate (per row)", n, nullptr,
                      once("SELECT id FROM item WHERE price < ?",
                           [](Engine& e, int h) { e.bind(h, 1, Value(5.0)); })});
        ws.push_back({"group by over full scan (per row)", n, nullptr,
                      once("SELECT cat, COUNT(*), SUM(price), MAX(name) FROM item GROUP BY cat")});
        ws.push_back({"order by over full scan, limit 100 (per row)", n, nullptr,
                      once("SELECT id, name FROM item ORDER BY price LIMIT 100")});
        ws.push_back({"covering index scan: names (per row)", n, nullptr,
                      once("SELECT name FROM item WHERE name > 'item-5'")});
        ws.push_back({"distinct over index (per row)", n, nullptr, once("SELECT DISTINCT cat FROM item")});
        ws.push_back({"full sort, all rows (per row)", n, nullptr, once("SELECT id, price FROM item ORDER BY price")});
    }
    if (c.wants("join")) {
        auto byCat = [&](Engine& e, int h, std::uint64_t i) { e.bind(h, 1, Value(std::int64_t(i % 1000))); };
        ws.push_back({"join via index (cat -> items -> sales)", q, nullptr,
                      pointRead("SELECT i.name, s.qty, s.ts FROM item i JOIN sale s ON s.item_id = i.id"
                                " WHERE i.cat = ?",
                                q, byCat, false)});
        ws.push_back({"join + group by (1000 items)", q, nullptr,
                      pointRead("SELECT i.cat, COUNT(*), SUM(s.qty) FROM item i JOIN sale s ON s.item_id = i.id"
                                " WHERE i.id >= ? AND i.id < ? GROUP BY i.cat",
                                q,
                                [&](Engine& e, int h, std::uint64_t i) {
                                    const std::int64_t lo = std::int64_t(order[i] % (n - 1000)) + 1;
                                    e.bind(h, 1, Value(lo));
                                    e.bind(h, 2, Value(lo + 1000));
                                },
                                false)});
    }
    auto batched = [&](const std::string& sql, std::uint64_t count,
                       std::function<void(Engine&, int, std::uint64_t)> param) {
        return [&, sql, count, param](Engine& e) {
            const int h = handles.get(e, sql);
            for (std::uint64_t at = 0; at < count; at += c.batch) {
                e.begin();
                for (std::uint64_t i = at; i < std::min(count, at + c.batch); ++i) {
                    param(e, h, i);
                    e.run(h);
                }
                e.commit();
            }
        };
    };
    if (c.wants("update")) {
        ws.push_back({"update by pk (batched)", m, nullptr,
                      batched("UPDATE item SET price = price + 1 WHERE id = ?", m,
                              [&](Engine& e, int h, std::uint64_t i) { e.bind(h, 1, items[order[i]].id); })});
        // Alternates between two names so every update really changes the index.
        auto flip = std::make_shared<std::map<Engine*, int>>();
        ws.push_back({"update indexed column (batched)", m, [flip](Engine& e) { (*flip)[&e] ^= 1; },
                      batched("UPDATE item SET name = ? WHERE id = ?", m,
                              [&, flip](Engine& e, int h, std::uint64_t i) {
                                  const ItemRow& r = items[order[i]];
                                  e.bind(h, 1, (*flip)[&e] ? r.rename : r.name);
                                  e.bind(h, 2, r.id);
                              })});
        const std::uint64_t k = std::min<std::uint64_t>(m, 10000);
        ws.push_back({"update by pk (autocommit)", k, nullptr,
                      pointRead("UPDATE item SET price = price + 1 WHERE id = ?", k,
                                [&](Engine& e, int h, std::uint64_t i) { e.bind(h, 1, items[order[i]].id); },
                                false)});
    }
    if (c.wants("delete")) {
        ws.push_back({"delete + reinsert by pk (batched)", m, nullptr, [&](Engine& e) {
                          batched("DELETE FROM item WHERE id = ?", m,
                                  [&](Engine& en, int h, std::uint64_t i) { en.bind(h, 1, items[order[i]].id); })(e);
                          batched(kInsert, m, [&](Engine& en, int h, std::uint64_t i) {
                              const ItemRow& r = items[order[i]];
                              en.bind(h, 1, r.id);
                              en.bind(h, 2, r.cat);
                              en.bind(h, 3, r.name);
                              en.bind(h, 4, r.price);
                              en.bind(h, 5, r.ts);
                              en.bind(h, 6, r.payload);
                          })(e);
                      }});
    }
    if (c.wants("prepare")) {
        const std::uint64_t k = std::min<std::uint64_t>(m, 20000);
        ws.push_back({"prepare + discard (select)", k, nullptr, [&](Engine& e) {
                          for (std::uint64_t i = 0; i < k; ++i)
                              e.prepareAndDiscard("SELECT id, cat, name, price FROM item WHERE id = ? AND cat > 0");
                      }});
        ws.push_back({"prepare + discard (join)", k, nullptr, [&](Engine& e) {
                          for (std::uint64_t i = 0; i < k; ++i)
                              e.prepareAndDiscard("SELECT i.name, s.qty FROM item i INNER JOIN sale s"
                                                  " ON s.item_id = i.id WHERE i.cat = ? ORDER BY i.name LIMIT 10");
                      }});
        ws.push_back({"exec trivial statement (one-off)", k, nullptr, [&](Engine& e) {
                          for (std::uint64_t i = 0; i < k; ++i)
                              e.oneOff("SELECT COUNT(*) FROM sale WHERE id = 1", {});
                      }});
    }

    int wins = 0;
    for (const Workload& w : ws) {
        const double a = measure(libsql, w, c.reps);
        const double b = measure(sqlite, w, c.reps);
        wins += a <= b;
        report(w, a, b);
    }
    std::printf("\n  libsql faster on %d of %zu shared-database workloads (checksums %lld / %lld)\n", wins,
                ws.size(), (long long)libsql.lastSum(), (long long)sqlite.lastSum());
    libsql.close();
    sqlite.close();
    return 0;
}
