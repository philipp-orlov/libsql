// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A tour of libsql in one file: schema, indexes, joins, parameters and a
// transaction.

#include <cstdio>
#include <filesystem>

#include "sql/sql.hpp"

int main()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "libsql-quickstart.db";
    std::filesystem::remove(path);

    sql::Database db(path);

    db.exec("CREATE TABLE author ("
            "  id   INTEGER PRIMARY KEY,"
            "  name TEXT NOT NULL UNIQUE)");
    db.exec("CREATE TABLE book ("
            "  id        INTEGER PRIMARY KEY,"
            "  author_id INTEGER NOT NULL,"
            "  title     TEXT NOT NULL,"
            "  price     REAL DEFAULT 0.0,"
            "  released  DATETIME,"
            "  cover     BLOB)");
    db.exec("CREATE INDEX book_by_author ON book (author_id)");

    // One transaction, several statements: all of it lands or none of it does.
    {
        sql::Transaction t = db.begin();
        t.exec("INSERT INTO author (name) VALUES ('Le Guin'), ('Lem')");
        t.exec("INSERT INTO book (author_id, title, price, released, cover) VALUES"
               " (1, 'The Dispossessed', 12.50, '1974-05-01', x'89504e47'),"
               " (1, 'A Wizard of Earthsea', 9.99, '1968-11-01', NULL),"
               " (2, 'Solaris', 11.00, '1961-01-01', NULL)");
        t.commit();
    }

    sql::Result r = db.exec("SELECT a.name, b.title, b.price, b.released"
                            " FROM book b"
                            " INNER JOIN author a ON b.author_id = a.id"
                            " WHERE b.price < ?"
                            " ORDER BY b.price DESC",
                            {sql::Value(12.0)});
    for (const sql::Row& row : r) {
        std::printf("%-10s %-22s %6.2f  %s\n", row["a.name"].toText().c_str(),
                    row["b.title"].toText().c_str(), row["b.price"].real(),
                    row["b.released"].toText().c_str());
    }

    // Blobs come back as bytes; examples/blobs.cpp has the whole story.
    const sql::Value cover = db.exec("SELECT cover FROM book WHERE id = 1")[0][0];
    std::printf("cover is %zu bytes: %s\n", cover.blob().size(), cover.toText().c_str());

    r = db.exec("UPDATE book SET price = price * 1.1 WHERE author_id = 1");
    std::printf("repriced %llu books\n", (unsigned long long)r.changes());

    r = db.exec("DELETE FROM book WHERE released < '1965-01-01'");
    std::printf("retired %llu books\n", (unsigned long long)r.changes());

    std::printf("%zu books left\n", db.exec("SELECT id FROM book").size());

    db.close();
    std::filesystem::remove(path);
    return 0;
}
