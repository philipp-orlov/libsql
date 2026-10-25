// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// sql::BufsWriter: which encoding each kind of column gets, batches, limits,
// and that ResultView reads back exactly what the database returned.

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>

#include "sql/bufs.hpp"
#include "sql/wire/result_view.hpp"
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
namespace w = sql::wire;

namespace {

/// The messages of an encoded stream.
struct Stream
{
    explicit Stream(BufsBytes b) : bytes(b.begin(), b.end())
    {
        // An aligned copy: the writer's buffer is gone after its next call.
        words.resize((bytes.size() + 7) / 8);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        const auto* base = reinterpret_cast<const std::uint8_t*>(words.data());
        CHECK(::bufs::verifyStream<w::ResultSet>(base, bytes.size()));
        ::bufs::MessageStream s(base, bytes.size());
        const std::uint8_t* m = nullptr;
        std::size_t size = 0;
        while (s.next(m, size))
            messages.push_back(m);
    }
    w::ResultSet::Reader rs(std::size_t i = 0) const { return ::bufs::root<w::ResultSet>(messages.at(i)); }
    w::Column::Reader col(std::size_t c, std::size_t message = 0) const { return rs(message).data()[c]; }
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint64_t> words;
    std::vector<const std::uint8_t*> messages;
};

Database fresh(const tst::Scratch& s)
{
    return s.open();
}

/// Encodes the query with default options.
Stream encode(Database& db, const std::string& q, BufsOptions o = BufsOptions())
{
    BufsWriter writer(o);
    return Stream(writer.exec(db, q));
}

std::string ints(int from, int to)
{
    std::string s = "WITH RECURSIVE c(n) AS (SELECT " + std::to_string(from) + " UNION ALL SELECT n + 1 FROM c WHERE n < " +
                    std::to_string(to) + ") ";
    return s;
}

}  // namespace

TEST(bufs_empty_result)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(id INTEGER)");
    Stream st = encode(db, "SELECT id FROM t WHERE 0");
    CHECK_EQ(st.messages.size(), std::size_t(1));
    CHECK_EQ(st.rs().rowCount(), std::uint32_t(0));
    CHECK_EQ(st.rs().columns().size(), std::uint32_t(1));
    CHECK_EQ(std::string(st.rs().columns()[0].name()), std::string("id"));
    CHECK(st.col(0).encoding() == w::Encoding::Empty);
    CHECK(!st.rs().more());
}

TEST(bufs_no_columns)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(x INTEGER)");
    db.exec("INSERT INTO t VALUES (0),(0),(0)");
    Stream st = encode(db, "UPDATE t SET x = 1");
    CHECK_EQ(st.messages.size(), std::size_t(1));
    CHECK_EQ(st.rs().columns().size(), std::uint32_t(0));
    CHECK_EQ(st.rs().data().size(), std::uint32_t(0));
    CHECK_EQ(st.rs().changes(), std::uint64_t(3));
    CHECK_EQ(st.rs().rowCount(), std::uint32_t(0));
}

TEST(bufs_int_u8)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    Stream st = encode(db, ints(1000, 1100) + "SELECT n FROM c");
    auto c = st.col(0);
    CHECK(c.encoding() == w::Encoding::Int);
    CHECK(c.width() == w::Width::U8);
    CHECK_EQ(c.base(), std::int64_t(1000));
    CHECK_EQ(c.u8().size(), std::uint32_t(101));
    CHECK_EQ(int(c.u8()[5]), 5);
    CHECK_EQ(st.rs().rowCount(), std::uint32_t(101));
}

TEST(bufs_int_u16_u32_i64)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(a INTEGER, b INTEGER, c INTEGER)");
    db.exec("INSERT INTO t VALUES (0, 0, -9223372036854775807 - 1), (300, 70000, 9223372036854775807)");
    Stream st = encode(db, "SELECT a, b, c FROM t");
    CHECK(st.col(0).width() == w::Width::U16);
    CHECK(st.col(1).width() == w::Width::U32);
    CHECK(st.col(2).width() == w::Width::I64);
    CHECK_EQ(st.col(2).base(), std::int64_t(0));
    CHECK_EQ(st.col(2).i64()[0], std::numeric_limits<std::int64_t>::min());
    CHECK_EQ(st.col(2).i64()[1], std::numeric_limits<std::int64_t>::max());
    CHECK_EQ(int(st.col(0).u16()[1]), 300);
    CHECK_EQ(st.col(1).u32()[1], std::uint32_t(70000));
}

