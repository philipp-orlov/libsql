# libsql

A SQL engine on top of [libnosql](../libnosql) that speaks the SQL language of
SQLite 3.45, plus a `FOR JSON` clause and a `DATETIME` storage class of its own.
Tables, indexes and rows live in one libnosql store, so libsql inherits that
store's ACID commits, single-writer/many-reader MVCC and single-file layout,
and adds a schema, the query language and a planner that uses the indexes you
declare.

```cpp
#include "sql/sql.hpp"

sql::Database db("catalog.db");

db.exec("CREATE TABLE author (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)");
db.exec("CREATE TABLE book ("
        "  id INTEGER PRIMARY KEY,"
        "  author_id INTEGER NOT NULL REFERENCES author(id),"
        "  title TEXT NOT NULL,"
        "  price REAL DEFAULT 0.0,"
        "  released DATETIME,"
        "  cover BLOB) STRICT");
db.exec("CREATE INDEX book_by_author ON book (author_id)");

db.exec("INSERT INTO author (name) VALUES ('Le Guin')");
db.exec("INSERT INTO book (author_id, title, price, released) VALUES (:a, :t, :p, :r)",
        sql::NamedParams{{"a", sql::Value(1)}, {"t", sql::Value("The Dispossessed")},
                         {"p", sql::Value(12.50)}, {"r", sql::Value("1974-05-01")}});

for (const sql::Row& r : db.exec(
         "SELECT a.name, b.title FROM book b"
         " JOIN author a ON b.author_id = a.id"
         " WHERE b.price < 20 ORDER BY b.released")) {
    std::printf("%s -- %s\n", r["a.name"].toText().c_str(), r["b.title"].toText().c_str());
}
```

## What it does

**The language**

Every statement in SQLite's language reference except `CREATE VIRTUAL TABLE`:

| | |
| --- | --- |
| `CREATE TABLE` | column and table constraints (`PRIMARY KEY [ASC\|DESC] [AUTOINCREMENT]`, `NOT NULL`, `UNIQUE`, `CHECK`, `DEFAULT` expressions, `COLLATE`, `GENERATED ALWAYS AS (...) VIRTUAL\|STORED`, `REFERENCES ... ON DELETE/UPDATE ...`), `ON CONFLICT` clauses, `STRICT`, `WITHOUT ROWID`, `CREATE TABLE ... AS SELECT`, `TEMP` |
| `ALTER TABLE` | `RENAME TO`, `RENAME COLUMN`, `ADD COLUMN`, `DROP COLUMN`, rewriting the views, triggers and indexes that name what changed |
| `CREATE [UNIQUE] INDEX` | on expressions, `COLLATE`/`ASC`/`DESC` per part, partial (`WHERE`) |
| `CREATE VIEW`, `CREATE TRIGGER` | `BEFORE`/`AFTER`/`INSTEAD OF`, `FOR EACH ROW`, `WHEN`, `UPDATE OF`, `RAISE(...)`, recursive triggers off by default |
| `INSERT` / `REPLACE` | `OR ABORT\|FAIL\|IGNORE\|REPLACE\|ROLLBACK`, `DEFAULT VALUES`, `INSERT ... SELECT`, upsert (`ON CONFLICT ... DO UPDATE\|NOTHING`), `RETURNING` |
| `UPDATE` / `DELETE` | `UPDATE ... FROM`, row-value assignments, `ORDER BY ... LIMIT`, `RETURNING` |
| `SELECT` | `WITH [RECURSIVE]`, `DISTINCT`, every join (`LEFT`, `RIGHT`, `FULL`, `CROSS`, `NATURAL`, `USING`), sub-queries anywhere, `UNION [ALL]`/`INTERSECT`/`EXCEPT`, `VALUES`, window functions with named windows, frames and `FILTER`, `NULLS FIRST/LAST`, table-valued `json_each`/`json_tree`/`pragma_*` |
| transactions | `BEGIN [DEFERRED\|IMMEDIATE\|EXCLUSIVE]`, `COMMIT`/`END`, `ROLLBACK`, `SAVEPOINT`, `RELEASE`, `ROLLBACK TO` |
| the rest | `PRAGMA` (the ones meaningful for a libnosql store), `ATTACH`/`DETACH` (files and `:memory:`), `VACUUM [INTO]`, `ANALYZE` (fills `sqlite_stat1`), `REINDEX`, `EXPLAIN [QUERY PLAN]` |

