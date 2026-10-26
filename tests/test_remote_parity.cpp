// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Database::connect behaves like a local Database: every scenario below runs once against a local database and
// once against a remote one, writes what it saw into a log, and the two logs must be identical.

#include <functional>
#include <sstream>

#include "remote_fixture.hpp"
#include "sql/bufs.hpp"
#include "sql/wire/result_view.hpp"

using namespace sql;

namespace {

/// Everything a scenario observes, as text.
struct Log
{
    std::ostringstream out;
    void line(const std::string& s) { out << s << '\n'; }

    void result(const std::string& label, const Result& r)
    {
        out << label << ": columns=";
        for (const std::string& c : r.columns())
            out << c << ',';
        out << " qualified=";
        for (const std::string& c : r.qualifiedColumns())
            out << c << ',';
        out << " changes=" << r.changes() << " last=" << r.lastInsertId() << '\n';
        for (const Row& row : r) {
            out << "  ";
            for (const Value& v : row)
                out << typeName(v) << ':' << v.toLiteral() << " | ";
            out << '\n';
        }
    }
    static const char* typeName(const Value& v) { return toString(v.type()); }

    template <class F>
    void attempt(const std::string& label, F&& f)
    {
        try {
            f();
        } catch (const Error& e) {
            out << label << ": Error code=" << int(e.code()) << " constraint=" << int(e.constraint()) << " what=" << e.what()
                << '\n';
        }
    }
    void counters(Database& db)
    {
        out << "counters changes=" << db.changes() << " total=" << db.totalChanges() << " last=" << db.lastInsertRowid()
            << " inTxn=" << db.inTransaction() << '\n';
    }
};

/// Records what a consumer is told.
struct Recorder final : ResultConsumer
{
    explicit Recorder(Log& l, std::size_t stopAfter = SIZE_MAX) : log(l), stop(stopAfter) {}
    void begin(const ResultShape& s) override
    {
        log.out << "begin(";
        for (std::size_t i = 0; i < s.names.size(); ++i)
            log.out << s.names[i] << '/' << (i < s.qualified.size() ? s.qualified[i] : "") << '/' << s.declTypes[i] << ',';
        log.out << ")\n";
    }
    bool row(const RowView& r) override
    {
        log.out << "row(";
        for (const Value& v : r)
            log.out << Log::typeName(v) << ':' << v.toLiteral() << ',';
        log.out << ")\n";
        return ++rows < stop;
    }
    void end(std::uint64_t changes, std::int64_t id) override
    {
        log.out << "end(" << changes << ',' << id << ")\n";
    }
    Log& log;
    std::size_t stop, rows = 0;
};

using Scenario = std::function<void(Database&, Log&)>;

void seed(Database& db)
{
    db.exec("CREATE TABLE person(id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, score REAL, born DATETIME, photo BLOB)");
    db.exec("CREATE TABLE tag(label TEXT UNIQUE, weight REAL)");
    db.exec("CREATE INDEX person_name ON person(name)");
    db.exec("INSERT INTO person(name, age, score, born, photo) VALUES ('Ada', 36, 1.5, '1990-02-03 04:05:06', x'0102'), "
            "('Bo', NULL, 2.25, NULL, NULL), ('Cy', 52, NULL, '2001-01-01', x'')");
}

void parity(const char* /*name*/, const Scenario& scenario)
{
    std::string local, remote;
    {
        tst::Scratch s("parity-local");
        Database db = s.open();
        seed(db);
        Log log;
        scenario(db, log);
        local = log.out.str();
    }
    {
        remote_test::Served served;
        Database db = served.connect();
        CHECK(db.remote());
        seed(db);  // through the connection, so that its counters match a local database's
        Log log;
        try {
            scenario(db, log);
        } catch (const std::exception& e) {
            tst::fail(__FILE__, __LINE__, std::string("remote scenario threw: ") + e.what() + "\nlog so far:\n" + log.out.str());
        }
        remote = log.out.str();
    }
    if (local != remote) {
        // Show the first differing line.
        std::istringstream a(local), b(remote);
        std::string x, y;
        int line = 1;
        while (std::getline(a, x) && std::getline(b, y) && x == y)
            ++line;
        tst::fail(__FILE__, __LINE__, "local and remote differ at line " + std::to_string(line) + ":\n  local : " + x +
                                          "\n  remote: " + y);
    }
    CHECK(!local.empty());
}

}  // namespace