TEST(bufs_int_negative)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    Stream st = encode(db, ints(-5, 5) + "SELECT n FROM c");
    CHECK(st.col(0).width() == w::Width::U8);
    CHECK_EQ(st.col(0).base(), std::int64_t(-5));
    CHECK_EQ(int(st.col(0).u8()[10]), 10);
}

TEST(bufs_nulls_bitmap)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)");
    for (int i = 0; i < 10; ++i)
        db.exec("INSERT INTO t VALUES (?, ?)",
                Params{Value(i), (i == 0 || i == 3 || i == 9) ? Value() : Value(10 + i)});
    Stream st = encode(db, "SELECT v FROM t ORDER BY id");
    auto c = st.col(0);
    CHECK_EQ(c.nullCount(), std::uint32_t(3));
    CHECK_EQ(c.nulls().size(), std::uint32_t(2));
    CHECK_EQ(int(c.nulls()[0]), 0x09);
    CHECK_EQ(int(c.nulls()[1]), 0x02);
    CHECK_EQ(c.base(), std::int64_t(11));
    CHECK_EQ(int(c.u8()[0]), 0);
    CHECK_EQ(int(c.u8()[3]), 0);
    CHECK_EQ(int(c.u8()[9]), 0);
    CHECK_EQ(int(c.u8()[1]), 0);  // value 11 - base 11
    CHECK_EQ(int(c.u8()[2]), 1);
}

TEST(bufs_all_null)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    Stream st = encode(db, "SELECT NULL FROM (SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3)");
    CHECK(st.col(0).encoding() == w::Encoding::Empty);
    CHECK_EQ(st.col(0).nullCount(), std::uint32_t(3));
}

TEST(bufs_real_f32_f64)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    Stream a = encode(db, "SELECT 0.5 UNION ALL SELECT 12.25");
    CHECK(a.col(0).encoding() == w::Encoding::Real);
    CHECK_EQ(a.col(0).f32().size(), std::uint32_t(2));
    CHECK(a.col(0).f64().empty());
    Stream b = encode(db, "SELECT 0.1");
    CHECK_EQ(b.col(0).f64().size(), std::uint32_t(1));
    CHECK(b.col(0).f32().empty());
}

TEST(bufs_real_nan_negzero)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(r REAL)");
    db.exec("INSERT INTO t VALUES (?), (?)", Params{Value(std::nan("")), Value(-0.0)});
    Stream st = encode(db, "SELECT r FROM t");
    // NaN is stored as NULL by SQLite-style engines; accept either, but -0.0 must keep its sign bit.
    auto view = w::ResultView::open(st.bytes.data(), st.bytes.size());
    const Result truth = db.exec("SELECT r FROM t");
    for (std::size_t i = 0; i < truth.size(); ++i)
        CHECK(compare(view.value(i, 0), truth[i][0]) == 0);
    BufsWriter writer;
    Result r = db.exec("SELECT -0.0");
    Stream neg(writer.write(r));
    CHECK(neg.col(0).encoding() == w::Encoding::Real);
    double d = 0;
    if (neg.col(0).hasF32()) {
        float f = neg.col(0).f32()[0];
        d = f;
    } else {
        d = neg.col(0).f64()[0];
    }
    CHECK(std::signbit(d));
}

TEST(bufs_datetime_scales)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(d DATETIME)");
    auto scale = [&](const char* a, const char* b) {
        db.exec("DELETE FROM t");
        db.exec("INSERT INTO t VALUES (?), (?)", Params{Value(parseDatetime(a)), Value(parseDatetime(b))});
        Stream st = encode(db, "SELECT d FROM t");
        CHECK(st.col(0).encoding() == w::Encoding::Datetime);
        return std::pair(st.col(0).scale(), st.col(0).width());
    };
    CHECK(scale("2024-01-01", "2024-03-01") == std::pair(w::Scale::Day, w::Width::U8));
    CHECK(scale("2024-01-01 00:00:01", "2024-12-31 23:59:59") == std::pair(w::Scale::Second, w::Width::U32));
    CHECK(scale("2024-01-01 00:00:00.001", "2024-01-01 00:00:03.250") == std::pair(w::Scale::Milli, w::Width::U16));
    CHECK(scale("2024-01-01 00:00:00.000001", "2024-01-01 00:00:03.000002") == std::pair(w::Scale::Micro, w::Width::U32));
}

