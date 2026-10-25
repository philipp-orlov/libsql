// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The ResultConsumer hook: exec(text, params, consumer) streams the rows of
// the last statement to a consumer, with the transaction rules of exec().

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>

#include "sql_test.hpp"

namespace {
std::atomic<std::uint64_t> gNews{0};
std::atomic<bool> gCounting{false};
}  // namespace

void* operator new(std::size_t n)
{
    if (gCounting.load(std::memory_order_relaxed))
        gNews.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n)
{
    return ::operator new(n);
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

using namespace sql;

namespace {

/// Logs what the consumer is told: "begin(id,name) row(1,Ada) row(2,Bo) end(0,0)".
class RecordingConsumer final : public ResultConsumer
{
public:
    explicit RecordingConsumer(std::size_t stopAfter = SIZE_MAX) : stopAfter_(stopAfter) {}

    void begin(const ResultShape& shape) override
    {
        ++begins;
        names = shape.names;
        qualified = shape.qualified;
        declTypes = shape.declTypes;
        log += "begin(";
        for (std::size_t i = 0; i < shape.names.size(); ++i)
            log += (i ? "," : "") + shape.names[i];
        log += ")";
    }
    bool row(const RowView& row) override
    {
        ++rows;
        log += " row(";
        for (std::size_t i = 0; i < row.size(); ++i)
            log += (i ? "," : "") + (row[i].isNull() ? std::string("NULL") : row[i].toText());
        log += ")";
        return rows < stopAfter_;
    }
    void end(std::uint64_t c, std::int64_t id) override
    {
        ++ends;
        changes = c;
        lastId = id;
        log += (log.empty() ? "" : " ") + std::string("end(") + std::to_string(c) + "," + std::to_string(id) + ")";
    }

    std::string log;
    std::vector<std::string> names, qualified, declTypes;
    int begins = 0, ends = 0;
    std::size_t rows = 0;
    std::uint64_t changes = 0;
    std::int64_t lastId = 0;

private:
    std::size_t stopAfter_;
};

/// A consumer that throws from row().
struct Thrower final : ResultConsumer
{
    void begin(const ResultShape&) override {}
    bool row(const RowView&) override { throw std::runtime_error("consumer failed"); }
    void end(std::uint64_t, std::int64_t) override {}
};

/// Copies what it is given and allocates nothing once warm.
struct Summer final : ResultConsumer
{
    void begin(const ResultShape& shape) override { width = shape.names.size(); }
    bool row(const RowView& r) override
    {
        for (const Value& v : r)
            if (v.type() == Type::Integer)
                sum += v.integer();
        ++rows;
        return true;
    }
    void end(std::uint64_t, std::int64_t) override { ++ends; }
    std::size_t width = 0, rows = 0, ends = 0;
    std::int64_t sum = 0;
};

Database people(const tst::Scratch& s)
{
    Database db = s.open();
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, at DATETIME)");
    db.exec("INSERT INTO t VALUES (1,'Ada','2024-01-01'),(2,'Bo','2024-01-02'),(3,NULL,NULL)");
    return db;
}

}  // namespace

TEST(consume_select)
{
    tst::Scratch s("consume");
    Database db = people(s);
    RecordingConsumer c;
    const std::uint64_t changes = db.exec("SELECT id, name FROM t ORDER BY id", Params{}, c);
    CHECK_EQ(changes, std::uint64_t(0));
    CHECK_EQ(c.log, std::string("begin(id,name) row(1,Ada) row(2,Bo) row(3,NULL) end(0,0)"));
    CHECK_EQ(c.qualified.size(), std::size_t(2));
    CHECK_EQ(c.qualified[0], std::string("t.id"));
    CHECK_EQ(c.qualified[1], std::string("t.name"));

    const Result r = db.exec("SELECT id, name FROM t ORDER BY id");
    CHECK_EQ(r.columns().size(), c.names.size());
    CHECK_EQ(r.size(), c.rows);
}

TEST(consume_zero_rows)
{
    tst::Scratch s("consume");
    Database db = people(s);
    RecordingConsumer c;
    db.exec("SELECT id FROM t WHERE 0", Params{}, c);
    CHECK_EQ(c.log, std::string("begin(id) end(0,0)"));
    RecordingConsumer prepared;
    Statement st = db.prepare("SELECT id FROM t WHERE id > ?");
    st.bind(1, Value(99));
    st.exec(prepared);
    CHECK_EQ(prepared.log, std::string("begin(id) end(0,0)"));
}

