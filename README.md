# libsql

A small SQL engine on top of [libnosql](../libnosql). Tables, indexes and rows
live in one libnosql store, so libsql inherits that store's ACID commits,
single-writer/many-reader MVCC and single-file layout, and adds a schema, a
query language and a planner that uses the indexes you declare.

```cpp
#include "sql/sql.hpp"

sql::Database db("catalog.db");

db.exec("CREATE TABLE author (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)");
db.exec("CREATE TABLE book ("
        "  id INTEGER PRIMARY KEY,"
        "  author_id INTEGER NOT NULL,"
        "  title TEXT NOT NULL,"
        "  price REAL DEFAULT 0.0,"
        "  released DATETIME,"
        "  cover BLOB)");
db.exec("CREATE INDEX book_by_author ON book (author_id)");

db.exec("INSERT INTO author (name) VALUES ('Le Guin')");
db.exec("INSERT INTO book (author_id, title, price, released) VALUES (?, ?, ?, ?)",
        {sql::Value(1), sql::Value("The Dispossessed"), sql::Value(12.50),
         sql::Value("1974-05-01")});

for (const sql::Row& r : db.exec(
         "SELECT a.name, b.title FROM book b"
         " INNER JOIN author a ON b.author_id = a.id"
         " WHERE b.price < 20 ORDER BY b.released")) {
    std::printf("%s -- %s\n", r["a.name"].toText().c_str(), r["b.title"].toText().c_str());
}
```

## What it does

**Statements**

| | |
| --- | --- |
| `CREATE TABLE [IF NOT EXISTS] t (col TYPE [constraints], ...)` | `PRIMARY KEY`, `NOT NULL`, `UNIQUE`, `DEFAULT literal`, and table-level `PRIMARY KEY (a, b, ...)` and `UNIQUE (a, b, ...)` |
| `DROP TABLE [IF EXISTS] t` | takes the table's indexes with it |
| `CREATE [UNIQUE] INDEX [IF NOT EXISTS] i ON t (a, b, ...)` | backfilled over the rows already there |
| `DROP INDEX [IF EXISTS] i` | |
| `INSERT INTO t [(cols)] VALUES (...), (...)` | multi-row, defaults filled in |
| `SELECT ... FROM t [alias] [INNER JOIN u ON ...] [WHERE ...] [GROUP BY ...] [HAVING ...] [ORDER BY ... [DESC]] [LIMIT n [OFFSET m]] [FOR JSON ...]` | `*`, `t.*`, expressions, `AS` aliases |
| `UPDATE t SET col = expr, ... [WHERE ...]` | may move the primary key |
| `DELETE FROM t [WHERE ...]` | |

Expressions cover `AND OR NOT`, `= <> != < <= > >=`, `+ - * / %`, `||`,
`IS [NOT] NULL`, `[NOT] IN (...)`, `[NOT] BETWEEN a AND b`, `[NOT] LIKE` with
`%` and `_`, parentheses, and `?` parameters bound left to right. NULL follows
SQL's three-valued logic. Identifiers may be quoted with `"..."`, `` `...` ``
or `[...]`, and are matched case-insensitively otherwise. `--` and `/* */`
comments are ignored.

**Aggregates**

`COUNT(*)`, `COUNT(x)`, `SUM(x)`, `AVG(x)`, `MIN(x)` and `MAX(x)`, alone or
inside a larger expression. All but `COUNT(*)` skip NULL inputs, and return
NULL when a group has nothing left to work with. `SUM` of integers stays an
integer; `AVG` is always real.

```sql
SELECT region, COUNT(*) AS n, SUM(amount) AS total
  FROM sale
 GROUP BY region
HAVING SUM(amount) > 100
 ORDER BY total DESC;
```

Without `GROUP BY` the whole result is one group, and aggregating no rows still
reports one row — `COUNT(*)` is 0 rather than absent. With `GROUP BY` an empty
input has no keys and so produces no rows. Groups come out in key order unless
`ORDER BY` says otherwise, and a NULL key forms a group of its own.

A column selected outside `GROUP BY` reports the first row of its group rather
than being an error, which is the useful reading of
`SELECT dept, name, COUNT(*) ... GROUP BY dept`.

`ORDER BY` may name a result column by its alias or by its 1-based position,
which is the only way to sort on something the row itself does not hold.