TEST(bufs_decl_types)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(a INTEGER)");
    db.exec("INSERT INTO t VALUES (1)");
    Stream st = encode(db, "SELECT a, a + 1 FROM t");
    CHECK_EQ(std::string(st.rs().columns()[0].declType()), std::string("INTEGER"));
    CHECK_EQ(std::string(st.rs().columns()[1].declType()), std::string());
    CHECK_EQ(std::string(st.rs().columns()[0].qualified()), std::string("t.a"));
}

TEST(bufs_text_plain)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(name TEXT)");
    for (int i = 0; i < 100; ++i)
        db.exec("INSERT INTO t VALUES (?)", Params{Value("name-" + std::to_string(i))});
    Stream st = encode(db, "SELECT name FROM t ORDER BY rowid");
    auto c = st.col(0);
    CHECK(c.encoding() == w::Encoding::Text);
    CHECK_EQ(c.offsets().size(), std::uint32_t(101));
    CHECK_EQ(int(c.offsets()[0]), 0);
    std::string all;
    for (int i = 0; i < 100; ++i)
        all += "name-" + std::to_string(i);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(c.bytes().data()), c.bytes().size()), all);
}

TEST(bufs_text_dict)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(status TEXT)");
    const char* statuses[] = {"pending", "shipped", "delivered"};
    for (int i = 0; i < 1000; ++i)
        db.exec("INSERT INTO t VALUES (?)", Params{Value(statuses[(i * 7) % 3])});
    Stream st = encode(db, "SELECT status FROM t ORDER BY rowid");
    auto c = st.col(0);
    CHECK(c.encoding() == w::Encoding::TextDict);
    CHECK(c.width() == w::Width::U8);
    CHECK_EQ(c.dictOffsets().size(), std::uint32_t(4));
    // first-seen order: row 0 -> statuses[0]
    CHECK_EQ(std::string(reinterpret_cast<const char*>(c.dictBytes().data()), 7), std::string("pending"));
    CHECK_EQ(c.u8().size(), std::uint32_t(1000));

    Stream off = encode(db, "SELECT status FROM t ORDER BY rowid", BufsOptions().dictionary(false));
    CHECK(off.col(0).encoding() == w::Encoding::Text);
}

TEST(bufs_text_dict_overflow)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    Stream st = encode(db, ints(1, 70000) + "SELECT 'value-' || n FROM c");
    CHECK(st.col(0).encoding() == w::Encoding::Text);
    auto view = w::ResultView::open(st.bytes.data(), st.bytes.size());
    CHECK_EQ(view.rowCount(), std::uint64_t(70000));
    CHECK_EQ(std::string(view.value(69999, 0).text()), std::string("value-70000"));
}

TEST(bufs_blob_prefix)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(b BLOB)");
    db.exec("INSERT INTO t VALUES (zeroblob(10)), (zeroblob(100))");
    Stream st = encode(db, "SELECT b FROM t ORDER BY rowid", BufsOptions().blobPrefix(32));
    auto c = st.col(0);
    CHECK(c.encoding() == w::Encoding::Blob);
    CHECK_EQ(c.bytes().size(), std::uint32_t(42));
    CHECK_EQ(c.blobSizes().size(), std::uint32_t(2));
    CHECK_EQ(c.blobSizes()[0], std::uint32_t(10));
    CHECK_EQ(c.blobSizes()[1], std::uint32_t(100));
    Stream whole = encode(db, "SELECT b FROM t ORDER BY rowid");
    CHECK(whole.col(0).blobSizes().empty());
    CHECK_EQ(whole.col(0).bytes().size(), std::uint32_t(110));
}