TEST(consume_decl_types)
{
    tst::Scratch s("consume");
    Database db = s.open();
    db.exec("CREATE TABLE t(a INTEGER, b TEXT, c DATETIME)");
    db.exec("INSERT INTO t VALUES (1, 'x', '2024-01-01')");
    RecordingConsumer c;
    db.exec("SELECT a, b, c, a + 1 FROM t", Params{}, c);
    CHECK_EQ(c.declTypes.size(), std::size_t(4));
    CHECK_EQ(c.declTypes[0], std::string("INTEGER"));
    CHECK_EQ(c.declTypes[1], std::string("TEXT"));
    CHECK_EQ(c.declTypes[2], std::string("DATETIME"));
    CHECK_EQ(c.declTypes[3], std::string());
}

TEST(consume_stop_early)
{
    tst::Scratch s("consume");
    Database db = s.open();
    db.exec("CREATE TABLE t(n INTEGER)");
    db.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 10) INSERT INTO t SELECT n FROM c");
    RecordingConsumer c(2);
    db.exec("SELECT n FROM t ORDER BY n", Params{}, c);
    CHECK_EQ(c.rows, std::size_t(2));
    CHECK_EQ(c.ends, 1);
    CHECK_EQ(c.log, std::string("begin(n) row(1) row(2) end(0,0)"));
}

TEST(consume_last_statement_only)
{
    tst::Scratch s("consume");
    Database db = people(s);
    RecordingConsumer c;
    db.exec("INSERT INTO t VALUES (4,'Cy',NULL); SELECT id FROM t WHERE id > 2 ORDER BY id", Params{}, c);
    CHECK_EQ(c.log, std::string("begin(id) row(3) row(4) end(0,0)"));
    CHECK_EQ(c.begins, 1);
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM t")), std::string("4"));
}

TEST(consume_no_columns)
{
    tst::Scratch s("consume");
    Database db = people(s);
    RecordingConsumer c;
    const std::uint64_t changes = db.exec("UPDATE t SET name = 'z' WHERE id < 3", Params{}, c);
    CHECK_EQ(changes, std::uint64_t(2));
    CHECK_EQ(c.begins, 0);
    CHECK_EQ(c.rows, std::size_t(0));
    CHECK_EQ(c.log, std::string("end(2,0)"));
    RecordingConsumer ddl;
    db.exec("CREATE TABLE u(x)", Params{}, ddl);
    CHECK_EQ(ddl.log, std::string("end(0,0)"));
}

TEST(consume_returning)
{
    tst::Scratch s("consume");
    Database db = people(s);
    RecordingConsumer c;
    const std::uint64_t changes = db.exec("INSERT INTO t(name) VALUES ('Di'),('Ed') RETURNING id", Params{}, c);
    CHECK_EQ(changes, std::uint64_t(2));
    CHECK_EQ(c.log, std::string("begin(id) row(4) row(5) end(2,5)"));
    CHECK_EQ(c.declTypes.size(), std::size_t(1));
}

TEST(consume_params)
{
    tst::Scratch s("consume");
    Database db = people(s);
    RecordingConsumer c;
    db.exec("SELECT name FROM t WHERE id = ?", Params{Value(2)}, c);
    CHECK_EQ(c.log, std::string("begin(name) row(Bo) end(0,0)"));
    RecordingConsumer named;
    db.exec("SELECT name FROM t WHERE id = :id", NamedParams{{":id", Value(1)}}, named);
    CHECK_EQ(named.log, std::string("begin(name) row(Ada) end(0,0)"));
    CHECK_THROWS(db.exec("SELECT name FROM t WHERE id = ?", Params{}, named), ErrorCode::InvalidArgument);
}