TEST(parity_select_and_dml)
{
    parity("select", [](Database& db, Log& log) {
        log.result("all", db.exec("SELECT * FROM person ORDER BY id"));
        log.result("param", db.exec("SELECT name, age FROM person WHERE age > ? ORDER BY name", {Value(40)}));
        log.result("named", db.exec("SELECT name FROM person WHERE name = :n", NamedParams{{":n", Value("Bo")}}));
        log.result("expr", db.exec("SELECT 1, 2.5, 'x', NULL, x'ff', 9223372036854775807, -9223372036854775807 - 1"));
        log.result("agg", db.exec("SELECT count(*), sum(age), avg(score), min(name), max(born) FROM person"));
        log.result("join", db.exec("SELECT p.name, t.label FROM person p LEFT JOIN tag t ON t.label = p.name ORDER BY p.id"));
        log.result("insert", db.exec("INSERT INTO person(name, age) VALUES ('Di', 20), ('Ed', 21)"));
        log.result("returning", db.exec("INSERT INTO person(name) VALUES ('Fy') RETURNING id, name"));
        log.result("update", db.exec("UPDATE person SET age = age + 1 WHERE age IS NOT NULL"));
        log.result("delete", db.exec("DELETE FROM person WHERE name IN ('Di', 'Ed')"));
        log.result("for json", db.exec("SELECT id, name FROM person ORDER BY id FOR JSON AUTO"));
        log.result("multi", db.exec("CREATE TABLE z(a); INSERT INTO z VALUES (1),(2); SELECT sum(a) FROM z"));
        log.result("empty", db.exec("SELECT * FROM person WHERE 0"));
        log.result("ddl", db.exec("CREATE VIEW v AS SELECT name FROM person"));
        log.result("pragma", db.exec("PRAGMA table_info(person)"));
        log.result("unicode", db.exec("SELECT ?, ?", {Value("h\xc3\xa9llo \xe2\x82\xac"), Value::blob("\0\1\2", 3)}));
        log.result("datetime", db.exec("SELECT born, typeof(born), date(born) FROM person ORDER BY id"));
        log.counters(db);
    });
}

TEST(parity_values_roundtrip)
{
    parity("values", [](Database& db, Log& log) {
        db.exec("CREATE TABLE v(i INTEGER, r REAL, t TEXT, b BLOB, d DATETIME, a)");
        std::vector<Params> rows = {
            {Value(0), Value(0.0), Value(""), Value::blob("", 0), Value(Datetime{0}), Value()},
            {Value(std::int64_t(1) << 62), Value(-1.5e300), Value("text with 'quotes' and \"doubles\""), Value::blob("\xff\x00\x01", 3),
             Value(Datetime{-86400000000LL}), Value(1)},
            {Value(-5), Value(3.141592653589793), Value(std::string(10000, 'x')), Value::blob(std::string(70000, '\x07').data(), 70000),
             Value(Datetime{1700000000123456LL}), Value("mixed")},
            {Value(), Value(), Value(), Value(), Value(), Value(2.5)},
        };
        for (const Params& p : rows)
            db.exec("INSERT INTO v VALUES (?,?,?,?,?,?)", p);
        const Result r = db.exec("SELECT i, r, t, length(b), b, d, a, typeof(a) FROM v ORDER BY rowid");
        log.result("values", r);
    });
}

TEST(parity_errors)
{
    parity("errors", [](Database& db, Log& log) {
        log.attempt("syntax", [&] { db.exec("SELEC 1"); });
        log.attempt("missing table", [&] { db.exec("SELECT * FROM nope"); });
        log.attempt("missing column", [&] { db.exec("SELECT nope FROM person"); });
        log.attempt("pk", [&] { db.exec("INSERT INTO person(id, name) VALUES (1, 'dup')"); });
        log.attempt("not null", [&] { db.exec("INSERT INTO person(name) VALUES (NULL)"); });
        log.attempt("unique", [&] {
            db.exec("INSERT INTO tag VALUES ('a', 1)");
            db.exec("INSERT INTO tag VALUES ('a', 2)");
        });
        log.attempt("param count", [&] { db.exec("SELECT ?, ?", {Value(1)}); });
        log.attempt("runtime", [&] { db.exec("SELECT 9223372036854775807 + 1"); });
        log.attempt("exists", [&] { db.exec("CREATE TABLE person(x)"); });
        log.attempt("readonly pragma", [&] { db.exec("PRAGMA nope_pragma = 1"); });
        log.result("still works", db.exec("SELECT count(*) FROM person"));
    });
}