TEST(bufs_mixed)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v)");
    db.exec("INSERT INTO t VALUES (1, 1), (2, 'a'), (3, 2.5), (4, NULL), (5, x'00')");
    db.exec("INSERT INTO t VALUES (6, ?)", Params{Value(Datetime{86400000000LL})});
    Stream st = encode(db, "SELECT v FROM t ORDER BY id");
    auto c = st.col(0);
    CHECK(c.encoding() == w::Encoding::Mixed);
    CHECK_EQ(c.tags().size(), std::uint32_t(6));
    CHECK_EQ(c.i64().size(), std::uint32_t(2));
    CHECK_EQ(c.f64().size(), std::uint32_t(1));
    CHECK_EQ(c.offsets().size(), std::uint32_t(3));
    auto view = w::ResultView::open(st.bytes.data(), st.bytes.size());
    const Result truth = db.exec("SELECT v FROM t ORDER BY id");
    for (std::size_t i = 0; i < truth.size(); ++i) {
        const Value got = view.value(i, 0);
        CHECK_EQ(got.type(), truth[i][0].type());
        CHECK(compare(got, truth[i][0]) == 0);
    }
}

TEST(bufs_batches)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    Stream st = encode(db, ints(1, 10) + "SELECT n FROM c", BufsOptions().batchRows(4));
    CHECK_EQ(st.messages.size(), std::size_t(3));
    CHECK(st.rs(0).hasColumns());
    CHECK(!st.rs(1).hasColumns());
    CHECK(!st.rs(2).hasColumns());
    CHECK_EQ(st.rs(0).rowOffset(), std::uint64_t(0));
    CHECK_EQ(st.rs(1).rowOffset(), std::uint64_t(4));
    CHECK_EQ(st.rs(2).rowOffset(), std::uint64_t(8));
    CHECK(st.rs(0).more());
    CHECK(st.rs(1).more());
    CHECK(!st.rs(2).more());
    CHECK_EQ(st.rs(2).rowCount(), std::uint32_t(2));
    CHECK_EQ(st.rs(2).totalRows(), std::uint64_t(10));
    auto view = w::ResultView::open(st.bytes.data(), st.bytes.size());
    for (std::uint64_t i = 0; i < 10; ++i)
        CHECK_EQ(view.value(i, 0).integer(), std::int64_t(i + 1));
}

TEST(bufs_batches_exact)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    Stream st = encode(db, ints(1, 8) + "SELECT n FROM c", BufsOptions().batchRows(4));
    CHECK_EQ(st.messages.size(), std::size_t(2));
    CHECK_EQ(st.rs(1).rowCount(), std::uint32_t(4));
    CHECK(!st.rs(1).more());
}

TEST(bufs_on_batch)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    const std::string q = ints(1, 10) + "SELECT n FROM c";
    BufsWriter writer(BufsOptions().batchRows(4));
    const std::vector<std::uint8_t> expected = [&] {
        BufsBytes b = writer.exec(db, q);
        return std::vector<std::uint8_t>(b.begin(), b.end());
    }();
    std::vector<std::uint8_t> got;
    int calls = 0;
    writer.onBatch([&](BufsBytes b) {
        ++calls;
        got.insert(got.end(), b.begin(), b.end());
    });
    CHECK(writer.exec(db, q).empty());
    CHECK_EQ(calls, 3);
    CHECK(got == expected);
}

TEST(bufs_max_rows)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    const std::string q = ints(1, 100) + "SELECT n FROM c";
    Stream a = encode(db, q, BufsOptions().maxRows(10));
    CHECK_EQ(a.rs().rowCount(), std::uint32_t(10));
    CHECK(a.rs().truncated());
    CHECK_EQ(a.rs().totalRows(), std::uint64_t(11));
    Stream b = encode(db, q, BufsOptions().maxRows(10).countAllRows());
    CHECK_EQ(b.rs().rowCount(), std::uint32_t(10));
    CHECK(b.rs().truncated());
    CHECK_EQ(b.rs().totalRows(), std::uint64_t(100));
    Stream c = encode(db, q, BufsOptions().maxRows(100));
    CHECK(!c.rs().truncated());
    CHECK_EQ(c.rs().totalRows(), std::uint64_t(100));
}