Expressions are SQLite's: operators including `->`/`->>`, `IS [NOT] DISTINCT
FROM`, `GLOB`, `LIKE ... ESCAPE`, `CAST`, `CASE`, `COLLATE`, `EXISTS`, scalar
and `IN` sub-queries, row values, and the parameter spellings `?`, `?NNN`,
`:name`, `@name` and `$name`. The built-in functions are those of a default
SQLite build: the core scalars, `printf`/`format`, the date and time
functions, the math functions, the aggregates (with `ORDER BY` inside them),
the window functions and the JSON functions. `SELECT name FROM
pragma_function_list` lists them. The schema is visible in `sqlite_schema`
(alias `sqlite_master`), `sqlite_temp_schema` and `sqlite_sequence`.

Results match SQLite's to the value: the same type rules, the same text for
reals (`%!.15g`), the same result column names, the same error messages where
an application might match on them. The engine is checked against SQLite
3.45.1 by the whole sqllogictest corpus (5.9 million records, all passing)
and by a differential fuzzer; see [tests/conformance/](tests/conformance/).

**JSON results**

A `SELECT` may end with `FOR JSON`, spelled as T-SQL spells it. The result is
one row of one `TEXT` column, named `json`, holding the whole document.

```sql
SELECT ... FOR JSON {AUTO | PATH}
                    [, ROOT[('name')]]
                    [, INCLUDE_NULL_VALUES]
                    [, WITHOUT_ARRAY_WRAPPER]
