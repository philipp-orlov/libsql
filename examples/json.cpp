// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// FOR JSON: turning a result set into a JSON document.
//
// libsql understands the T-SQL spelling of the clause --
//
//     SELECT ... FOR JSON AUTO
//     SELECT ... FOR JSON PATH, ROOT('orders'), INCLUDE_NULL_VALUES
//
// -- and exposes the same renderer to C++ through sql::JsonWriter, for results
// that were produced without the clause. The last section shows what the
// writer is really for: a service that answers the same query over and over
// and must not fragment its heap doing it.

#include <cstdio>
#include <filesystem>
#include <string>

#include "sql/json.hpp"

namespace {

void show(const char* label, const sql::Result& r)
{
    std::printf("%-28s %s\n", label, r[0][0].text().c_str());
}

}  // namespace

int main()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "libsql-json.db";
    std::filesystem::remove(path);

    sql::Database db(path);
    db.exec("CREATE TABLE customer ("
            "  id      INTEGER PRIMARY KEY,"
            "  name    TEXT NOT NULL,"
            "  contact TEXT,"
            "  badge   BLOB)");
    db.exec("CREATE TABLE line ("
            "  id          INTEGER PRIMARY KEY,"
            "  customer_id INTEGER NOT NULL,"
            "  item        TEXT NOT NULL,"
            "  qty         INTEGER,"
            "  shipped     DATETIME)");
    db.exec("CREATE INDEX line_by_customer ON line (customer_id)");

    {
        sql::Transaction t = db.begin();
        t.exec("INSERT INTO customer (id, name, contact, badge) VALUES"
               " (1, 'Ada Lovelace', 'ada@example.com', NULL),"
               " (2, 'Bo \"Bear\" Nilsson', NULL, x'89504e47')");
        t.exec("INSERT INTO line (id, customer_id, item, qty, shipped) VALUES"
               " (1, 1, 'anvil', 2, '2024-03-01T09:00:00'),"
               " (2, 1, 'rope',  5, NULL),"
               " (3, 2, 'kite',  1, '2024-03-04T17:30:00')");
        t.commit();
    }

    // ---------------------------------------------------------- AUTO ------
    // One object per row for a single table...
    show("AUTO, one table:",
         db.exec("SELECT id, name FROM customer ORDER BY id FOR JSON AUTO"));

    // ...and, across a join, the second table nested under its alias with the
    // rows of one customer gathered into a single element. Order the query the
    // way you want the document nested: AUTO groups rows as they arrive.
    show("AUTO, joined:",
         db.exec("SELECT c.name, l.item, l.qty"
                 " FROM customer c INNER JOIN line l ON l.customer_id = c.id"
                 " ORDER BY c.id, l.id"
                 " FOR JSON AUTO"));

    // ---------------------------------------------------------- PATH ------
    // PATH takes its shape from the column aliases instead: a dot is a level.
    show("PATH, dotted aliases:",
         db.exec("SELECT l.id,"
                 "       c.name    AS \"customer.name\","
                 "       c.contact AS \"customer.contact\","
                 "       l.item    AS \"line.item\","
                 "       l.qty     AS \"line.qty\""
                 " FROM customer c INNER JOIN line l ON l.customer_id = c.id"
                 " ORDER BY l.id"
                 " FOR JSON PATH"));

    // ------------------------------------------------------ modifiers -----
    // NULL columns are left out unless you ask for them; note how Bo's missing
    // contact makes the whole `customer` object disappear in the first one.
    show("NULLs dropped:",
         db.exec("SELECT id, contact AS \"customer.contact\" FROM customer"
                 " ORDER BY id FOR JSON PATH"));
    show("INCLUDE_NULL_VALUES:",
         db.exec("SELECT id, contact AS \"customer.contact\" FROM customer"
                 " ORDER BY id FOR JSON PATH, INCLUDE_NULL_VALUES"));

    // ROOT wraps the array in a named object, which is what most HTTP APIs
    // want to hand back.
    show("ROOT('customers'):",
         db.exec("SELECT id, name FROM customer ORDER BY id"
                 " FOR JSON PATH, ROOT('customers')"));

    // WITHOUT_ARRAY_WRAPPER drops the brackets, for a query that yields one row.
    show("WITHOUT_ARRAY_WRAPPER:",
         db.exec("SELECT id, name FROM customer WHERE id = 1"
                 " FOR JSON PATH, WITHOUT_ARRAY_WRAPPER"));

    // Datetimes become ISO-8601 in UTC, blobs become base64, and text is
    // escaped -- Bo's quoted nickname included.
    show("types and escaping:",
         db.exec("SELECT c.name, c.badge, l.shipped FROM customer c"
                 " INNER JOIN line l ON l.customer_id = c.id"
                 " WHERE c.id = 2 FOR JSON PATH"));

    // ------------------------------------------------- the C++ writer -----
    // The same renderer, driven from C++ over a result the clause never saw.
    const sql::Result rows = db.exec("SELECT id, name FROM customer ORDER BY id");
    std::printf("\ntoJson():                    %s\n", sql::toJson(rows).c_str());

    // `paths` re-nests a query without rewriting it.
    sql::JsonWriter writer(sql::JsonOptions().mode(sql::JsonMode::Path).root("customers"));
    const std::vector<std::string> paths = {"key", "who.name"};
    std::printf("re-nested:                   %s\n",
                std::string(writer.write(rows, paths)).c_str());

    // What the writer is for. It measures the document before writing it, so
    // the buffer grows at most once; reuse it and the steady state is no
    // allocation at all, which is what keeps a long-lived process from
    // fragmenting its heap one query at a time.
    sql::JsonWriter hot(sql::JsonOptions().mode(sql::JsonMode::Path));
    sql::Statement query = db.prepare("SELECT c.name AS name, l.item AS item, l.qty AS qty"
                                      " FROM customer c INNER JOIN line l"
                                      " ON l.customer_id = c.id"
                                      " ORDER BY c.id, l.id");
    std::size_t previous = 0;
    std::printf("\nreusing one writer:\n");
    for (int i = 0; i < 5; ++i) {
        const sql::Result r = query.exec();
        const std::string_view document = hot.write(r);
        std::printf("  pass %d: %zu bytes, buffer holds %zu%s\n", i, document.size(),
                    hot.capacity(), hot.capacity() == previous ? " (unchanged)" : " (grown)");
        previous = hot.capacity();
    }
    // Hand the memory back after an unusually large document.
    hot.compact();
    std::printf("  after compact(): buffer holds %zu\n", hot.capacity());

    // And for a caller that owns its own buffer -- a response body, say --
    // appendTo reserves exactly once and never shrinks it.
    std::string body;
    body.reserve(4096);
    for (int i = 0; i < 3; ++i) {
        body.clear();
        body += "{\"data\":";
        hot.appendTo(query.exec(), body);
        body += "}";
    }
    std::printf("  body: %s\n", body.c_str());

    db.close();
    std::filesystem::remove(path);
    return 0;
}