**JSON results**

A `SELECT` may end with `FOR JSON`, spelled as T-SQL spells it. The result is
one row of one `TEXT` column, named `json`, holding the whole document.

```sql
SELECT ... FOR JSON {AUTO | PATH}
                    [, ROOT[('name')]]
                    [, INCLUDE_NULL_VALUES]
                    [, WITHOUT_ARRAY_WRAPPER]
```

`AUTO` takes its shape from the `FROM` clause: the first table is the object,
and each joined table is nested one level further in under its alias, with
consecutive rows that agree on the enclosing columns collapsed into a single
element. Order the query the way you want the document nested — `AUTO` groups
rows as they arrive, so `ORDER BY` the outer key first.

```sql
SELECT c.name, l.item, l.qty
  FROM customer c INNER JOIN line l ON l.customer_id = c.id
 ORDER BY c.id, l.id
   FOR JSON AUTO;
-- [{"name":"Ada","l":[{"item":"anvil","qty":2},{"item":"rope","qty":5}]},
--  {"name":"Bo", "l":[{"item":"kite","qty":1}]}]
```

`PATH` takes its shape from the column aliases instead: a dot is a level, every
row is one object, and nothing is collapsed.

```sql
SELECT l.id, c.name AS "customer.name", l.item AS "line.item"
  FROM customer c INNER JOIN line l ON l.customer_id = c.id
   FOR JSON PATH;
-- [{"id":1,"customer":{"name":"Ada"},"line":{"item":"anvil"}}, ...]
```

A NULL column is left out, and a nested object with nothing left in it goes
with it, unless `INCLUDE_NULL_VALUES` says otherwise. `ROOT` wraps the array in
a one-property object, defaulting to the name `root`. `WITHOUT_ARRAY_WRAPPER`
drops the enclosing brackets, and cannot be combined with `ROOT`. An empty
result renders as `[]` rather than as no rows at all.

Integers and reals go out as JSON numbers, shortest round-trip; `DATETIME`
becomes ISO-8601 in UTC (`"2024-03-01T09:00:00Z"`, with `.ffffff` when the
fraction is nonzero); `BLOB` becomes base64; `TEXT` is escaped and is expected
to be UTF-8 — bind anything else as a blob. `FOR` is a reserved word from now
on, so a column of that name needs quoting.

The same renderer is available to C++ over any `Result`, through
[include/sql/json.hpp](include/sql/json.hpp):

```cpp
sql::Result rows = db.exec("SELECT id, name FROM customer");
std::string doc = sql::toJson(rows, sql::JsonOptions().mode(sql::JsonMode::Path).root("customers"));
```

For anything in a loop, use a `sql::JsonWriter` instead and keep it: it holds on
to its buffer and to the plan it derived from the column names, and it measures
the document before writing it, so the buffer grows at most once and a warmed-up
writer allocates nothing per query. `appendTo` does the same onto a buffer you
own — a response body, say — and `write(result, paths)` renders a query under
different property paths without rewriting it.
[examples/json.cpp](examples/json.cpp) runs through all of it.

**Types**

`NULL`, `INTEGER` (signed 64-bit), `REAL` (double), `TEXT`, `DATETIME`
(microseconds since the Unix epoch, UTC) and `BLOB`. The usual spellings map
onto them: `INT`/`BIGINT`, `FLOAT`/`DOUBLE`, `VARCHAR(n)`/`CHAR`/`STRING`,
`TIMESTAMP`/`DATE`, `BINARY`.

A value is converted to the column's declared type on the way in, and a literal
compared against a column is read the way that column is stored, so
`WHERE released > '2024-01-01'` compares two datetimes. A conversion that
cannot work raises `TypeMismatch` rather than storing something else. A column
declared `NULL` accepts any type and stores it as-is.

Datetime literals are `YYYY-MM-DD`, `YYYY-MM-DD HH:MM[:SS[.ffffff]]`, with `T`
allowed for the separator and a trailing `Z` accepted. Blob literals are
`x'00ff'`; binary is usually better bound as a parameter, which
[examples/blobs.cpp](examples/blobs.cpp) works through end to end.

**Keys and indexes**