```

`AUTO` takes its shape from the `FROM` clause as written: the first table is
the object, and each joined table is nested one level further in under its
alias, with consecutive rows that agree on the enclosing columns collapsed into
a single element. Order the query the way you want the document nested --
`AUTO` groups rows as they arrive, so `ORDER BY` the outer key first.

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

`FOR JSON` works wherever a `SELECT` does: in a sub-query, a view, a CTE. A
nested `FOR JSON` result -- and any value a JSON function produced -- is
embedded as JSON rather than quoted as a string, so
`SELECT c.name, (SELECT item FROM line l WHERE l.customer_id = c.id FOR JSON PATH) AS lines FROM customer c FOR JSON PATH`
builds a nested document in one statement.

Integers and reals go out as JSON numbers, shortest round-trip; `DATETIME`
becomes ISO-8601 in UTC (`"2024-03-01T09:00:00Z"`, with `.ffffff` when the
fraction is nonzero); `BLOB` becomes base64; `TEXT` is escaped and is expected
to be UTF-8 -- bind anything else as a blob. `FOR` is recognized as the clause
only after a complete `SELECT`, so it stays usable as a column name.

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
own -- a response body, say -- and `write(result, paths)` renders a query under
different property paths without rewriting it.
[examples/json.cpp](examples/json.cpp) runs through all of it.

**Types**

Ordinary tables use SQLite's flexible typing: a column's declared type gives it
an affinity (`INTEGER`, `REAL`, `NUMERIC`, `TEXT` or `BLOB`, by SQLite's
substring rules), values are converted toward it when that loses nothing, and
any value can be stored in any column. `STRICT` tables check types instead,
over `INT`, `INTEGER`, `REAL`, `TEXT`, `BLOB` and `ANY`, raising
`ConstraintViolation` ("cannot store TEXT value in INTEGER column t.c") on a
value that does not convert losslessly.

libsql adds a sixth storage class, `DATETIME` (microseconds since the Unix
epoch, UTC; `typeof` says `'datetime'`; it sorts between numbers and text). A
`STRICT` table may declare a `DATETIME` column: text written to it is parsed as
a date, and text compared with it is read as one, so
`WHERE released > '2024-01-01'` compares two datetimes. Bound
`sql::Datetime` values keep the class in any column. In a flexible table
`DATETIME` is just a type name with `NUMERIC` affinity, as in SQLite. The date
and time functions accept the class as a time value. Datetime text is
`YYYY-MM-DD`, `YYYY-MM-DD HH:MM[:SS[.ffffff]]`, with `T` allowed for the
separator and a trailing `Z` accepted.

Blob literals are `x'00ff'`; binary is usually better bound as a parameter,
which [examples/blobs.cpp](examples/blobs.cpp) works through end to end.

**Keys and indexes**

A table without a declared key, or with an `INTEGER PRIMARY KEY`, is stored by
its rowid, and a new row gets one past the largest rowid in use (or, with
`AUTOINCREMENT`, one past the largest ever used). A table with any other
`PRIMARY KEY` -- one column or several -- is stored clustered under that key,
so the key is also the table's order and its first index; it still has a
hidden rowid, which is never reused. `WITHOUT ROWID` tables have no rowid at
all. Primary key columns are always `NOT NULL`.

```sql
CREATE TABLE price (
  sku    TEXT,
  region TEXT,
  cents  INTEGER NOT NULL,
  PRIMARY KEY (sku, region),
  UNIQUE (region, cents)
);
```

A leading run of key columns pinned by equality (here `sku`, or `sku` and
`region`) plus a range on the next one is answered from a window over the rows
themselves. `UNIQUE` constraints become indexes named
`sqlite_autoindex_<table>_<n>`, as in SQLite.

The planner looks at the conjuncts constraining each table and picks the
narrowest of a rowid or key lookup, an index lookup (equality on a leading run
of index columns, `IN` lists, plus a range on the column after them) and a full
scan. Inner joins are reordered so the most selective access runs outermost
(`CROSS JOIN` pins the order, as in SQLite); each inner table is planned with
the outer tables' columns already bound, so `ON a.id = b.a_id` becomes a lookup
rather than a second scan, and an inner table that does not depend on the outer
ones is read once and cached. Whatever the plan, every predicate is re-checked
against the row, so a plan is only ever allowed to be too wide, never too
narrow. `EXPLAIN QUERY PLAN` shows the choice.

## Transactions

`Database::exec` runs every `;`-separated statement in its argument inside one
transaction: committed on return, rolled back if anything throws. A batch that
contains transaction control (`BEGIN`, `COMMIT`, `SAVEPOINT`, ...) follows
SQLite's autocommit model instead, and a transaction opened by `BEGIN` stays
open across `exec` calls until `COMMIT` or `ROLLBACK`; `inTransaction()` says
whether one is. A failing statement undoes only its own changes, the way
SQLite's statement journal does, and `ON CONFLICT ROLLBACK`/`RAISE(ROLLBACK)`
end the whole transaction.

For a transaction driven from C++, use `Database::begin`, which returns a
`Transaction` that rolls back if it is destroyed without a `commit`, and inside
which `SAVEPOINT`/`RELEASE`/`ROLLBACK TO` work. `begin` takes the writer slot
and so waits until any other write transaction finishes -- indefinitely by
default, or for at most `PRAGMA busy_timeout` milliseconds (also settable as
`Database::configure().busyTimeout(...)`), after which it fails with `Busy`
("database is locked"). For multi-statement
reads use `Database::beginRead`, which takes a snapshot instead: it neither
blocks nor is blocked, and it rejects any statement that would write.

`changes()`, `totalChanges()` and `lastInsertRowid()` report what SQLite's
functions of the same names report; `interrupt()` stops running statements
from another thread.

## Prepared statements

`Database::prepare` parses once so that running costs no parsing, which is worth
it for anything in a loop. Parameters are bound 1-based as in SQLite, or by
name -- with its prefix (`":id"`) or without (`"id"`) -- and ones never bound
are NULL.

```cpp
sql::Statement insert = db.prepare("INSERT INTO book (title, price) VALUES (:title, ?)");
sql::Transaction t = db.begin();
for (const Book& b : books)
    insert.bind("title", sql::Value(b.title)).bind(2, sql::Value(b.price)).exec(t);
