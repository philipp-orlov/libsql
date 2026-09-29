// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Long-run behaviour.
//
// A database is expected to stay up for months, so "does not grow" is a
// correctness property rather than a nicety: a leak of one index entry per
// update is invisible for an afternoon and fatal after a week.
//
// Two things are measured here, and they fail in different ways:
//
//   live bytes   what the program has allocated and not yet freed. Growth here
//                is a leak, and the counters below come from a replaced
//                operator new so the figure is exact.
//   heap bytes   what the allocator holds from the OS. This can grow while live
//                bytes stay flat, which is what fragmentation looks like.
//
// The store's own mapping does not go through malloc, so neither figure is
// polluted by however much of the file happens to be paged in; the file is
// watched separately, on disk.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "sql/json.hpp"
#include "sql_test.hpp"

namespace {

std::atomic<long long> gLiveBytes{0};
std::atomic<long long> gLiveBlocks{0};

/// Every block carries its size just ahead of the pointer handed out, so that
/// the plain (unsized) delete can still account for it. One max_align_t keeps
/// the payload as aligned as malloc would have.
constexpr std::size_t kHeader = alignof(std::max_align_t);

void* tracked(std::size_t size)
{
    auto* raw = static_cast<std::byte*>(std::malloc(size + kHeader));
    if (!raw)
        throw std::bad_alloc();
    std::memcpy(raw, &size, sizeof size);
    gLiveBytes.fetch_add(static_cast<long long>(size), std::memory_order_relaxed);
    gLiveBlocks.fetch_add(1, std::memory_order_relaxed);
    return raw + kHeader;
}

void untracked(void* pointer) noexcept
{
    if (!pointer)
        return;
    auto* raw = static_cast<std::byte*>(pointer) - kHeader;
    std::size_t size = 0;
    std::memcpy(&size, raw, sizeof size);
    gLiveBytes.fetch_sub(static_cast<long long>(size), std::memory_order_relaxed);
    gLiveBlocks.fetch_sub(1, std::memory_order_relaxed);
    std::free(raw);
}

}  // namespace

