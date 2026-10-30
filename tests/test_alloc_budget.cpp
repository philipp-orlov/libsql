// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The allocation budget of isvc's statements (its DDL and statement shapes):
// a warm statement allocates nothing from the heap, and a long run of
// enrollments, searches and removals keeps the heap flat. Counts every
// global operator new while a measurement runs.

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

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
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    return ::operator new(n, std::nothrow);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    std::free(p);
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

using Embedding = std::array<float, 128>;  // 512 bytes, as isvc stores them

/// Heap allocations per call of `fn`, after `warm` calls that are not counted.
template <class F>
double allocationsPer(int warm, int reps, F&& fn)
{
    for (int i = 0; i < warm; ++i)
        fn();
    gNews = 0;
    gCounting = true;
    for (int i = 0; i < reps; ++i)
        fn();
    gCounting = false;
    return double(gNews.load()) / reps;
}

/// isvc's metrics store: its tables, indexes and statements.
struct Metrics
{
    explicit Metrics(const tst::Scratch& s, Database::Options options = Database::configure())
        : db(options.createIfMissing().open(s.file()))
    {
        db.exec(
            "CREATE TABLE IF NOT EXISTS record ("
            "  id INTEGER PRIMARY KEY, text TEXT NOT NULL, jurisdiction INTEGER NOT NULL,"
            "  category INTEGER NOT NULL, token INTEGER)");
        db.exec(
            "CREATE TABLE IF NOT EXISTS embedding ("
            "  record_id INTEGER NOT NULL, slot INTEGER NOT NULL, vector BLOB NOT NULL,"
            "  PRIMARY KEY (record_id, slot)) WITHOUT ROWID");
        db.exec(
            "CREATE UNIQUE INDEX IF NOT EXISTS record_key ON record (text, jurisdiction, "
            "category)");
        db.exec("CREATE UNIQUE INDEX IF NOT EXISTS record_by_token ON record (token)");
        findRecord = db.prepare(
            "SELECT id, token FROM record WHERE text = ? AND jurisdiction = ? AND category = ?");
        vectorsOf = db.prepare("SELECT vector FROM embedding WHERE record_id = ? ORDER BY slot");
        topSlot = db.prepare("SELECT MAX(slot) FROM embedding WHERE record_id = ?");
        countSamples = db.prepare("SELECT COUNT(*) FROM embedding WHERE record_id = ?");
        insertRecord = db.prepare(
            "INSERT INTO record (text, jurisdiction, category, token) VALUES (?, ?, ?, ?)");
        insertSample =
            db.prepare("INSERT INTO embedding (record_id, slot, vector) VALUES (?, ?, ?)");
        deleteSamples = db.prepare("DELETE FROM embedding WHERE record_id = ?");
        deleteRecord = db.prepare("DELETE FROM record WHERE id = ?");
        for (std::size_t i = 0; i < sample.size(); ++i)
            sample[i] = float(i) * 0.01f;
    }

    /// `records` records of `samples` samples each, as isvc's add() writes them.
    void populate(int records, int samples)
    {
        for (int r = 0; r < records; ++r) {
            Transaction txn = db.begin();
            const std::string text = "ABC" + std::to_string(1000 + r);
            const std::int64_t id =
                insertRecord
                    .exec(txn, {Value(text), Value(1), Value(2), Value(std::int64_t(100000 + r))})
                    .lastInsertId();
            for (int s = 0; s < samples; ++s)
                insertSample.exec(txn, {Value(id), Value(std::int64_t(s)),
                                        Value::blobRef(sample.data(), sizeof sample)});
            txn.commit();
        }
    }

    Database db;
    Statement findRecord, vectorsOf, topSlot, countSamples, insertRecord, insertSample,
        deleteSamples, deleteRecord;
    Embedding sample{};
};

}  // namespace

TEST(eachInKeyOrderAllocatesNothing)
{
    tst::Scratch s("alloc-each");
    Metrics m(s);
    m.populate(20, 50);
    std::int64_t lastSlot = -1;
    std::size_t rows = 0;
    bool ordered = true, borrowed = true;
    const double perSearch = allocationsPer(20, 200, [&] {
        Transaction txn = m.db.beginRead();
        lastSlot = -1;
        rows = 0;
        m.vectorsOf.bind(1, Value(7)).each(txn, [&](const RowView& row) {
            // The key yields slot order: the ORDER BY needs no sort and the
            // cells stay borrowed.
            ++rows;
            borrowed = borrowed && row[0].borrowed();
            return true;
        });
    });
    CHECK_EQ(perSearch, 0.0);
    CHECK_EQ(rows, std::size_t(50));
    CHECK(borrowed);
    Transaction txn = m.db.beginRead();
    Statement slots = m.db.prepare("SELECT slot FROM embedding WHERE record_id = ? ORDER BY slot");
    slots.bind(1, Value(7)).each(txn, [&](const RowView& row) {
        ordered = ordered && row[0].integer() == lastSlot + 1;
        lastSlot = row[0].integer();
        return true;
    });
    CHECK(ordered);
    CHECK_EQ(lastSlot, std::int64_t(49));
}