t.commit();
```

`exec()` runs in a transaction of its own; `exec(txn)` runs in one you control.
Both take an optional `Params` or `NamedParams` to bind and run in a single
call. Values in braces, `insert.exec(t, {sql::Value(id), sql::Value::blobRef(p, n)})`,
are bound in place: each is copied into the statement's own parameter cell,
which keeps its buffer, so no vector is built and a warm call allocates
nothing. A `Value::textRef` or `Value::blobRef` stays a reference, as it does
through `bind`, so its bytes must outlive the run. `parameterIndex`,
`parameterName` and `columnNames` describe the statement; `sql::complete(text)`
tells a prompt whether a statement is finished.

A prepared statement also keeps its plan -- the columns it bound, the access
path it chose for each table, the layout and names of its result -- and reuses
it for as long as the schema it was planned against is the one the transaction
sees. Any DDL makes the next run plan again, so a prepared statement keeps
working after the schema changes and picks up an index created after it was
prepared. A statement belongs to the database it came from and holds it open.
It is move-only and not safe to run from two threads at once.

Results own their values, so a `Result` outlives the transaction that produced
it. For large results, `each` hands the rows to a callback as they are
produced instead, with nothing collected:

```cpp
sql::Statement scan = db.prepare("SELECT id, name FROM book WHERE price < ?");
scan.bind(1, sql::Value(20.0)).each([&](const sql::RowView& r) {
    index.add(r[0].integer(), r["name"].text());  // valid only during the call
    return true;                                  // false stops the statement
});
```

The callback sees the rows without a copy: `text()` is a `std::string_view` and
`blob()` a `sql::BlobView`, and in a row from `each` they may point straight
into the database pages. Copy the `Value` (or call `r.row()`) to keep one. A
lambda is called where it is, through a `sql::RowVisitor`, never copied into a
`std::function`, whatever it captures; a `RowCallback` (`std::function`) is
still accepted.

To keep rows after the transaction without paying for a fresh `Result` each
time, run into one you keep:

```cpp
sql::Result rows;
sql::Transaction t = db.beginRead();
vectorsOf.bind(1, sql::Value(id)).execInto(t, rows);  // returns rows
```

`execInto(txn, out)` refills the rows `out` holds from its last run of the same
statement in place, each cell keeping its buffer, so a warm run allocates
nothing. `out` keeps the rows of its largest run until it is destroyed; running
another statement into it starts it afresh.

An `ORDER BY` that the access path already delivers costs no sort: one table
read through its primary key, its rowid or an index, ordered by the key parts
after the ones `WHERE` pins with `=`, ascending and `BINARY` (an index's
entries continue with the row key). The rows then stream to `each` with their
cells still borrowed. All-descending terms that reach the end of the primary
key (or the rowid) walk the window backward instead, so `ORDER BY slot DESC
LIMIT 1` reads one row. `EXPLAIN QUERY PLAN` shows `USE TEMP B-TREE FOR ORDER
BY` only when a sort remains. A lone `min(c)` or `max(c)` of the part after the
pinned ones, with every `WHERE` term enforced by that window, is one seek
instead of a scan: `SELECT max(slot) FROM embedding WHERE record_id = ?` reads
one row however many samples the record has.

`ORDER BY` without `LIMIT` over more rows than fit in the sort memory
(`Database::configure().sortMemoryLimit(bytes)`, 64 MiB by default) writes
sorted runs to a temporary file and merges them; with `LIMIT` it only ever
holds the rows that can still make the cut. `PRAGMA temp_store = MEMORY` keeps
every sort in memory.

### Streaming a result to your own writer

`db.exec(text, params, consumer)` (also on `Transaction` and `Statement`) runs
like `exec`, with the same transactions and errors, but hands the rows of the
last statement to a `sql::ResultConsumer` instead of building a `Result`:
`begin(shape)` with column names, `alias.column` names and declared types (not
called when the statement returns no columns), `row(RowView)` per row (return
false to stop; copy what you keep, the cells may borrow from the file), then
`end(changes, lastInsertId)`. A warm call with a consumer that allocates
nothing allocates nothing per row.

## The store underneath

`Database::env()` returns the `nosql::Env` the tables live in, and
`Transaction::txn()` the `nosql::Txn` a transaction runs on. They exist so other
libnosql layers can share the file: `nosql::BlobStorage::open(db.env(), "images")`
keeps bulk payloads in a side-car tar archive with its index in this database,
and `blobs.beginWrite(t.txn())` puts the appends under the same commit as the
rows that describe them. Both are borrowed -- valid while the database is open
and the transaction live. The sub-database names `sql_catalog`, `t#*` and
`i#*` belong to libsql.

The catalog stores each object's `CREATE` statement, exactly as
`sqlite_schema.sql` shows it, and re-reads the schema from that text. This
release changed the format of keys, rows and the catalog, and runs on
libnosql's format 7: a store written by libsql 0.1, or by an earlier 0.2
development build, is refused rather than misread.

libnosql verifies a page's checksum when it reads it. libsql turns on its
read cache: a page whose checksum this process has verified since it was last
written is not re-hashed on the next read, which roughly halves the cost of a
point lookup and of every probe inside a join. The trade is that damage done to
the file from outside is noticed on the next re-verification -- every minute by
default, `cacheReadChecksums(true, interval)` to change it -- rather than the
next read; writes always verify. `Database::configure().cacheReadChecksums(false)`
verifies every page on every read; `revalidateAfter(interval)` sets the
interval alone.

How the store flushes, and how it uses memory, are libnosql's settings, which
`Database::Options` passes through:

```cpp
sql::Database db = sql::Database::configure()
                       .durability(nosql::Durability::Lazy)       // any libnosql mode
                       .syncInterval(std::chrono::seconds(1))
                       .pageSize(4096)
                       .initialSize(64ull << 20)
                       .growthStep(16ull << 20)
                       .bufferCache(16ull << 20)
                       .dirtyLimit(64ull << 20)
                       .open(path);
```

`durable(true)`, the default, is `nosql::Durability::Safe`: every commit is
flushed. `durable(false)` is `nosql::Durability::None`: commits reach the
device only on `nosql::Env::sync()`, on close, or when libnosql decides to, and
a crash loses the ones since then. After a process crash the store reopens on
a consistent snapshot either way. After a power loss it does so only with a
libnosql that keeps the newest meta page in memory until a sync (deferred
meta); with an older one, `None` leaves meta and data pages unordered on the
device, so do not open a store that must survive a power cut with
`durable(false)` there. `durability(mode)` takes any `nosql::Durability` value
(`Safe`, `NoMetaSync`, `None`, `Lazy`, ...) and wins over `durable`;
`syncInterval` is the interval of `Lazy`. The others are as libnosql documents
them: the page size of a new store, the size a new file starts at and the least
it grows by, the page buffers kept between transactions, and the most dirty
bytes one write transaction may hold (more fails with
`nosql::ErrorCode::OutOfMemory`). Left unset, each keeps libnosql's default.

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
  --listen PORT serve a web interface on PORT, after any -c or -f
  --host ADDR   with --listen, bind to ADDR (default 127.0.0.1; there is no login)
  --public DIR  with --listen, serve the files of DIR instead of the built-in page