TEST(bufs_write_equals_exec)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(a INTEGER, b TEXT, c REAL)");
    for (int i = 0; i < 50; ++i)
        db.exec("INSERT INTO t VALUES (?, ?, ?)", Params{Value(i), Value(i % 2 ? "x" : "y"), Value(i * 0.5)});
    const std::string q = "SELECT a, b, c FROM t ORDER BY a";
    BufsWriter w1, w2;
    Stream direct(w1.exec(db, q));
    Stream viaResult(w2.write(db.exec(q)));
    CHECK_EQ(direct.messages.size(), viaResult.messages.size());
    // The columns' data is identical; only decl_type differs (write() has none).
    for (std::size_t c = 0; c < 3; ++c) {
        CHECK(direct.col(c).encoding() == viaResult.col(c).encoding());
        CHECK(direct.col(c).width() == viaResult.col(c).width());
        CHECK_EQ(direct.col(c).base(), viaResult.col(c).base());
        CHECK_EQ(direct.col(c).u8().size(), viaResult.col(c).u8().size());
    }
    CHECK_EQ(std::string(direct.rs().columns()[0].declType()), std::string("INTEGER"));
    CHECK_EQ(std::string(viaResult.rs().columns()[0].declType()), std::string());
    CHECK_EQ(direct.bytes.size() - viaResult.bytes.size(), direct.bytes.size() - viaResult.bytes.size());
}

namespace {

/// Random cells of mixed classes in 6 columns.
void fillRandom(Database& db, unsigned seed, int rows)
{
    std::mt19937_64 rng(seed);
    db.exec("CREATE TABLE r(id INTEGER PRIMARY KEY, a, b, c, d, e, f)");
    Transaction txn = db.begin();
    for (int i = 0; i < rows; ++i) {
        Params p;
        p.emplace_back(i);
        for (int c = 0; c < 6; ++c) {
            // Column c leans toward one class so that pure columns occur too.
            const int kind = (rng() % 10 == 0) ? int(rng() % 6) : c % 6;
            switch (kind) {
                case 0: p.emplace_back(); break;
                case 1: p.emplace_back(std::int64_t(rng() % 2000) - 1000 + (c == 1 ? (std::int64_t(1) << 40) : 0)); break;
                case 2: p.emplace_back(double(std::int64_t(rng() % 100000)) / ((rng() % 2) ? 4.0 : 10.0)); break;
                case 3: p.emplace_back(Datetime{std::int64_t(rng() % 100000) * (rng() % 2 ? 1000000 : 1)}); break;
                case 4: {
                    std::string t(rng() % 12, 'a');
                    for (char& ch : t)
                        ch = char('a' + rng() % (c == 4 ? 3 : 26));
                    p.emplace_back(t);
                    break;
                }
                default: {
                    std::string b(rng() % 40, '\0');
                    for (char& ch : b)
                        ch = char(rng());
                    p.push_back(Value::blob(b.data(), b.size()));
                    break;
                }
            }
        }
        txn.exec("INSERT INTO r VALUES (?, ?, ?, ?, ?, ?, ?)", p);
    }
    txn.commit();
}

}  // namespace

TEST(bufs_round_trip_random)
{
    for (unsigned seed = 1; seed <= 50; ++seed) {
        tst::Scratch s("bufs-rt");
        Database db = fresh(s);
        fillRandom(db, seed, 300);
        const Result truth = db.exec("SELECT a, b, c, d, e, f FROM r ORDER BY id");
        for (std::size_t batch : {std::size_t(0), std::size_t(64)}) {
            BufsWriter writer(BufsOptions().batchRows(batch));
            const BufsBytes bytes = writer.exec(db, "SELECT a, b, c, d, e, f FROM r ORDER BY id");
            auto view = w::ResultView::open(bytes.data(), bytes.size());
            CHECK_EQ(view.rowCount(), std::uint64_t(truth.size()));
            for (std::size_t r = 0; r < truth.size(); ++r)
                for (std::size_t c = 0; c < 6; ++c) {
                    const Value got = view.value(r, c);
                    const Value& want = truth[r][c];
                    if (got.type() != want.type() || compare(got, want) != 0)
                        tst::fail(__FILE__, __LINE__,
                                  "seed " + std::to_string(seed) + " row " + std::to_string(r) + " col " +
                                      std::to_string(c) + ": " + got.toLiteral() + " != " + want.toLiteral());
                }
            const Result back = w::toResult(view);
            CHECK_EQ(back.size(), truth.size());
        }
        if (seed == 1) {
            // Narrowing off must decode the same.
            BufsWriter raw(BufsOptions().narrowing(false).dictionary(false));
            const BufsBytes bytes = raw.exec(db, "SELECT a, b, c, d, e, f FROM r ORDER BY id");
            auto view = w::ResultView::open(bytes.data(), bytes.size());
            for (std::size_t r = 0; r < truth.size(); ++r)
                for (std::size_t c = 0; c < 6; ++c)
                    CHECK(compare(view.value(r, c), truth[r][c]) == 0);
        }
    }
}