TEST(consume_transactions)
{
    tst::Scratch s("consume");
    Database db = people(s);
    // SQL transaction control in the text.
    RecordingConsumer a;
    db.exec("BEGIN; INSERT INTO t VALUES (10,'Q',NULL); SELECT id FROM t WHERE id = 10", Params{}, a);
    CHECK_EQ(a.log, std::string("begin(id) row(10) end(0,0)"));
    RecordingConsumer b;
    db.exec("ROLLBACK", Params{}, b);  // the last statement is control: still an end()
    CHECK_EQ(b.log, std::string("end(0,0)"));
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM t WHERE id = 10")), std::string("0"));

    // Inside a Transaction object.
    Transaction txn = db.begin();
    RecordingConsumer c;
    txn.exec("INSERT INTO t VALUES (11,'W',NULL); SELECT name FROM t WHERE id = 11", Params{}, c);
    CHECK_EQ(c.log, std::string("begin(name) row(W) end(0,0)"));
    RecordingConsumer n;
    txn.exec("SELECT name FROM t WHERE id = :i", NamedParams{{":i", Value(11)}}, n);
    CHECK_EQ(n.log, std::string("begin(name) row(W) end(0,0)"));
    txn.rollback();
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM t WHERE id = 11")), std::string("0"));

    // Through a Statement with bound params, alone and in a Transaction.
    Statement st = db.prepare("SELECT name FROM t WHERE id = ?");
    st.bind(1, Value(1));
    RecordingConsumer d;
    st.exec(d);
    CHECK_EQ(d.log, std::string("begin(name) row(Ada) end(0,0)"));
    Transaction t2 = db.beginRead();
    st.bind(1, Value(2));
    RecordingConsumer e;
    st.exec(t2, e);
    CHECK_EQ(e.log, std::string("begin(name) row(Bo) end(0,0)"));
}

TEST(consume_throw)
{
    tst::Scratch s("consume");
    Database db = people(s);
    Thrower thrower;
    bool caught = false;
    try {
        db.exec("INSERT INTO t VALUES (20,'X',NULL); SELECT id FROM t", Params{}, thrower);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
    // One transaction for the text: the INSERT went with it.
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM t")), std::string("3"));
    CHECK_EQ(tst::only(db.exec("SELECT COUNT(*) FROM t WHERE id = 20")), std::string("0"));
}

TEST(consume_error_has_no_end)
{
    tst::Scratch s("consume");
    Database db = people(s);
    RecordingConsumer c;
    bool caught = false;
    try {
        db.exec("SELECT * FROM nonexistent", Params{}, c);
    } catch (const Error&) {
        caught = true;
    }
    CHECK(caught);
    CHECK_EQ(c.ends, 0);
}

TEST(consume_borrowed_text)
{
    tst::Scratch s("consume");
    Database db = s.open();
    db.exec("CREATE TABLE big(id INTEGER PRIMARY KEY, body TEXT)");
    std::vector<std::string> expected;
    for (int i = 0; i < 20; ++i) {
        expected.push_back(std::string(20000, char('a' + i)));
        db.exec("INSERT INTO big VALUES (?, ?)", Params{Value(i), Value(expected.back())});
    }
    struct Copier final : ResultConsumer
    {
        void begin(const ResultShape&) override {}
        bool row(const RowView& r) override
        {
            copies.emplace_back(r[1].text());  // copy inside row(): the view may borrow from the file
            return true;
        }
        void end(std::uint64_t, std::int64_t) override {}
        std::vector<std::string> copies;
    } copier;
    db.exec("SELECT id, body FROM big ORDER BY id", Params{}, copier);
    CHECK_EQ(copier.copies.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK(copier.copies[i] == expected[i]);
}

TEST(consume_no_alloc)
{
    tst::Scratch s("consume");
    Database db = s.open();
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, n INTEGER, name TEXT)");
    for (int i = 1; i <= 50; ++i)
        db.exec("INSERT INTO t VALUES (?, ?, 'row')", Params{Value(i), Value(i * 3)});
    Summer sum;
    Statement st = db.prepare("SELECT id, n, name FROM t WHERE id > ?");
    st.bind(1, Value(10));
    Transaction txn = db.beginRead();
    for (int i = 0; i < 20; ++i)
        st.exec(txn, sum);  // warm
    gNews = 0;
    gCounting = true;
    for (int i = 0; i < 1000; ++i)
        st.exec(txn, sum);
    gCounting = false;
    CHECK_EQ(gNews.load(), std::uint64_t(0));
    CHECK_EQ(sum.width, std::size_t(3));
    CHECK_EQ(sum.rows, std::size_t(1020 * 40));
    CHECK_EQ(sum.ends, std::size_t(1020));
}

int main()
{
    return tst::runAll("consume");
}