TEST(parity_prepared_statements)
{
    parity("prepared", [](Database& db, Log& log) {
        Statement st = db.prepare("SELECT id, name FROM person WHERE age > :min AND name <> :skip ORDER BY id");
        log.line("sql=" + st.sql());
        log.line("parameters=" + std::to_string(st.parameters()));
        log.line("index(:min)=" + std::to_string(st.parameterIndex(":min")) + " index(skip)=" + std::to_string(st.parameterIndex("skip")) +
                 " index(nope)=" + std::to_string(st.parameterIndex("nope")));
        log.line("name(1)=" + st.parameterName(1) + " name(2)=" + st.parameterName(2) + " name(3)=[" + st.parameterName(3) + "]");
        std::string cols;
        for (const std::string& c : st.columnNames())
            cols += c + ",";
        log.line("columns=" + cols + " writes=" + std::to_string(st.writes()));
        st.bind(":min", Value(10)).bind(2, Value("Cy"));
        log.result("exec", st.exec());
        st.clear();
        st.bind(":min", Value(0));
        log.result("unbound is null", st.exec());
        log.result("init list", st.exec({Value(30), Value("zz")}));
        log.attempt("wrong count", [&] { st.exec({Value(1)}); });
        log.attempt("no such parameter", [&] { st.bind("nope", Value(1)); });
        log.attempt("bind range", [&] { st.bind(9, Value(1)); });
        st.bind(Params{Value(0), Value("Ada")});
        Recorder consumer(log);
        log.line("changes=" + std::to_string(st.exec(consumer)));
        std::size_t seen = 0;
        st.bind(Params{Value(0), Value("zz")});
        st.each([&](const RowView& r) {
            log.line("each " + r[1].toText());
            return ++seen < 2;
        });
        Statement write = db.prepare("INSERT INTO tag(label, weight) VALUES (?, ?)");
        log.line("writes=" + std::to_string(write.writes()));
        log.result("insert", write.exec({Value("t1"), Value(1.5)}));
        log.result("insert 2", write.exec({Value("t2"), Value(2.5)}));
        log.attempt("unique", [&] { write.exec({Value("t1"), Value(0)}); });
        log.result("tags", db.exec("SELECT * FROM tag ORDER BY label"));
        log.counters(db);
    });
}

TEST(parity_transactions)
{
    parity("transactions", [](Database& db, Log& log) {
        {
            Transaction t = db.begin();
            t.exec("INSERT INTO tag VALUES ('x', 1)");
            log.result("inside", t.exec("SELECT count(*) FROM tag"));
            t.rollback();
        }
        log.result("after rollback", db.exec("SELECT count(*) FROM tag"));
        {
            Transaction t = db.begin();
            t.exec("INSERT INTO tag VALUES ('y', 2)");
            t.commit();
        }
        log.result("after commit", db.exec("SELECT * FROM tag"));
        {
            Transaction t = db.begin();
            t.exec("INSERT INTO tag VALUES ('z', 3)");
            // leaves scope without a commit: rolled back
        }
        log.result("scope exit", db.exec("SELECT count(*) FROM tag"));
        {
            Transaction t = db.beginRead();
            log.result("read txn", t.exec("SELECT count(*) FROM person"));
            log.attempt("write in read txn", [&] { t.exec("INSERT INTO tag VALUES ('w', 0)"); });
        }
        {
            Transaction t = db.begin();
            log.attempt("error inside", [&] { t.exec("INSERT INTO tag VALUES ('y', 9)"); });
            log.result("txn survives", t.exec("SELECT count(*) FROM tag"));
            t.commit();
            log.attempt("finished", [&] { t.exec("SELECT 1"); });
        }
        // Transaction control in the text.
        db.exec("BEGIN");
        log.line("inTransaction=" + std::to_string(db.inTransaction()));
        db.exec("INSERT INTO tag VALUES ('sql-txn', 5)");
        log.attempt("begin while sql txn", [&] { db.begin(); });
        db.exec("ROLLBACK");
        log.line("inTransaction=" + std::to_string(db.inTransaction()));
        db.exec("SAVEPOINT sp; INSERT INTO tag VALUES ('sp', 1); ROLLBACK TO sp; RELEASE sp");
        log.result("savepoint", db.exec("SELECT count(*) FROM tag"));
        // A prepared statement in a transaction.
        Statement st = db.prepare("INSERT INTO tag VALUES (?, ?)");
        Transaction t = db.begin();
        st.exec(t, {Value("in-txn"), Value(1)});
        Result into;
        db.prepare("SELECT label FROM tag ORDER BY label").execInto(t, into);
        log.result("execInto", into);
        t.commit();
        log.counters(db);
    });
}