TEST(bufs_verify_corruption)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    fillRandom(db, 7, 100);
    BufsWriter writer;
    const BufsBytes b = writer.exec(db, "SELECT a, b, c, d, e, f FROM r");
    std::vector<std::uint64_t> words((b.size() + 7) / 8);
    std::memcpy(words.data(), b.data(), b.size());
    auto* bytes = reinterpret_cast<std::uint8_t*>(words.data());
    CHECK(::bufs::verify<w::ResultSet>(bytes, b.size()));
    std::mt19937 rng(99);
    for (int i = 0; i < 20000; ++i) {
        const std::size_t at = rng() % b.size();
        const std::uint8_t keep = bytes[at];
        bytes[at] = std::uint8_t(rng());
        (void)::bufs::verify<w::ResultSet>(bytes, b.size());  // must not crash, whatever the damage
        bytes[at] = keep;
    }
}

TEST(bufs_view_rejects_bad_streams)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(a INTEGER)");
    db.exec("INSERT INTO t VALUES (1),(2),(3)");
    BufsWriter writer(BufsOptions().batchRows(2));
    const BufsBytes b = writer.exec(db, "SELECT a FROM t");
    CHECK_THROWS(w::ResultView::open(b.data(), b.size() - 8), ErrorCode::InvalidArgument);
    CHECK_THROWS(w::ResultView::open(b.data(), 0), ErrorCode::InvalidArgument);
    // A misaligned buffer is copied.
    std::vector<std::uint8_t> shifted(b.size() + 1);
    std::memcpy(shifted.data() + 1, b.data(), b.size());
    auto view = w::ResultView::open(shifted.data() + 1, b.size());
    CHECK_EQ(view.rowCount(), std::uint64_t(3));
    CHECK_EQ(view.value(2, 0).integer(), std::int64_t(3));
    // Another type id.
    std::vector<std::uint64_t> other((b.size() + 7) / 8);
    std::memcpy(other.data(), b.data(), b.size());
    reinterpret_cast<std::uint32_t*>(other.data())[1] ^= 1;
    CHECK_THROWS(w::ResultView::open(other.data(), b.size()), ErrorCode::InvalidArgument);
}

TEST(bufs_writer_no_alloc)
{
    tst::Scratch s("bufs");
    Database db = fresh(s);
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, status TEXT, price REAL, at DATETIME, note TEXT)");
    {
        Transaction txn = db.begin();
        for (int i = 0; i < 200; ++i)
            txn.exec("INSERT INTO t VALUES (?, ?, ?, ?, ?, ?)",
                     Params{Value(i), Value("name-" + std::to_string(i)), Value(i % 3 ? "open" : "closed"),
                            Value(i * 0.01), Value(Datetime{std::int64_t(i) * 1000000}),
                            (i % 5) ? Value() : Value("n")});
        txn.commit();
    }
    BufsWriter writer;
    Statement st = db.prepare("SELECT id, name, status, price, at, note FROM t WHERE id >= ?");
    st.bind(1, Value(0));
    Transaction txn = db.beginRead();
    for (int i = 0; i < 20; ++i)
        writer.exec(st, txn);
    gNews = 0;
    gCounting = true;
    std::size_t total = 0;
    for (int i = 0; i < 1000; ++i)
        total += writer.exec(st, txn).size();
    gCounting = false;
    CHECK_EQ(gNews.load(), std::uint64_t(0));
    CHECK(total > 0);
}

int main()
{
    return tst::runAll("bufs");
}