Every table has a key. Declare one with `PRIMARY KEY` on a column or with a
table-level `PRIMARY KEY (a, b, ...)` over several, or let the engine keep an
implicit integer key that SQL can still address as `rowid`. A single integer
primary key left NULL on insert is assigned the next free value, and an
explicitly supplied one pushes that counter forward; a composite key has to be
supplied whole, and no part of it may be NULL.

```sql
CREATE TABLE price (
  sku    TEXT,
  region TEXT,
  cents  INTEGER NOT NULL,
  PRIMARY KEY (sku, region),
  UNIQUE (region, cents)
);
```

Rows are stored under their key columns encoded in key order, so the key is also
the table's clustering order and its first index: a leading run of key columns
pinned by equality (here `sku`, or `sku` and `region`) plus a range on the next
one is answered from a window over the rows themselves. A table-level
`UNIQUE (a, b, ...)` becomes a unique index over those columns, as a column-level
`UNIQUE` does for one.

The planner looks at the conjuncts constraining each table and picks the
narrowest of a primary-key lookup, an index lookup (equality on a leading run of
index columns, plus a range on the column after it) and a full scan. Joins are
nested loops, and the inner table is planned with the outer tables' columns
already bound, so `ON a.id = b.a_id` becomes a lookup rather than a second scan.
Whatever the plan, every predicate is re-checked against the row, so a plan is
only ever allowed to be too wide, never too narrow.

## Transactions

`Database::exec` runs every `;`-separated statement in its argument inside one
transaction: committed on return, rolled back if anything throws. For a
transaction spanning several calls, use `Database::begin`, which returns a
`Transaction` that rolls back if it is destroyed without a `commit`.

`begin` takes the writer slot and so blocks until any other write transaction
finishes. For multi-statement reads use `Database::beginRead`, which takes a
snapshot instead: it neither blocks nor is blocked, and it rejects any statement
that would write.

## Prepared statements

`Database::prepare` parses once so that running costs no parsing, which is worth
it for anything in a loop. Parameters are bound 1-based as in SQLite, and ones
never bound are NULL.

```cpp
sql::Statement insert = db.prepare("INSERT INTO book (title, price) VALUES (?, ?)");
sql::Transaction t = db.begin();
for (const Book& b : books)
    insert.bind(1, sql::Value(b.title)).bind(2, sql::Value(b.price)).exec(t);
t.commit();
```

`exec()` runs in a transaction of its own; `exec(txn)` runs in one you control.
Both take an optional `Params` to bind and run in a single call.

A prepared statement also keeps its plan -- the columns it bound, the access
path it chose for each table, the layout and names of its result -- and reuses
it for as long as the schema it was planned against is the one the transaction
sees. A `CREATE INDEX` or `DROP TABLE` anywhere makes the next run plan again,
so a prepared statement keeps working after the schema changes and picks up an
index created after it was prepared. A statement belongs to the database it
came from and holds it open. It is move-only and not safe to run from two
threads at once.

Results own their values, so a `Result` outlives the transaction that produced
it.

## The store underneath

`Database::env()` returns the `nosql::Env` the tables live in, and
`Transaction::txn()` the `nosql::Txn` a transaction runs on. They exist so other
libnosql layers can share the file: `nosql::BlobStorage::open(db.env(), "images")`
keeps bulk payloads in a side-car tar archive with its index in this database,
and `blobs.beginWrite(t.txn())` puts the appends under the same commit as the
rows that describe them. Both are borrowed -- valid while the database is open
and the transaction live. The sub-database names `sql_catalog`, `tbl:*` and
`idx:*` belong to libsql.

`Database::configure().cacheReadChecksums()` passes libnosql's read cache
through: a page whose checksum this process has verified since it was last
written is not re-hashed on the next read, which roughly halves the cost of a
point lookup and of every probe inside a join. The trade is that damage done to
the file from outside is noticed on the next re-verification -- every minute by
default, `cacheReadChecksums(true, interval)` to change it -- rather than the
next read; writes always verify. It is off by default.

## Python

[python/](python/) is a pybind11 binding of the engine and of `BlobStorage`,
built with `-DSQL_BUILD_PYTHON=ON` or `pip install ./python`. Blobs come out
as zero-copy buffers (`numpy.frombuffer`, `torch.frombuffer`), batches of keys
resolve in one pass, and the next batch's pages can be prefetched -- the shape
a training loader wants. See [python/README.md](python/README.md) and
[python/examples/](python/examples/).

## The `sql` tool