TEST(execIntoReusesItsRows)
{
    tst::Scratch s("alloc-into");
    Metrics m(s);
    m.populate(20, 50);
    const std::string key = "ABC1007";
    Result record, vectors, top;
    std::size_t bytes = 0;
    const double perSearch = allocationsPer(20, 200, [&] {
        Transaction txn = m.db.beginRead();
        m.findRecord.bind(1, Value::textRef(key))
            .bind(2, Value(1))
            .bind(3, Value(2))
            .execInto(txn, record);
        m.vectorsOf.bind(1, record[0][0]).execInto(txn, vectors);
        m.topSlot.bind(1, record[0][0]).execInto(txn, top);
        bytes = 0;
        for (const Row& row : vectors)
            bytes += row[0].blob().size();
    });
    CHECK_EQ(perSearch, 0.0);
    CHECK_EQ(record.size(), std::size_t(1));
    CHECK_EQ(record.columns().size(), std::size_t(2));
    CHECK_EQ(vectors.size(), std::size_t(50));
    CHECK_EQ(bytes, 50 * sizeof(Embedding));
    CHECK_EQ(top[0][0].integer(), std::int64_t(49));
    // The rows own their bytes: they outlive the transaction.
    CHECK(!vectors[3][0].borrowed());
    CHECK_EQ(std::memcmp(vectors[3][0].blob().data(), m.sample.data(), sizeof(Embedding)), 0);

    // Another statement into the same Result starts afresh.
    Transaction txn = m.db.beginRead();
    m.countSamples.bind(1, Value(3)).execInto(txn, vectors);
    CHECK_EQ(tst::only(vectors), std::string("50"));
    CHECK_EQ(vectors.columns()[0], std::string("COUNT(*)"));
}

TEST(boundInsertAllocatesAtMostTheCommit)
{
    tst::Scratch s("alloc-insert");
    Metrics m(s);
    m.populate(2, 5);
    std::int64_t slot = 100;
    {
        // The statement alone, in transactions of their own: libnosql's
        // buffers for the pages it dirties are its transaction's, recycled
        // from one to the next.
        std::uint64_t news = 0;
        for (int i = 0; i < 220; ++i) {
            Transaction txn = m.db.begin();
            gNews = 0;
            gCounting = true;
            m.insertSample.exec(
                txn, {Value(1), Value(slot++), Value::blobRef(m.sample.data(), sizeof m.sample)});
            gCounting = false;
            if (i >= 20)
                news += gNews.load();
            txn.commit();
        }
        CHECK_EQ(news, std::uint64_t(0));
    }
    // A commit of its own adds what libnosql's commit allocates (at most one).
    const double perCommit = allocationsPer(20, 200, [&] {
        Transaction txn = m.db.begin();
        m.insertSample.exec(
            txn, {Value(2), Value(slot++), Value::blobRef(m.sample.data(), sizeof m.sample)});
        txn.commit();
    });
    CHECK(perCommit <= 1.0);
    CHECK_EQ(tst::only(m.db.exec("SELECT COUNT(*) FROM embedding")), std::to_string(10 + 440));
}

TEST(heapStaysFlatOverAddSearchRemove)
{
    tst::Scratch s("alloc-flat");
    Metrics m(s, Database::configure().durable(false));
    m.populate(50, 20);
    Result record, top;
    std::string text;
    long atStart = 0;
    for (int cycle = 1; cycle <= 20000; ++cycle) {
        text = "CYC" + std::to_string(cycle % 97);
        std::int64_t id = 0;
        {
            Transaction txn = m.db.begin();
            m.findRecord.bind(1, Value::textRef(text))
                .bind(2, Value(1))
                .bind(3, Value(2))
                .execInto(txn, record);
            if (record.empty())
                id = m.insertRecord.exec(txn, {Value::textRef(text), Value(1), Value(2), Value()})
                         .lastInsertId();
            else
                id = record[0][0].integer();
            m.topSlot.bind(1, Value(id)).execInto(txn, top);
            const std::int64_t next = top[0][0].isNull() ? 0 : top[0][0].integer() + 1;
            m.insertSample.exec(
                txn, {Value(id), Value(next), Value::blobRef(m.sample.data(), sizeof m.sample)});
            txn.commit();
        }
        {
            Transaction txn = m.db.beginRead();
            std::size_t rows = 0;
            m.vectorsOf.bind(1, Value(id)).each(txn, [&](const RowView&) {
                ++rows;
                return true;
            });
            CHECK(rows > 0);
        }
        if (cycle % 5 == 0) {
            Transaction txn = m.db.begin();
            m.deleteSamples.exec(txn, {Value(id)});
            m.deleteRecord.exec(txn, {Value(id)});
            txn.commit();
        }
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
        if (cycle == 4000 || cycle == 20000) {
            const struct mallinfo2 mi = mallinfo2();
            const long inUse = long(mi.uordblks + mi.hblkhd);
            if (cycle == 4000)
                atStart = inUse;
            else
                CHECK(inUse - atStart <= 4096);
        }
#endif
    }
    (void)atStart;
    CHECK_EQ(tst::only(m.db.exec("PRAGMA integrity_check")), std::string("ok"));
}

TEST(readerHonorsTheStatementCacheSize)
{
    // A reader takes its memory options from the Database it came from: with
    // the statement cache off, it parses the same text on every run.
    tst::Scratch a("alloc-reader-a"), b("alloc-reader-b");
    Metrics cached(a), uncached(b, Database::configure().statementCacheSize(0));
    Database ra = cached.db.openReader(), rb = uncached.db.openReader();
    const double withCache =
        allocationsPer(5, 50, [&] { (void)ra.exec("SELECT COUNT(*) FROM record"); });
    const double withoutCache =
        allocationsPer(5, 50, [&] { (void)rb.exec("SELECT COUNT(*) FROM record"); });
    CHECK(withoutCache > withCache + 5);
}

int main()
{
    return tst::runAll("alloc_budget");
}
