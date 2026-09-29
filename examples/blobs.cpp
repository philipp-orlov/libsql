// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Putting binary data in and getting it back out.
//
// A BLOB column takes any bytes at all, zeros included. Bind them as a
// parameter rather than spelling them out as an x'..' literal and the statement
// stays the same size whatever the payload is.

#include <cstdio>
#include <filesystem>
#include <string>

#include "sql/sql.hpp"

namespace {

/// Stands in for a file read off disk.
sql::Blob payloadOf(std::size_t size, unsigned seed)
{
    sql::Blob out(size);
    for (std::size_t i = 0; i < size; ++i)
        out[i] = std::byte((i * 31 + seed) & 0xff);
    return out;
}

void describe(const std::string& name, const sql::Value& v)
{
    if (v.isNull()) {
        std::printf("  %-10s NULL\n", name.c_str());
        return;
    }
    std::string hex = v.toText();  // blobs render as lowercase hex
    if (hex.size() > 32)
        hex = hex.substr(0, 32) + "...";
    std::printf("  %-10s %6zu bytes  %s\n", name.c_str(), v.blob().size(), hex.c_str());
}

}  // namespace

int main()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "libsql-blobs.db";
    std::filesystem::remove(path);

    sql::Database db(path);
    db.exec("CREATE TABLE asset ("
            "  id    INTEGER PRIMARY KEY,"
            "  name  TEXT NOT NULL UNIQUE,"
            "  type  TEXT,"
            "  bytes BLOB,"
            "  added DATETIME)");

    // --- inserting ---

    const sql::Blob file = payloadOf(64 * 1024, 7);
    db.exec("INSERT INTO asset (name, type, bytes, added) VALUES (?, ?, ?, ?)",
            {sql::Value("logo.png"), sql::Value("image/png"), sql::Value(file),
             sql::Value("2024-05-01 09:30:00")});

    // Short constants read well as literals: x'' takes pairs of hex digits.
    db.exec("INSERT INTO asset (name, type, bytes) VALUES"
            " ('png-magic', 'application/octet-stream', x'89504e470d0a1a0a')");

    // Straight from a buffer, and a zero byte is data rather than a terminator.
    db.exec("INSERT INTO asset (name, bytes) VALUES (?, ?)",
            {sql::Value("with-nuls"), sql::Value::blob("a\0b\0c", 5)});

    // An empty blob and a missing one are different things.
    db.exec("INSERT INTO asset (name, bytes) VALUES ('empty', x''), ('absent', NULL)");

    // --- retrieving ---

    sql::Result r = db.exec("SELECT id, type, bytes, added FROM asset WHERE name = ?",
                            {sql::Value("logo.png")});
    // A Result owns its values, so the bytes outlive the statement that read
    // them and there is nothing to copy out of before the next query.
    const sql::Blob& stored = r[0]["bytes"].blob();
    std::printf("logo.png  id=%lld  %s  %zu bytes  added %s  round trip: %s\n\n",
                (long long)r[0]["id"].integer(), r[0]["type"].toText().c_str(), stored.size(),
                r[0]["added"].toText().c_str(), stored == file ? "identical" : "CORRUPT");

    for (const sql::Row& row : db.exec("SELECT name, bytes FROM asset ORDER BY name"))
        describe(row["name"].toText(), row["bytes"]);

    // --- blobs in predicates ---

    std::printf("\n%zu row carries the PNG signature\n",
                db.exec("SELECT name FROM asset WHERE bytes = x'89504e470d0a1a0a'").size());
    std::printf("%zu row has no bytes at all\n",
                db.exec("SELECT name FROM asset WHERE bytes IS NULL").size());

    // Blobs order byte by byte, shorter-is-less on a common prefix, and the
    // NULL row takes part in neither side of the comparison.
    std::printf("sorting below the signature:");
    for (const sql::Row& row : db.exec("SELECT name FROM asset"
                                       " WHERE bytes < x'89504e470d0a1a0a' ORDER BY bytes"))
        std::printf(" %s", row["name"].toText().c_str());
    std::putchar('\n');

    // --- replacing ---

    r = db.exec("UPDATE asset SET bytes = ?, added = ? WHERE name = ?",
                {sql::Value(payloadOf(32, 1)), sql::Value("2024-06-01"), sql::Value("logo.png")});
    std::printf("\nreplaced the bytes of %llu row, now %zu bytes\n",
                (unsigned long long)r.changes(),
                db.exec("SELECT bytes FROM asset WHERE name = 'logo.png'")[0][0].blob().size());

    db.close();
    std::filesystem::remove(path);
    return 0;
}
