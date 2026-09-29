#!/usr/bin/env python3
"""The SQL side of libsql from Python: tables, parameters, joins, transactions
and prepared statements.

    PYTHONPATH=build/python python3 python/examples/quickstart.py
"""

import datetime as dt
import os
import tempfile

import libsql

path = os.path.join(tempfile.mkdtemp(prefix="libsql-quickstart-"), "catalog.db")

# A Database is one file. It is a context manager; close() releases the file.
with libsql.Database(path) as db:
    db.exec(
        """
        CREATE TABLE author (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
        CREATE TABLE book (
            id        INTEGER PRIMARY KEY,
            author_id INTEGER NOT NULL,
            title     TEXT NOT NULL,
            price     REAL DEFAULT 0.0,
            released  DATETIME,
            cover     BLOB);
        CREATE INDEX book_by_author ON book (author_id);
        """
    )

    # Parameters bind left to right. Python None, bool, int, float, str, bytes,
    # datetime.datetime (naive is taken as UTC) and datetime.date all map onto
    # the six SQL storage classes; so do numpy scalars and arrays.
    r = db.exec("INSERT INTO author (name) VALUES (?), (?)", ("Le Guin", "Banks"))
    print("inserted", r.changes, "authors; last id", r.last_insert_id)

    books = [
        (1, "The Dispossessed", 12.50, dt.datetime(1974, 5, 1, tzinfo=dt.timezone.utc)),
        (1, "A Wizard of Earthsea", 9.99, dt.date(1968, 11, 1)),
        (2, "Use of Weapons", 14.00, "1990-09-01"),  # a literal string parses as a datetime too
    ]
    # A prepared statement parses once; exec(params, txn=...) runs it inside a
    # transaction you hold, so the whole load is one commit.
    insert = db.prepare("INSERT INTO book (author_id, title, price, released) VALUES (?, ?, ?, ?)")
    with db.begin() as t:  # commits on a clean exit, rolls back on an exception
        for b in books:
            insert.exec(b, txn=t)

    # Results are lists of rows; rows index by position or by (case-insensitive) name.
    rows = db.exec(
        """
        SELECT a.name, b.title, b.price, b.released
          FROM book b INNER JOIN author a ON b.author_id = a.id
         WHERE b.price < ?
         ORDER BY b.released
        """,
        (13.0,),
    )
    print(rows.columns)
    for row in rows:
        print(f"  {row['a.name']:8} {row['b.title']:22} {row[2]:6.2f}  {row['b.released']:%Y-%m-%d}")

    # Aggregates, and scalar() for a one-value answer.
    print("books per author:", db.exec("SELECT author_id, COUNT(*) AS n FROM book GROUP BY author_id").rows)
    print("mean price:", round(db.exec("SELECT AVG(price) FROM book").scalar(), 2))

    # Every statement runs in a transaction; an error rolls the whole exec back.
    try:
        db.exec("INSERT INTO author (name) VALUES ('Banks')")
    except libsql.SqlError as e:
        print("refused:", e.code, "-", e)

    # A read transaction is a snapshot: consistent across several statements,
    # never blocked by a writer.
    with db.begin_read() as snap:
        n = snap.exec("SELECT COUNT(*) FROM book").scalar()
        total = snap.exec("SELECT SUM(price) FROM book").scalar()
        print(f"{n} books worth {total:.2f} at one instant")

    # Schema introspection.
    print("tables:", db.tables())
    print("book columns:", [(c["name"], c["type"]) for c in db.table("book")["columns"]])
    print("indexes on book:", [ix["name"] for ix in db.indexes("book")])

print("database at", path)