// Every plain form has to be replaced together: the standard lets a block from
// the nothrow new come back through the sized delete, so replacing only some of
// them hands a header-carrying pointer to a deallocator that knows nothing
// about it. The over-aligned forms are left alone, which is safe because they
// only ever pair with each other.
void* operator new(std::size_t size)
{
    return tracked(size ? size : 1);
}
void* operator new[](std::size_t size)
{
    return tracked(size ? size : 1);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return tracked(size ? size : 1);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return tracked(size ? size : 1);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept
{
    untracked(p);
}
void operator delete[](void* p) noexcept
{
    untracked(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    untracked(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    untracked(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    untracked(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    untracked(p);
}

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

struct Snapshot
{
    long long liveBytes = 0;
    long long liveBlocks = 0;
    long long heapBytes = 0;  ///< 0 when the allocator will not say
};

Snapshot sample()
{
    Snapshot s;
    s.liveBytes = gLiveBytes.load(std::memory_order_relaxed);
    s.liveBlocks = gLiveBlocks.load(std::memory_order_relaxed);
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
    const struct mallinfo2 info = mallinfo2();
    s.heapBytes = static_cast<long long>(info.arena) + static_cast<long long>(info.hblkhd);
#endif
#endif
    return s;
}

void report(const char* what, const Snapshot& warm, const Snapshot& late)
{
    std::printf("        %-34s live %+lld B / %+lld blocks, heap %+lld B\n", what,
                late.liveBytes - warm.liveBytes, late.liveBlocks - warm.liveBlocks,
                late.heapBytes - warm.heapBytes);
}

/// After a warm-up the working set is fixed, so a long run may not add to it.
/// The slack absorbs lazily grown allocator bookkeeping, not per-iteration
/// growth: the second phase does an order of magnitude more work than the
/// first, so anything proportional to iterations blows straight past it.
void checkStable(const char* what, const Snapshot& warm, const Snapshot& late,
                 long long slackBytes = 256 * 1024)
{
    report(what, warm, late);
    if (kSanitized)
        return;  // too short to have plateaued, and the wrong allocator anyway
    CHECK(late.liveBytes - warm.liveBytes <= slackBytes);
    CHECK(late.liveBlocks - warm.liveBlocks <= 512);
    if (warm.heapBytes && late.heapBytes)
        CHECK(late.heapBytes - warm.heapBytes <= slackBytes);
}

using namespace sql;

/// Long runs are the point, but not on every build. LIBSQL_SOAK_SCALE
/// multiplies the measured phase, so the same test is a quick check by default
/// and an overnight soak when asked.
int scale()
{
    static const int value = [] {
        const char* env = std::getenv("LIBSQL_SOAK_SCALE");
        const int n = env ? std::atoi(env) : 1;
        return n > 0 ? n : 1;
    }();
    return value;
}

/// Iterations for one measured phase. A sanitizer charges so much per
/// allocation that the full workload takes hours, and it replaces the allocator
/// whose footprint these figures are about -- so under one, this runs a short
/// version to hunt for memory errors and leaves the measuring to the ordinary
/// build.
int rounds(int n)
{
    const int wanted = n * scale();
    return kSanitized ? std::max(1, wanted / 200) : wanted;
}

/// Same, for the phase that only has to reach the plateau.
int warmup(int n)
{
    return kSanitized ? std::max(1, n / 20) : n;
}

Database schema(const tst::Scratch& s)
{
    Database db = Database::configure().durable(false).open(s.file());
    db.exec("CREATE TABLE t ("
            "  id   INTEGER PRIMARY KEY,"
            "  a    INTEGER,"
            "  b    TEXT,"
            "  c    REAL,"
            "  d    DATETIME,"
            "  blob BLOB)");
    db.exec("CREATE INDEX t_by_a ON t (a)");
    db.exec("CREATE INDEX t_by_b ON t (b, a)");
    return db;
}

std::uintmax_t fileSize(const tst::Scratch& s)
{
    return std::filesystem::file_size(s.file());
}

/// Every indexed value must be reachable through its index exactly as often as
/// a full scan finds it. An index entry that outlived its row shows up here
/// long before it shows up as a wrong answer to something a user asked.
void checkIndexesAgreeWithScan(Database& db)
{
    Result grouped = db.exec("SELECT a, COUNT(*) FROM t GROUP BY a ORDER BY a");
    std::int64_t total = 0;
    for (const Row& row : grouped) {
        const Value& a = row[0];
        const std::int64_t expected = row[1].integer();
        total += expected;
        if (a.isNull())
            continue;  // NULLs are not index lookups
        Result viaIndex = db.exec("SELECT id FROM t WHERE a = ?", {a});
        CHECK_EQ(viaIndex.size(), std::size_t(expected));
    }
    CHECK_EQ(db.exec("SELECT COUNT(*) FROM t")[0][0].integer(), total);
}

}  // namespace

TEST(oneOffStatementsDoNotAccumulate)
{
    tst::Scratch s("soak");
    Database db = schema(s);

    // Parse, plan, run and throw away: the path with the most churn in it,
    // because every call rebuilds the token list, the tree and the catalog.
    const auto round = [&](int i) {
        db.exec("INSERT INTO t (a, b, c, d) VALUES (?, ?, ?, ?)",
                {Value(i % 97), Value("row " + std::to_string(i % 31)), Value(i * 0.5),
                 Value("2024-05-01 09:30:00")});
        db.exec("SELECT id, b FROM t WHERE a = ? ORDER BY id LIMIT 5", {Value(i % 97)});
        db.exec("UPDATE t SET c = c + 1 WHERE a = ?", {Value(i % 97)});
        db.exec("DELETE FROM t WHERE id = ?", {Value(i - 200)});
    };

    for (int i = 0; i < warmup(400); ++i)
        round(i);
    const Snapshot warm = sample();
    const int n = rounds(6000);
    for (int i = 400; i < 400 + n; ++i)
        round(i);
    const Snapshot late = sample();

    checkStable("one-off exec", warm, late);
    checkIndexesAgreeWithScan(db);
}

TEST(preparedStatementsDoNotAccumulate)
{
    tst::Scratch s("soak");
    Database db = schema(s);

    Statement insert = db.prepare("INSERT INTO t (a, b, c) VALUES (?, ?, ?)");
    Statement query = db.prepare("SELECT id, b FROM t WHERE a = ? ORDER BY id LIMIT 5");
    Statement update = db.prepare("UPDATE t SET c = c + 1 WHERE a = ?");
    Statement remove = db.prepare("DELETE FROM t WHERE id = ?");

    const auto round = [&](int i) {
        insert.exec({Value(i % 97), Value("row " + std::to_string(i % 31)), Value(i * 0.5)});
        query.exec({Value(i % 97)});
        update.exec({Value(i % 97)});
        remove.exec({Value(i - 200)});
    };

    for (int i = 0; i < warmup(400); ++i)
        round(i);
    const Snapshot warm = sample();
    const int n = rounds(6000);
    for (int i = 400; i < 400 + n; ++i)
        round(i);
    const Snapshot late = sample();

    // Re-running a prepared statement re-plans it in place, so the annotations
    // it writes into its own tree have to settle rather than pile up.
    checkStable("prepared exec", warm, late);
    checkIndexesAgreeWithScan(db);
}

TEST(preparingAndDiscardingStatementsDoesNotAccumulate)
{
    tst::Scratch s("soak");
    Database db = schema(s);
    db.exec("INSERT INTO t (a, b) VALUES (1, 'one'), (2, 'two')");

    // A server that prepares per request rather than once at start-up. Each
    // statement pins its database until it goes, so a missed release would show
    // up as both blocks and bytes climbing.
    const auto round = [&](int i) {
        Statement one = db.prepare("SELECT id, b FROM t WHERE a = ? ORDER BY id");
        Statement two = db.prepare("UPDATE t SET c = ? WHERE a = ?");
        one.exec({Value(i % 2 + 1)});
        two.exec({Value(double(i)), Value(i % 2 + 1)});
    };

    for (int i = 0; i < warmup(200); ++i)
        round(i);
    const Snapshot warm = sample();
    const int n = rounds(4000);
    for (int i = 200; i < 200 + n; ++i)
        round(i);
    const Snapshot late = sample();

    checkStable("prepare and discard", warm, late);
}

TEST(churnKeepsTheFileAndTheIndexesBounded)
{
    tst::Scratch s("soak");
    Database db = schema(s);
    std::mt19937_64 rng(20260909);

    // A bounded key space under random traffic: the contents plateau, so the
    // file and the indexes must too.
    const auto churn = [&](int rounds) {
        for (int r = 0; r < rounds; ++r) {
            Transaction t = db.begin();
            for (int i = 0; i < 20; ++i) {
                const std::int64_t id = std::int64_t(rng() % 2000);
                switch (rng() % 3) {
                    case 0:
                        // There is no INSERT OR REPLACE, so an upsert is the
                        // delete and the insert together in one transaction.
                        t.exec("DELETE FROM t WHERE id = ?", {Value(id)});
                        t.exec("INSERT INTO t (id, a, b) VALUES (?, ?, ?)",
                               {Value(id), Value(std::int64_t(rng() % 50)),
                                Value("v" + std::to_string(rng() % 40))});
                        break;
                    case 1:
                        t.exec("UPDATE t SET a = ?, b = ? WHERE id = ?",
                               {Value(std::int64_t(rng() % 50)),
                                Value("v" + std::to_string(rng() % 40)), Value(id)});
                        break;
                    default: t.exec("DELETE FROM t WHERE id = ?", {Value(id)}); break;
                }
            }
            t.commit();
        }
    };

    churn(warmup(200));
    const Snapshot warmHeap = sample();
    const std::uintmax_t warmFile = fileSize(s);
    checkIndexesAgreeWithScan(db);

    churn(rounds(1000));
    const Snapshot lateHeap = sample();
    const std::uintmax_t lateFile = fileSize(s);

    std::printf("        %-34s file %ju -> %ju bytes\n", "churn", (std::uintmax_t)warmFile,
                (std::uintmax_t)lateFile);
    checkStable("churn", warmHeap, lateHeap);
    // Five times the traffic, so anything that leaked per operation would be
    // nowhere near a doubling.
    CHECK(lateFile < warmFile * 2);
    checkIndexesAgreeWithScan(db);
}

TEST(aGrowingDatabaseConvergesRatherThanKeepingPace)
{
    tst::Scratch s("soak");
    Database db = schema(s);

    // Every other test here holds the data size still. This one lets it grow,
    // where a larger working set is legitimate: a deeper tree dirties more
    // pages per commit, and the store keeps some of those buffers to reuse.
    // What must not happen is cost that keeps pace with the traffic, so the
    // same work is done twice and the second half must not cost more.
    Statement insert = db.prepare("INSERT INTO t (a, b) VALUES (?, ?)");
    const int perBatch = warmup(2000);
    int next = 0;
    const auto batches = [&](int count) {
        for (int b = 0; b < count; ++b) {
            Transaction t = db.begin();
            for (int i = 0; i < perBatch; ++i, ++next)
                insert.exec(t, {Value(next % 500), Value("k" + std::to_string(next))});
            t.commit();
        }
    };

    batches(4);
    const Snapshot start = sample();
    const int half = rounds(20);
    batches(half);
    const Snapshot middle = sample();
    batches(half);
    const Snapshot end = sample();

    const long long firstHeap = middle.heapBytes - start.heapBytes;
    const long long secondHeap = end.heapBytes - middle.heapBytes;
    const long long firstLive = middle.liveBytes - start.liveBytes;
    const long long secondLive = end.liveBytes - middle.liveBytes;
    std::printf("        %-34s heap %+lld then %+lld B, live %+lld then %+lld B\n",
                "equal halves of a growing load", firstHeap, secondHeap, firstLive, secondLive);

    CHECK_EQ(db.exec("SELECT COUNT(*) FROM t")[0][0].integer(),
             std::int64_t((4 + 2 * half) * perBatch));
    if (kSanitized)
        return;  // too short to have converged, and the wrong allocator anyway
    CHECK(secondHeap <= firstHeap);
    CHECK(secondLive <= firstLive);
    CHECK(end.liveBlocks - start.liveBlocks <= 512);
}

TEST(largeValuesReleaseTheirBuffers)
{
    tst::Scratch s("soak");
    Database db = schema(s);

    Blob payload(512 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = std::byte(i & 0xff);

    Statement store = db.prepare("INSERT INTO t (id, a, blob) VALUES (?, 1, ?)");
    Statement replace = db.prepare("UPDATE t SET blob = ? WHERE id = ?");
    Statement fetch = db.prepare("SELECT blob FROM t WHERE id = ?");
    store.exec({Value(1), Value(payload)});

    const auto round = [&] {
        replace.exec({Value(payload), Value(1)});
        CHECK_EQ(fetch.exec({Value(1)})[0][0].blob().size(), payload.size());
    };

    for (int i = 0; i < warmup(20); ++i)
        round();
    const Snapshot warm = sample();
    const std::uintmax_t warmFile = fileSize(s);
    for (int i = 0; i < rounds(400); ++i)
        round();
    const Snapshot late = sample();

    std::printf("        %-34s file %ju -> %ju bytes\n", "512 KiB rewrite",
                (std::uintmax_t)warmFile, (std::uintmax_t)fileSize(s));
    // A half-megabyte value is in flight at the plateau, so the slack has to
    // cover one of them rather than none.
    checkStable("512 KiB rewrite", warm, late, 2 * 1024 * 1024);
    CHECK(fileSize(s) < warmFile * 4);
}

TEST(wideResultsAndJoinsReleaseWhatTheyRead)
{
    tst::Scratch s("soak");
    Database db = schema(s);
    db.exec("CREATE TABLE u (id INTEGER PRIMARY KEY, t_id INTEGER, label TEXT)");
    db.exec("CREATE INDEX u_by_t ON u (t_id)");
    const int rows = warmup(1000);
    {
        Transaction t = db.begin();
        Statement addT = db.prepare("INSERT INTO t (id, a, b) VALUES (?, ?, ?)");
        Statement addU = db.prepare("INSERT INTO u (t_id, label) VALUES (?, ?)");
        for (int i = 0; i < rows; ++i) {
            addT.exec(t, {Value(i), Value(i % 25), Value("name " + std::to_string(i))});
            addU.exec(t, {Value(i), Value("label " + std::to_string(i))});
        }
        t.commit();
    }

    Statement joined = db.prepare("SELECT x.b, u.label FROM t x JOIN u ON u.t_id = x.id"
                                  " WHERE x.a = ? ORDER BY x.b");
    Statement rolled = db.prepare("SELECT x.a, COUNT(*), MAX(x.b) FROM t x JOIN u ON u.t_id = x.id"
                                  " GROUP BY x.a ORDER BY x.a");

    const auto round = [&](int i) {
        const int a = i % 25;
        CHECK_EQ(joined.exec({Value(a)}).size(), std::size_t((rows - a + 24) / 25));
        CHECK_EQ(rolled.exec().size(), std::size_t(std::min(rows, 25)));
    };

    for (int i = 0; i < warmup(20); ++i)
        round(i);
    const Snapshot warm = sample();
    const int n = rounds(600);
    for (int i = 20; i < 20 + n; ++i)
        round(i);
    const Snapshot late = sample();

    checkStable("join + group by", warm, late);
}

TEST(jsonRenderingSettles)
{
    tst::Scratch s("soak");
    Database db = schema(s);
    db.exec("CREATE TABLE u (id INTEGER PRIMARY KEY, t_id INTEGER, label TEXT)");
    db.exec("CREATE INDEX u_by_t ON u (t_id)");
    const int kGroups = 25;  ///< how many distinct documents the round cycles through
    const int rows = warmup(400);
    {
        Transaction t = db.begin();
        Statement addT = db.prepare("INSERT INTO t (id, a, b, c, d, blob)"
                                    " VALUES (?, ?, ?, ?, ?, ?)");
        Statement addU = db.prepare("INSERT INTO u (t_id, label) VALUES (?, ?)");
        for (int i = 0; i < rows; ++i) {
            addT.exec(t, {Value(i), Value(i % kGroups),
                          Value("name \"" + std::to_string(i) + "\""), Value(i * 1.5),
                          Value(parseDatetime("2024-01-02 03:04:05")),
                          Value::blob("payload", 7)});
            addU.exec(t, {Value(i), Value("label " + std::to_string(i))});
        }
        t.commit();
    }

    Statement viaClause = db.prepare("SELECT x.b, u.label, x.c, x.d, x.blob"
                                     " FROM t x JOIN u ON u.t_id = x.id"
                                     " WHERE x.a = ? ORDER BY x.id"
                                     " FOR JSON AUTO, ROOT('rows'), INCLUDE_NULL_VALUES");
    Statement plain = db.prepare("SELECT x.b AS b, u.label AS label FROM t x"
                                 " JOIN u ON u.t_id = x.id WHERE x.a = ? ORDER BY x.id");
    // The writer a long-running caller would keep, and the buffer it would own.
    JsonWriter writer(JsonOptions().mode(JsonMode::Path));
    std::string body;

    const auto round = [&](int i) {
        const Value a(i % kGroups);
        CHECK(viaClause.exec({a})[0][0].text().size() > 2);

        const Result rows2 = plain.exec({a});
        CHECK_EQ(writer.measure(rows2), writer.write(rows2).size());

        body.clear();
        writer.appendTo(rows2, body);
        CHECK(body.front() == '[');
    };

    // The warm-up has to cover the whole cycle of query shapes, or the buffer
    // plateaus during the measured phase instead of before it.
    const int warmRounds = std::max(warmup(50), 2 * kGroups);

    for (int i = 0; i < warmRounds; ++i)
        round(i);
    const Snapshot warm = sample();
    const std::size_t warmCapacity = writer.capacity();
    const int n = rounds(4000);
    for (int i = warmRounds; i < warmRounds + n; ++i)
        round(i);
    const Snapshot late = sample();

    // Neither the writer's own buffer nor the caller's grows past what the
    // warm-up already asked for: the document is measured before it is written.
    CHECK_EQ(writer.capacity(), warmCapacity);
    checkStable("for json", warm, late);
}

TEST(transactionsAndSchemaChangesSettle)
{
    tst::Scratch s("soak");
    Database db = Database::configure().durable(false).open(s.file());

    const auto cycle = [&](int r) {
        db.exec("CREATE TABLE scratchpad (id INTEGER PRIMARY KEY, v TEXT)");
        db.exec("CREATE INDEX scratchpad_by_v ON scratchpad (v)");
        {
            Transaction t = db.begin();
            for (int i = 0; i < 50; ++i)
                t.exec("INSERT INTO scratchpad (v) VALUES (?)",
                       {Value("x" + std::to_string(r + i))});
            t.commit();
        }
        {
            Transaction t = db.beginRead();
            CHECK_EQ(t.exec("SELECT COUNT(*) FROM scratchpad")[0][0].integer(), std::int64_t(50));
        }
        db.exec("DROP TABLE scratchpad");
    };

    for (int r = 0; r < warmup(20); ++r)
        cycle(r);
    const Snapshot warm = sample();
    const std::uintmax_t warmFile = fileSize(s);
    const int n = rounds(200);
    for (int r = 20; r < 20 + n; ++r)
        cycle(r);
    const Snapshot late = sample();

    std::printf("        %-34s file %ju -> %ju bytes\n", "create/drop", (std::uintmax_t)warmFile,
                (std::uintmax_t)fileSize(s));
    checkStable("create/drop cycles", warm, late);
    CHECK(fileSize(s) < warmFile * 2);
    CHECK(db.tables().empty());
}

int main()
{
    return tst::runAll("soak");
}