TEST(parity_consumers)
{
    parity("consumers", [](Database& db, Log& log) {
        {
            Recorder c(log);
            log.line("changes=" + std::to_string(db.exec("SELECT id, name FROM person ORDER BY id", Params{}, c)));
        }
        {
            Recorder c(log);
            db.exec("SELECT id FROM person WHERE 0", Params{}, c);
        }
        {
            Recorder c(log);
            log.line("changes=" + std::to_string(db.exec("UPDATE person SET age = 1", Params{}, c)));
        }
        {
            Recorder c(log);
            db.exec("INSERT INTO person(name) VALUES ('Q') RETURNING id, name", Params{}, c);
        }
        {
            Recorder c(log, 2);  // stops after two rows
            db.exec("SELECT id, name FROM person ORDER BY id", Params{}, c);
        }
        {
            Recorder c(log);
            db.exec("SELECT name FROM person WHERE id = :id", NamedParams{{":id", Value(2)}}, c);
        }
        {
            Recorder c(log);
            Transaction t = db.begin();
            t.exec("SELECT count(*) FROM person", Params{}, c);
        }
        {
            Recorder c(log);
            log.attempt("consumer error", [&] { db.exec("SELECT * FROM nope", Params{}, c); });
        }
        // A consumer that throws: the statement is abandoned, the database stays usable.
        struct Thrower final : ResultConsumer
        {
            void begin(const ResultShape&) override {}
            bool row(const RowView&) override { throw std::runtime_error("consumer failed"); }
            void end(std::uint64_t, std::int64_t) override {}
        } thrower;
        bool caught = false;
        try {
            db.exec("SELECT * FROM person", Params{}, thrower);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        log.line("thrower caught=" + std::to_string(caught));
        log.result("usable", db.exec("SELECT count(*) FROM person"));
    });
}

TEST(parity_catalog)
{
    parity("catalog", [](Database& db, Log& log) {
        for (const std::string& t : db.tables())
            log.line("table " + t);
        const TableInfo info = db.table("person");
        log.line("name=" + info.name + " strict=" + std::to_string(info.strict) + " rowid=" + std::to_string(info.withoutRowid) +
                 "\nsql=" + info.sql);
        for (int k : info.primaryKey)
            log.line("pk " + std::to_string(k));
        for (const ColumnInfo& c : info.columns)
            log.line(c.name + " type=" + std::to_string(int(c.type)) + " decl=" + c.declaredType + " aff=" + c.affinity +
                     " coll=" + c.collation + " pk=" + std::to_string(c.primaryKey) + " nn=" + std::to_string(c.notNull) +
                     " u=" + std::to_string(c.unique) + " def=" + std::to_string(c.hasDefault) + " defsql=" + c.defaultSql +
                     " gen=" + std::to_string(c.generated));
        db.exec("CREATE TABLE k(a INTEGER, b TEXT DEFAULT 'x', c REAL GENERATED ALWAYS AS (a * 2) STORED, PRIMARY KEY (b, a))");
        const TableInfo k = db.table("k");
        for (int p : k.primaryKey)
            log.line("k pk " + std::to_string(p));
        for (const ColumnInfo& c : k.columns)
            log.line(c.name + " def=" + c.defaultValue.toLiteral() + " defsql=" + c.defaultSql + " gen=" + std::to_string(c.generated));
        db.exec("CREATE UNIQUE INDEX k_ab ON k(a, b) WHERE a > 0");
        for (const IndexInfo& ix : db.indexes()) {
            std::string cols;
            for (const std::string& c : ix.columns)
                cols += c + ",";
            log.line("index " + ix.name + " on " + ix.table + " unique=" + std::to_string(ix.unique) + " partial=" + std::to_string(ix.partial) +
                     " cols=" + cols + " sql=" + ix.sql);
        }
        log.line("indexes(person)=" + std::to_string(db.indexes("person").size()));
        log.attempt("missing", [&] { db.table("nope"); });
    });
}

TEST(parity_counters)
{
    parity("counters", [](Database& db, Log& log) {
        log.counters(db);
        db.exec("INSERT INTO tag VALUES ('a', 1), ('b', 2)");
        log.counters(db);
        db.exec("UPDATE tag SET weight = weight + 1");
        log.counters(db);
        db.exec("SELECT * FROM tag");
        log.counters(db);
        db.exec("INSERT INTO person(name) VALUES ('Z')");
        log.counters(db);
        db.exec("DELETE FROM tag");
        log.counters(db);
        db.exec("BEGIN");
        log.counters(db);
        db.exec("COMMIT");
        log.counters(db);
    });
}

TEST(parity_large_result)
{
    parity("large", [](Database& db, Log& log) {
        db.exec("CREATE TABLE big(n INTEGER PRIMARY KEY, s TEXT, r REAL, m)");
        Transaction t = db.begin();
        for (int i = 0; i < 3000; ++i)
            t.exec("INSERT INTO big VALUES (?, ?, ?, ?)",
                   {Value(i), Value("row-" + std::to_string(i % 17) + std::string(i % 50, 'z')), Value(i * 0.125),
                    i % 3 == 0 ? Value(i) : (i % 3 == 1 ? Value("m" + std::to_string(i)) : Value())});
        t.commit();
        const Result r = db.exec("SELECT n, s, r, m FROM big ORDER BY n");
        std::uint64_t hash = 1469598103934665603ull;
        for (const Row& row : r)
            for (const Value& v : row)
                for (char c : v.toLiteral())
                    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        log.line("rows=" + std::to_string(r.size()) + " hash=" + std::to_string(hash));
        Recorder skipped(log, 1);
        db.exec("SELECT n FROM big", Params{}, skipped);
    });
}

TEST(parity_bufs_writer)
{
    parity("bufs", [](Database& db, Log& log) {
        BufsWriter writer;
        const BufsBytes bytes = writer.exec(db, "SELECT id, name, age, score, born, photo FROM person ORDER BY id");
        std::ostringstream hex;
        for (std::uint8_t b : bytes)
            hex << std::hex << int(b) << ' ';
        log.line("bufs " + std::to_string(bytes.size()) + " bytes: " + hex.str());
        auto view = wire::ResultView::open(bytes.data(), bytes.size());
        log.line("rows=" + std::to_string(view.rowCount()));
        BufsWriter batched(BufsOptions().batchRows(2));
        const BufsBytes b2 = batched.exec(db, "SELECT id FROM person");
        log.line("batched " + std::to_string(wire::ResultView::open(b2.data(), b2.size()).batchCount()));
    });
}

TEST(parity_open_handles)
{
    // openReader / openHandle on a remote database open another session on the same server.
    remote_test::Served served;
    seed(served.db);
    Database db = served.connect();
    Database reader = db.openReader();
    Database writer = db.openHandle();
    CHECK(reader.remote());
    CHECK_EQ(tst::only(reader.exec("SELECT count(*) FROM person")), std::string("3"));
    CHECK_THROWS(reader.exec("DELETE FROM person"), ErrorCode::ReadOnly);
    writer.exec("INSERT INTO person(name) VALUES ('shared')");
    CHECK_EQ(tst::only(db.exec("SELECT count(*) FROM person")), std::string("4"));
}

int main()
{
    return tst::runAll("remote_parity");
}