```
sql <database> [options]

  -c SQL        run SQL, then exit; may be repeated
  -f FILE       run the statements in FILE; may be repeated
  -i            keep reading stdin after any -c or -f
  --mode M      output as table (default), csv or list
  --no-header   leave the column names out
  --readonly    open an existing database and refuse to write to it
```

With no `-c` and no `-f` it reads statements from stdin and offers
`.tables`, `.schema [TABLE]`, `.indexes [TABLE]`, `.mode`, `.headers`, `.help`
and `.quit`.

```
$ sql catalog.db --mode table -c "SELECT title, price FROM book ORDER BY price"
+----------------------+-------+
| title                | price |
+----------------------+-------+
| A Wizard of Earthsea | 9.99  |
| The Dispossessed     | 12.5  |
+----------------------+-------+
2 rows
```

Exit status: 0 ok, 1 bad usage, 2 statement or store error.

## Performance

`benchmarks/bench_sql` runs a fixed set of statement shapes over a 200,000-row
table with two indexes and a 400,000-row joined table, and reports the median
ns/op of each together with the heap allocations it made. Run it pinned to a
core in Release; `--json` and `--csv` write the figures out, `--only` picks
workloads, `--cache` turns the read cache on and `--durable` fsyncs every commit.

The engine is built so that a statement that has run once allocates almost
nothing on the next run: a prepared statement keeps its plan (bound columns,
access paths, output layout, column names) until the schema changes, decodes
rows into cells that keep their buffers, builds keys and payloads in strings it
reuses, evaluates expressions through references rather than copies, and drives
a recycled cursor for every scan. The schema itself is decoded once per change
and shared by every transaction that sees that version. On a 2.1 GHz Xeon Gold
6252 (GCC 13.3, Release, one pinned core, `durable(false)`), before and after:

| statement | before | after | after, `cacheReadChecksums()` |
|---|---:|---:|---:|
| `SELECT ... WHERE id = ?`, prepared, own transaction | 7.6 µs, 47 allocations | 3.5 µs, 7 | 2.0 µs, 7 |
| the same inside one read transaction | 6.8 µs, 47 | 2.1 µs, 6 | 1.8 µs |
| `SELECT ... WHERE id = ?` as one-off `exec` | 12.1 µs, 64 | 8.5 µs, 41 | 8.5 µs |
| `INSERT` of a 6-column row, 2 indexes, prepared, batches of 1,000 | 10.3 µs, 38 | 6.3 µs, 2 | 5.8 µs |
| the same `INSERT`, one transaction per row | 25.6 µs, 44 | 13.6 µs, 6 | 13.6 µs |
| index equality returning ~200 rows | 380 µs, 1,049 | 330 µs, 414 | 198 µs |
| `SELECT COUNT(*)` over 200,000 rows, per row | 430 ns, 3 | 152 ns, 0 | 139 ns |
| `ORDER BY name LIMIT 100` over a full scan, per row | 1,727 ns, 5 | 893 ns, 1 | 805 ns |
| join through an index, ~400 output rows | 1.46 ms, 22,590 | 1.22 ms, 8,160 | 0.74 ms |
| join with `GROUP BY` over 1,000 items | 6.0 ms, 140,810 | 5.2 ms, 70,280 | 3.0 ms |
| `UPDATE ... WHERE id = ?`, prepared, batches of 1,000 | 18.6 µs, 61 | 11.5 µs, 4 | 11.2 µs |
| `UPDATE ... WHERE id = ?`, own transaction | 147 µs, 57 | 25.7 µs, 8 | 36 µs |

Allocation counts are heap allocations per statement (per row for the scans),
measured by the benchmark's replaced `operator new`.

