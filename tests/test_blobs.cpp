// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The store underneath: Database::env() and Transaction::txn() let libnosql
// layers -- BlobStorage here -- share the file with the tables.

#include "nosql/blob_storage.hpp"
#include "sql_test.hpp"

using namespace sql;

namespace {

std::string payload(std::size_t n, unsigned seed)
{
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
        s[i] = char((i * 31 + seed) & 0xff);
    return s;
}

TEST(blobStorageSharesTheStore)
{
    tst::Scratch s("blobs");
    Database db = s.open();
    db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, label INTEGER)");
    auto blobs = nosql::BlobStorage::open(db.env(), "tensors");
    CHECK_EQ(blobs.archivePath().parent_path(), s.file().parent_path());

    // Rows and blob index entries under one commit.
    {
        Transaction t = db.begin();
        auto w = blobs.beginWrite(t.txn());
        for (int i = 0; i < 10; ++i) {
            t.exec("INSERT INTO sample VALUES (?, ?)", {Value(i), Value(i % 3)});
            w.add(nosql::Slice::ref(i), std::to_string(i) + ".bin", payload(1000 + i, i));
        }
        w.commit();
        t.commit();
    }
    CHECK_EQ(db.exec("SELECT COUNT(*) FROM sample")[0][0].integer(), 10);
    CHECK_EQ(blobs.count(), std::uint64_t(10));

    // Rolled back together, too.
    {
        Transaction t = db.begin();
        auto w = blobs.beginWrite(t.txn());
        t.exec("INSERT INTO sample VALUES (10, 0)");
        const int k = 10;
        w.add(nosql::Slice::ref(k), "10.bin", payload(50, 10));
        w.commit();
        t.rollback();
    }
    CHECK_EQ(db.exec("SELECT COUNT(*) FROM sample")[0][0].integer(), 10);
    CHECK_EQ(blobs.count(), std::uint64_t(10));

    // Rows and the blobs they point at, read from one snapshot.
    {
        Transaction r = db.beginRead();
        std::vector<nosql::Slice> keys;
        std::vector<int> ids;
        for (const Row& row : r.exec("SELECT id FROM sample WHERE label = 1 ORDER BY id"))
            ids.push_back(int(row[0].integer()));
        for (const int& id : ids)
            keys.emplace_back(nosql::Slice::ref(id));
        const std::vector<nosql::Blob> got = blobs.findMany(r.txn(), keys);
        CHECK_EQ(got.size(), std::size_t(3));
        for (std::size_t i = 0; i < ids.size(); ++i)
            CHECK_EQ(got[i].data().string(), payload(1000 + std::size_t(ids[i]), unsigned(ids[i])));
        r.rollback();
        CHECK_THROWS(r.txn(), ErrorCode::BadTransaction);
    }

    // The blob sub-databases are invisible to the SQL catalog.
    CHECK_EQ(db.tables().size(), std::size_t(1));
}

TEST(envIsRefusedOnceClosed)
{
    tst::Scratch s("blobs");
    Database db = s.open();
    db.close();
    CHECK_THROWS(db.env(), ErrorCode::InvalidArgument);
}

}  // namespace

int main()
{
    return tst::runAll("blobs");
}