```

With no `-c` and no `-f` it reads statements from stdin -- a statement ends at
a `;` outside quotes, comments and trigger bodies -- and offers `.tables`,
`.schema [TABLE]` (the `CREATE` statements as they were written), `.dump
[TABLE]` (a script that rebuilds the schema and the rows), `.indexes [TABLE]`,
`.mode`, `.headers`, `.help` and `.quit`.

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

`--listen PORT` serves a browser front end that works like DB Browser for
SQLite: the structure of the database, a grid to browse and edit a table, the
pragmas, and an Execute SQL tab. See [tools/sql/README.md](tools/sql/README.md).

## Performance

`benchmarks/bench_sql` runs a fixed set of statement shapes over a 200,000-row
table with two indexes and a 400,000-row joined table, and reports the median
ns/op of each together with the heap allocations it made. Run it pinned to a
core in Release; `--json` and `--csv` write the figures out, `--only` picks
workloads, `--no-cache` turns the read-checksum cache off and `--durable`
fsyncs every commit.

`benchmarks/bench_vs_sqlite` (built when both `SQL_BUILD_BENCHMARKS` and
`SQL_BUILD_CONFORMANCE` are on) runs the same kind of workloads, on the same
data and schema, against libsql and against the SQLite 3.45.1 amalgamation the
conformance harness fetches, and prints the two side by side. Each engine gets
its fastest honest setup: libsql with `durable(false)`; SQLite in WAL mode with
`synchronous=OFF`, a 256 MiB page cache, a 1 GiB mmap and `THREADSAFE=0`. Both
read every column of every row in place -- `sqlite3_column_*` on one side,
`Statement::each` on the other (`--materialize` makes libsql build a `Result`
instead). On a Xeon Gold 5420+ VM (GCC 13.3, one pinned core), with libsql
built plain Release and with profile-guided optimization (`tools/pgo-build.sh`,
see Building):

| workload | libsql | libsql PGO | SQLite | PGO / SQLite |
|---|---:|---:|---:|---:|
| `INSERT`, 2 indexes, prepared, batches of 1,000 | 3.89 µs | 3.63 µs | 4.85 µs | 0.75x |
| the same, one transaction per row | 9.2 µs | 8.9 µs | 11.9 µs | 0.75x |
| `INSERT` prepared and run once each | 9.3 µs | 8.9 µs | 14.3 µs | 0.63x |
| `SELECT ... WHERE id = ?`, prepared, own transaction | 1.45 µs | 1.27 µs | 1.86 µs | 0.68x |
| the same inside one read transaction | 0.53 µs | 0.48 µs | 0.72 µs | 0.67x |
| `SELECT ... WHERE id = ?` prepared and run once each | 1.46 µs | 1.35 µs | 4.18 µs | 0.32x |
| `SELECT id FROM item WHERE name = ?` | 1.82 µs | 1.66 µs | 2.03 µs | 0.82x |
| index equality returning ~200 rows | 55 µs | 44 µs | 72 µs | 0.61x |
| rowid range of 100 rows | 8.9 µs | 7.8 µs | 7.9 µs | 0.98x |
| index equality, `ORDER BY name LIMIT 10` | 69 µs | 54 µs | 83 µs | 0.64x |
| `count(*)` of ~200 rows through an index | 1.76 µs | 1.56 µs | 6.5 µs | 0.24x |
| `count(*)` of the whole table | constant | constant | 0.1 ns/row | |
| full scan with a predicate, per row | 24.1 ns | 24.7 ns | 34.7 ns | 0.71x |
| `GROUP BY` over a full scan, per row | 140 ns | 121 ns | 376 ns | 0.32x |
| `ORDER BY price LIMIT 100` over a full scan, per row | 53 ns | 51 ns | 54 ns | 0.95x |
| covering index scan returning 100,000 names, per row | 21.1 ns | 18.8 ns | 19.4 ns | 0.97x |
| `SELECT DISTINCT cat` (1,000 values) over 200,000 rows, per row | 2.1 ns | 1.7 ns | 1.9 ns | 0.89x |
| full sort of 200,000 rows, per row | 524 ns | 483 ns | 693 ns | 0.70x |
| join through an index, ~400 output rows | 265 µs | 237 µs | 368 µs | 0.64x |
| join with `GROUP BY` over 1,000 items | 1.16 ms | 0.91 ms | 1.29 ms | 0.71x |
| `UPDATE ... WHERE id = ?`, batches of 1,000 | 2.7 µs | 2.4 µs | 4.3 µs | 0.55x |
| `UPDATE` of an indexed column, batches of 1,000 | 6.1 µs | 5.8 µs | 10.4 µs | 0.55x |
| `UPDATE ... WHERE id = ?`, one transaction each | 7.3 µs | 6.7 µs | 7.8 µs | 0.86x |
| `DELETE` + re-`INSERT` by key, batches of 1,000 | 12.6 µs | 12.3 µs | 23.1 µs | 0.53x |
| `prepare` and discard a `SELECT` | 2.7 µs | 1.9 µs | 2.5 µs | 0.75x |
| `prepare` and discard a join | 3.7 µs | 2.6 µs | 5.2 µs | 0.51x |
| a trivial statement run once | 1.48 µs | 1.23 µs | 3.23 µs | 0.38x |

The SQLite column is from the plain build's tree, which compiles the
amalgamation with LTO; the PGO tree compiles it with `-fprofile-use`, no
profile and no LTO, which makes it 10-20% slower, so its own SQLite figures
are not used. A plain build is ahead on 23 of the 27, a PGO build on all 27.

What the engine does to get there:

- **Checksums verified once.** libnosql checksums every page; libsql turns on
  its read cache by default, so a page verified since it was last written is
  not re-hashed on every read (every remembered check expires after a minute,
  and writes always verify). `cacheReadChecksums(false)` restores
  verify-every-read.
- **Rows decode in two phases.** A row record starts with a header of type
  codes, so a column is found without parsing the ones before it. A scan
  decodes first only the columns its filters (or an `ORDER BY ... LIMIT`'s
  sort keys) read, and the rest only for rows that pass; columns nothing reads
  are never decoded.
- **Filters test the stored row.** A comparison of a column with a numeric
  constant is tested on the row's record before the row is decoded: the scan
  walks the type codes to that column and compares the stored integer or real
  where it lies, so a row the filter rejects is never decoded. The predicate
  scan, which rejects 199 of every 200 rows, went from 41.5 ns to 24 ns a row.
- **Comparisons without the interpreter.** `column <op> constant` and
  `column <op> column` compare the two values directly when affinity cannot
  change either, instead of evaluating three expression nodes.
- **Index-only scans and exact windows.** When an index holds every column a
  query reads, the table is never touched; equality and ranges on key and
  index parts become exact key windows that are not re-checked per row.
- **The store counts.** Every branch page in libnosql records how many entries
  lie under each child, so `count(*)` over an exact window, and an `OFFSET`
  over one, take O(log n) instead of visiting the entries (an `OFFSET` of
  150,000: 2.2 µs).
- **Top-N.** `ORDER BY ... LIMIT n` keeps only rows that can still make the
  cut; a row that cannot is dropped after its sort keys are computed, before
  the rest of it is decoded or copied.
- **No sort the key already makes.** An `ORDER BY` of the key parts after the
  pinned ones streams the rows as the window yields them (backward for
  `DESC` over a unique key), and a lone `min()` or `max()` of such a part is
  one seek. isvc's `WHERE record_id = ? ORDER BY slot` over 50 rows of 512-byte
  blobs went from 50 allocations to none through `each`, and its
  `MAX(slot)` from a scan of the record to one row.
- **Hash joins.** An equality join with no usable index on the inner table
  reads that table once and buckets it by the join key -- SQLite's automatic
  index -- instead of re-reading it for every outer row.
- **Flat hash tables.** `GROUP BY`, `DISTINCT`, `count(DISTINCT ...)` and
  `IN (SELECT ...)` key sets are open-addressing tables over one byte buffer,
  reused from run to run.
- **Nearby seeks stay in the leaf.** A source keeps its cursor between opens,
  and a seek that lands in the cursor's current leaf skips the descent from
  the root.
- **Page searches guess first.** A descent knows the separators either side
  of the page it enters, so it guesses the target's slot from them and
  gallops out from the guess. Rowid keys and most index keys land within one
  or two probes, where a binary search takes six or seven dependent cache
  misses; keys compare eight bytes at a time, inline. A join through an index
  went from 420 µs to 252 µs.
- **Cheap commits.** A small commit records the pages it frees, and the roots
  of up to six recently written tables, in the meta page it writes anyway,
  so it rewrites neither a free-list page nor the catalog: an autocommit
  `UPDATE` went from 13.1 µs to 7.5 µs.
- **Cheap prepare.** Statement trees live in a per-statement arena, keywords
  are recognized through one small static table, and a per-connection cache of
  parsed batches means `exec` of a repeated text plans once.
- **Text stays in the page.** A `SELECT` leaves the text and blobs it reads
  where the store keeps them: a `Value` can borrow its bytes, so a table or
  index read sets a pointer and a length instead of copying. Rows reach a
  `Statement::each` callback that way; a `Result`, a sort, a cache or a copy
  takes its own bytes.
- **Small cells.** A `Value` is a 32-byte tagged union rather than a
  `std::variant` of a string and a vector: up to 24 bytes of text sit inside
  the cell, and copying, swapping or resetting one is a few stores. With
  borrowed text, a rowid range went from 13.7 µs to 8.9 µs and a covering scan
  from 24.8 ns to 21.1 ns per row (plain build).
- **No copies on the way out.** Result rows hold up to four values inline and
  wider rows share a few chunks of their `Result`; an output that is a plain
  column takes or borrows its value from the row instead of copying it, and
  outputs that are a consecutive run of one table's columns go out as the row
  itself; `Statement::each` skips building a `Result` altogether.

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

Scratch that outlives a statement is bounded so that one unusual query cannot
pin memory for the life of the process: a plan's sort buffer, group table and
DISTINCT/IN key sets are given back after a run that made them larger than the
connection's `scratchRetain` (see Memory below), a result is pre-sized from the
previous run only up to 65,536 rows, the parser's per-thread token buffers are
kept only up to 4,096 tokens and four at a time, and an `ORDER BY` larger than
the sort memory goes to a temporary file rather than the heap. A statement's
parse tree lives in a few arena blocks of its own, freed together, so dropping
statements leaves no small holes behind.

The flat key sets used for grouping and DISTINCT/IN grow only for a new key,
not for a repeated key at the load limit. A failed allocation while adding key
bytes cannot publish a partial member. The soak suite injects these failures
and checks that existing keys survive and insertion can be retried; it also
requires zero allocations and unchanged retained bytes for repeated keys at
the load limit.

### Memory

What a `Database` keeps between statements so as not to allocate it again is
set by a profile, and each figure can be set on its own as well (an explicit
setting wins whatever order it is given in):

```cpp
sql::Database db = sql::Database::configure()
                       .memoryProfile(sql::MemoryProfile::Compact)
                       .statementCacheSize(16)
                       .open(path);