The `UPDATE` line is the store's doing rather than the SQL layer's: a small
commit on a database that had just been bulk loaded used to pay for the size of
the free list and of the dirty-page table the load left behind, and no longer
does (see libnosql's README, "Numbers").

## Long runs

A database is expected to stay up for months, so "does not grow" is treated as a
correctness property rather than a nicety. `tests/test_soak.cpp` replaces
`operator new` to count live bytes and blocks exactly, reads the allocator's own
footprint through `mallinfo2`, and watches the store file on disk. It brackets a
warm-up phase and a much longer measured phase and compares them.

At a fixed data size, every workload measured comes out at **zero** growth in
live bytes, live blocks and allocator footprint, and the store file does not move
a byte. That holds at the default length and unchanged at twenty times it:

| workload | measured phase at `LIBSQL_SOAK_SCALE=20` |
| --- | --- |
| one-off `exec`, parsed and planned every call | 480,000 statements |
| prepared statements re-run | 480,000 executions |
| statements prepared and discarded per request | 160,000 prepares |
| random insert/update/delete over a bounded key space | 20,000 transactions, 400,000 operations |
| 512 KiB blob rewritten and read back | 8,000 rounds |
| join with `GROUP BY` over 1,000 rows | 24,000 queries |
| `FOR JSON` and a reused `JsonWriter` over a join | 80,000 renders |
| `CREATE TABLE` / load / `DROP TABLE` cycles | 4,000 |

Set `LIBSQL_SOAK_SCALE=20` (or higher) to lengthen the measured phase; the whole
run above takes forty seconds. Under a sanitizer the same test runs a short
version and reports without asserting, because the sanitizer supplies the
allocator the figures would be about — there it is looking for memory errors and
leaks, and the measuring is left to the ordinary build.

Two things do grow, both on purpose. A database that gets bigger needs a bigger
working set: a deeper tree dirties more pages per commit, and libnosql keeps some
of those buffers to reuse. That cost converges rather than keeping pace with the
traffic — the soak does the same work twice over a growing table and requires the
second half to cost no more than the first. And a transaction holds its dirty
pages until it commits, so a single enormous transaction needs memory in
proportion; libnosql caps that at its `dirtyLimit` and raises `OutOfMemory`
rather than letting the process run away. Both bounds are the store's, and
neither depends on how long the process has been up.

Fragmentation is measured, not assumed: allocator footprint is sampled
separately from live bytes, so a heap that grew while holding the same amount of
data would fail even though nothing leaked. The store's file mapping does not go
through `malloc`, so neither figure is disturbed by how much of the file happens
to be paged in.

`FOR JSON` is built to the same rule. The renderer counts the document before
it writes it, so the output buffer is sized once and exactly instead of doubling
its way there, and it formats numbers, datetimes and blobs into stack buffers
rather than into temporary strings. The clause keeps one writer per thread for
the life of the process, which is why running the same query a million times
leaves the plan and the buffers where they were.

The soak also re-checks, at every sample point, that each index returns exactly
what a full scan finds. A stale index entry is the failure mode that a long run
produces and a short one hides.

## Errors

Everything the SQL layer rejects throws `sql::Error`, carrying a
`sql::ErrorCode`: `SyntaxError`, `Unsupported`, `InvalidArgument`,
`NoSuchTable`, `TableExists`, `NoSuchColumn`, `AmbiguousColumn`, `NoSuchIndex`,
`IndexExists`, `TypeMismatch`, `ConstraintViolation`, `BadTransaction`,
`ReadOnly`, `Internal`. Storage-level failures keep arriving as `nosql::Error`.

## Not implemented

Outer and comma joins, sub-queries, `UNION`, `DISTINCT`, `ALTER TABLE`, views,
triggers and scalar functions. Each of these is reported as `Unsupported`
rather than misread.

## Building

libsql needs libnosql. An installed one is used if CMake can find it; otherwise
the sibling checkout at `../libnosql` is built in place. Point
`-DSQL_NOSQL_DIR=...` somewhere else if it lives elsewhere.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Options: `SQL_BUILD_TESTS`, `SQL_BUILD_EXAMPLES`, `SQL_BUILD_TOOLS`,
`SQL_BUILD_BENCHMARKS`, `SQL_BUILD_PYTHON`, `SQL_ENABLE_ASAN`. Benchmarks and
the Python binding are off by default; the binding is experimental and has not
been through the robustness review the C++ library has. When libsql builds its
sibling libnosql it compiles only the pieces it needs (`NOSQL_WITH_BLOB` for
the tests and the binding; `NOSQL_WITH_MQ` and `NOSQL_WITH_REPLICATION` off).
C++17, 64-bit,
Linux and Windows, no third-party dependencies beyond libnosql itself. GCC 9.4
with CMake 3.16 -- NVIDIA JetPack 5.1.2's Ubuntu 20.04 -- is the oldest
toolchain the code is written for; nothing beyond C++17 is used.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