```

| profile | `statementCacheSize` | `scratchRetain` | `sortMemoryLimit` | `arenaBlockSize` |
|---|---:|---:|---:|---:|
| `Compact` | 8 | 64 KiB | 8 MiB | 1 KiB |
| `Balanced` (default) | 32 | 2 MiB | 64 MiB | 4 KiB |
| `Throughput` | 128 | 16 MiB | 256 MiB | 16 KiB |

- `statementCacheSize`: one-off statement texts kept parsed and planned, per
  `Database` and per transaction; 0 turns the cache off.
- `scratchRetain`: per-run working memory a statement keeps for its next run.
- `sortMemoryLimit`: what an `ORDER BY` holds before it spills to a temporary
  file.
- `arenaBlockSize`: the block size of each parsed statement's arena.

A reader from `openReader()` keeps the figures of the `Database` it came from.

The soak also re-checks, at every sample point, that each index returns exactly
what a full scan finds. A stale index entry is the failure mode that a long run
produces and a short one hides.

## Errors

Everything the SQL layer rejects throws `sql::Error`, carrying a
`sql::ErrorCode`: `SyntaxError`, `Unsupported`, `InvalidArgument`,
`NoSuchTable`, `TableExists`, `NoSuchColumn`, `AmbiguousColumn`, `NoSuchIndex`,
`IndexExists`, `TypeMismatch`, `ConstraintViolation`, `BadTransaction`,
`ReadOnly`, `Runtime`, `Busy`, `Interrupted`, `Internal`. A
`ConstraintViolation` also says which kind through `Error::constraint()`
(`PrimaryKey`, `Unique`, `NotNull`, `Check`, `ForeignKey`, `Trigger`, ...).
Messages are SQLite's where SQLite has one ("UNIQUE constraint failed: t.a",
"no such column: x"). Planning errors are `InvalidArgument`, errors while
running are `Runtime`, an unknown function is `Unsupported`. Storage-level
failures keep arriving as `nosql::Error`.

## Where it differs from SQLite

Deliberately, and nowhere else:

- the `DATETIME` storage class and `FOR JSON`, above;
- `Database::exec` runs a batch without transaction control as one
  transaction (`sqlite3_exec` commits each statement);
- primary key columns are always `NOT NULL`; tables with a declared key are
  clustered by it, so an unordered scan comes out in key order; their hidden
  rowids are never reused;
- double-quoted strings are always identifiers, never string literals
  (SQLite's recommended `DQS=0`);
- `EXPLAIN` lists libsql's plan operators rather than VDBE byte code, and
  `EXPLAIN QUERY PLAN` wording is close to SQLite's but not identical;
  `sqlite_schema.rootpage` is the object's id;
- attached databases are separate libnosql stores and commit one after the
  other, not atomically together (as SQLite in WAL mode);
- only `ORDER BY` spills to a temporary file; grouping, `DISTINCT`, compound
  selects, window partitions and materialized sub-queries stay in memory;
- `BEGIN EXCLUSIVE` does not block readers, which are lock-free snapshots;
  `PRAGMA busy_timeout = 0` (the default) makes a writer wait for the writer
  slot as long as it takes rather than fail at once, since the threads sharing
  a `Database` expect to take turns; `VACUUM` compacts nothing in place
  (`VACUUM INTO` copies).
- an unordered scan may come out in index order: a scan that a narrower index
  covers reads that index, as SQLite does.

Not implemented: `CREATE VIRTUAL TABLE` and every module (FTS, R*Tree,
`generate_series`, ...), `load_extension()`, a `REGEXP` function, the
`sqlite3_*` C API and SQLite's file format. Each is reported as an error
("no such module: fts5") rather than misread.

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
`SQL_BUILD_BENCHMARKS`, `SQL_BUILD_PYTHON`, `SQL_BUILD_CONFORMANCE` (the
SQLite differential harness; downloads the SQLite 3.45.1 amalgamation as a
test-only oracle), `SQL_ENABLE_ASAN`, `SQL_LTO` (link-time optimization; off,
since it measured slower on scans here), `SQL_PGO` (`GENERATE` or `USE`, with
`SQL_PGO_DIR` for the profiles). Benchmarks and
the Python binding are off by default; the binding is experimental and has not
been through the robustness review the C++ library has. When libsql builds its
sibling libnosql it compiles only the pieces it needs (`NOSQL_WITH_BLOB` for
the tests and the binding; `NOSQL_WITH_MQ` and `NOSQL_WITH_REPLICATION` off).
A profile-guided build is 5-40% faster than a plain one (see Performance);
`tools/pgo-build.sh <builddir> [cmake args]` configures with
`SQL_PGO=GENERATE`, trains on `bench_sql` and rebuilds the same tree with
`SQL_PGO=USE`. Train and ship from the same build tree: the profiles are keyed
by object paths.

C++17, 64-bit,
Linux and Windows, no third-party dependencies beyond libnosql itself. GCC 9.4
with CMake 3.16 -- NVIDIA JetPack 5.1.2's Ubuntu 20.04 -- is the oldest
toolchain the code is written for; nothing beyond C++17 is used.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
