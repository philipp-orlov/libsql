# libsql 0.2.0: design and implementation reference

This document describes the C++ libsql engine as it exists in version 0.2.0:
the SQLite-3.45 language implementation, the libsql extensions that remain,
the bytes written to libnosql, and the runtime rules that make the engine safe
to leave open for long periods. It is intentionally an implementation
reference, not a tutorial. A reader should be able to rebuild the engine from
this document and the public API contracts without first reverse-engineering
the source.

Paths are relative to this `docs/` directory. libsql itself is in the parent
directory; libnosql is the sibling checkout at `../../libnosql`.

The companion migration note,
[`internal/support-sqlite.md`](internal/support-sqlite.md), records the design
that led to the rewrite, the conformance evidence, and the divergence ledger.
This document records the implementation that was actually built.

---

## Contents

1. [Problem statement and goals](#1-problem-statement-and-goals)
2. [Architecture and life of a statement](#2-architecture-and-life-of-a-statement)
3. [The storage engine underneath](#3-the-storage-engine-underneath)
4. [Values, types, affinity and comparison](#4-values-types-affinity-and-comparison)
5. [Function registry and built-ins](#5-function-registry-and-built-ins)
6. [Encodings](#6-encodings)
7. [Catalog, schema and store layout](#7-catalog-schema-and-store-layout)
8. [Lexer](#8-lexer)
9. [Parser and AST](#9-parser-and-ast)
10. [Expression evaluation](#10-expression-evaluation)
11. [Planning and name resolution](#11-planning-and-name-resolution)
12. [Scanning and access paths](#12-scanning-and-access-paths)
13. [SELECT execution](#13-select-execution)
14. [DML, constraints, triggers and foreign keys](#14-dml-constraints-triggers-and-foreign-keys)
15. [DDL, schema tables, pragmas and maintenance statements](#15-ddl-schema-tables-pragmas-and-maintenance-statements)
16. [Transactions, savepoints and connection state](#16-transactions-savepoints-and-connection-state)
17. [`FOR JSON` and the JSON renderer](#17-for-json-and-the-json-renderer)
18. [Public C++ API](#18-public-c-api)
19. [Errors](#19-errors)
20. [The `sql` command-line tool](#20-the-sql-command-line-tool)
21. [Build system and source layout](#21-build-system-and-source-layout)
22. [Testing and conformance](#22-testing-and-conformance)
23. [Long-run behavior and memory reuse](#23-long-run-behavior-and-memory-reuse)
24. [Limitations and deliberate divergences](#24-limitations-and-deliberate-divergences)
25. [Rebuilding from scratch: suggested order](#25-rebuilding-from-scratch-suggested-order)
26. [Design decisions and rejected alternatives](#26-design-decisions-and-rejected-alternatives)
27. [Glossary](#27-glossary)

---

## 1. Problem statement and goals

libnosql is an ordered key/value store with named sub-databases, ACID commits,
single-writer/many-reader MVCC and a single-file layout. It has no schema,
rows, SQL parser, query planner or type system. libsql adds those layers while
leaving atomicity, durability and snapshot isolation to libnosql.

Version 0.2.0 is a full rewrite of the old subset engine. Its goals are:

1. **SQLite-language compatibility.** The target language is SQLite 3.45.1,
   except `CREATE VIRTUAL TABLE` modules and other deliberate divergences in
   [§24](#24-limitations-and-deliberate-divergences). The parser, planner and
   executor live in [`../src/internal/`](../src/internal/).
2. **Preserve libsql extensions.** `FOR JSON` and the public JSON renderer
   remain, and libsql keeps a sixth storage class, `DATETIME`, in addition to
   SQLite's `NULL`, `INTEGER`, `REAL`, `TEXT` and `BLOB`.
3. **Use libnosql's guarantees rather than reimplementing them.** Every
   statement runs in one or more libnosql transactions. Statement atomicity is
   implemented with nested libnosql transactions when a larger SQL transaction
   is active.
4. **Keep long-running processes bounded.** Plans, row buffers, aggregate
   state, JSON writers, catalogs and transaction objects are reused. Large
   one-off allocations are explicitly shrunk when they would otherwise pin
   memory.
5. **Keep the file shareable.** Applications may keep other libnosql
   sub-databases in the same file. libsql reserves the catalog and data/index
   sub-database names documented in [§7](#7-catalog-schema-and-store-layout).
6. **Stay C++17 and dependency-light.** The library depends on libnosql and
   the C++ standard library. The SQLite amalgamation is used only by the
   conformance harness.

The public README, [`../README.md`](../README.md), is the user-facing feature
summary. This document describes how those features are implemented.

---

## 2. Architecture and life of a statement

The main layers are:

```text
application
  │
  ├─ include/sql/value.hpp       Value, Type, Datetime
  ├─ include/sql/error.hpp       Error and ErrorCode
  ├─ include/sql/sql.hpp         Database, Transaction, Statement, Result
  └─ include/sql/json.hpp        JsonOptions, JsonWriter, toJson
       │
       ▼
../src/database.cpp
  connection state, batch cache, transactions, prepared statements,
  SQL transaction control, ATTACH/DETACH, VACUUM INTO, public API glue
       │
       ├─ internal/lexer.*       SQL text -> Token stream
       ├─ internal/parser.*      Token stream -> AST in ast.hpp
       ├─ internal/schema.*      decoded immutable Schema objects
       ├─ internal/catalog.*     sql_catalog and table/index sub-databases
       ├─ internal/plan_select.* name resolution and SELECT planning
       ├─ internal/access.*      table/index access paths
       ├─ internal/eval.*        expression evaluation
       ├─ internal/run_select.*  SELECT execution
       ├─ internal/dml.*         Writer, constraints, triggers, FKs
       ├─ internal/statement.*   INSERT/UPDATE/DELETE/RETURNING/upsert plans
       ├─ internal/ddl.*         CREATE/DROP/ALTER/ANALYZE/REINDEX
       ├─ internal/pragma.*      PRAGMA, schema tables, pragma_* TVFs
       ├─ internal/func_*.cpp    scalar, aggregate, date, printf, JSON funcs
       └─ internal/encoding.*    on-disk key and row bytes
            │
            ▼
        nosql::Env / nosql::Txn / nosql::Db
```

All internal implementation types are in namespace `sql::internal` and are not
installed. [`../include/sql/sql.hpp`](../include/sql/sql.hpp) is the normal
header for applications; it includes the value and error headers.

### 2.1 Statement objects and batches

[`../src/database.cpp`](../src/database.cpp) uses a `Batch` for every SQL text.
A `Batch` owns:

- the original SQL text;
- the parser's `parameterNames`;
- `StatementMeta` for each statement (`mutating`, transaction `control`,
  connection-level, pragma writes);
- unplanned AST nodes in `fresh`;
- lazily planned `Program` objects in `programs`;
- booleans summarizing whether the batch contains transaction-control or
  connection statements and whether it needs a writer.

One-off `Database::exec` calls use a `BatchCache` with `kSize = 32`. Each
`TxnImpl` also has its own `BatchCache`, so loops inside an explicit
transaction avoid repeated parse/plan work.

### 2.2 Program and frame

A planned statement is a `Program` in
[`../src/internal/plan.hpp`](../src/internal/plan.hpp). It owns the AST node it
plans, any ASTs parsed from views or trigger bodies, sub-query plans, select
plans, generated expressions and a flat `frame` of `Value` slots. Name
resolution annotates expression nodes with slot numbers, function definitions,
collations, affinity and sub-query plans. Execution then walks the annotated
tree and reads or writes known slots.

The frame is shared by all operators in the statement:

- table sources occupy contiguous slot ranges;
- aggregate and window results occupy slots;
- pseudo sources (`NEW`, `OLD`, `excluded`) occupy slots;
- correlated sub-queries read outer values by slot number.

The runtime `Ctx` in [`../src/internal/exec.hpp`](../src/internal/exec.hpp)
passes the slot array, bound parameters, `Session`, function context, trigger
depth and execution run id to every evaluator and runner.

### 2.3 Life of `Database::exec("SELECT ...")`

For a batch without SQL transaction-control statements:

1. `Database::exec` looks up or builds a `Batch`, checks that positional
   bindings supply at least the largest parameter slot, and calls
   `execBatch`.
2. `execBatch` opens one transaction for the whole batch. It opens a writer
   when any statement mutates or writes a persistent pragma; otherwise it
   opens read snapshots.
3. `start` opens libnosql transactions for every open store and opens a
   `Catalog` for each one. The catalog loads or reuses the schema snapshot
   visible to that transaction.
4. `runOne` checks read-only state, starts or upgrades the transaction if
   needed, ensures the statement is planned, then runs `runProgram`.
5. A `SELECT` calls `runSelect`; non-SELECT statements call their `ExecPlan`.
   Result rows are copied into a `ResultBuilder`, so the result outlives the
   transaction.
6. On success, writable roots flush catalog rowid/sequence state and commit.
   Read roots abort. Pooled `TxnImpl` objects are parked for reuse.

If the batch contains `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE` or
`ROLLBACK TO`, `execBatch` follows SQLite's SQL transaction model instead:
statements outside an explicit SQL transaction commit individually, and a SQL
transaction kept in `DatabaseImpl::sqlTxn` persists across calls until it ends.

### 2.4 Prepared statements

`Database::prepare` parses the text into a private `Batch` and creates a
move-only `Statement`. The statement stores its bound `Value` vector. Binding
by name accepts either the exact spelling (`:id`, `@id`, `$id`, `?3`) or an
unprefixed name that matches any named parameter with a non-`?` prefix.

Prepared statements keep their plans until a schema they depend on changes.
`pinSchemas` records each store's `std::shared_ptr<const Schema>` and its
per-edit schema stamp. `planCurrent` rejects a plan if the number of stores,
schema pointer or schema stamp differs. The stamp matters inside a transaction:
DDL edits a transaction-local schema copy in place, so pointer identity alone
would not notice a second DDL statement.

One-off texts get the same treatment through a `BatchCache` per `Database`
and per transaction: an LRU of parsed batches indexed by text, holding up to
`statementCacheSize` entries (`MemoryProfile`). Entries are shared pointers,
so a batch that is running survives being evicted by what it runs, or the
cache being cleared by its own `DETACH`.

Each parsed statement owns an `Arena` (`StatementNode::arena`, declared before
the body so the body is destroyed first). While a statement is parsed,
`makeExpr` places nodes in it -- `ExprPtr`'s deleter only destructs a node
marked `inArena` -- so a statement tree is a few blocks of
`arenaBlockSize` rather than one heap allocation per node. Nodes the planner
makes later stay on the heap.

---

## 3. The storage engine underneath

libsql uses these libnosql capabilities:

| Need | libnosql surface | Where libsql uses it |
| --- | --- | --- |
| Open a store with durability, read-only, max size and checksum-cache options | `nosql::Env::configure()` | `Database::Options::open` |
| Share an open environment with a read-only handle | `Env::share()` | `Database::openReader` |
| Open a writer or reader | `Env::writeTxn()`, `Env::readTxn()` | all statement execution |
| Nested transactions | `Txn::nested()` | savepoints and statement atomicity |
| Named sub-databases | `Txn::db(name, DbFlags)` | `sql_catalog`, `t#<id>`, `i#<id>` |
| Check/drop/list sub-databases | `Txn::hasDb`, `Txn::dropDb`, `Txn::listDbs` | catalog load/drop and `VACUUM INTO` |
| Ordered keys | `Db::cursor()`, cursor seek/first/next/last | scans, indexes, rowid allocation |
| Point lookups and writes | `Db::get`, `Db::put`, `Db::erase`, `Db::clear` | rows, indexes, catalog |
| Insert-if-absent | `PutMode::InsertUnique` | primary-key conflict deferral |
| Count rows | `Db::count()` | `ANALYZE` |

The SQL layer assumes keys compare as unsigned byte strings and that cursors
see writes made earlier in the same transaction. Every order-preserving
encoding in [§6](#6-encodings) relies on bytewise order.

### 3.1 Multiple stores

A `Session` can see several stores:

- store 0 is `main`;
- store 1 is `temp`, opened lazily as a scratch libnosql file;
- stores 2 and later are attached databases.

`ATTACH` refuses `main` and `temp`, refuses duplicate schema names, and allows
at most ten attached databases (`db.stores.size() >= 12` is rejected). `:memory:`
and empty paths are scratch stores. Scratch stores are deleted by
`StoreHandle::~StoreHandle`.

Transactions over attached stores are coordinated by libsql but are not a
single libnosql transaction. Commits happen store by store.

---

## 4. Values, types, affinity and comparison

The public `sql::Value` type is defined in
[`../include/sql/value.hpp`](../include/sql/value.hpp) and implemented in
[`../src/value.cpp`](../src/value.cpp). It stores one of six storage classes:

| `Type` | Stored payload | Notes |
| --- | --- | --- |
| `Null` (`0`) | `std::monostate` | SQL NULL |
| `Integer` (`1`) | signed 64-bit integer | SQLite INTEGER |
| `Real` (`2`) | `double` | SQLite REAL |
| `Datetime` (`3`) | `Datetime{usec}` | libsql extension; microseconds since Unix epoch UTC |
| `Text` (`4`) | `std::string` | byte string treated as UTF-8 where SQL requires characters |
| `Blob` (`5`) | `std::vector<std::byte>` | opaque bytes |

The enum order is also the row-value tag order. SQL sort order is defined by
`compareValues` in [`../src/internal/types.cpp`](../src/internal/types.cpp):

```text
NULL < INTEGER/REAL < DATETIME < TEXT < BLOB
```

Integers and reals compare numerically with one another. `DATETIME` values
sort by their microsecond count and sort between numbers and text. Text
comparison uses one of three collations:

- `BINARY`;
- `NOCASE`, ASCII-only folding;
- `RTRIM`, ignoring trailing spaces.

### 4.1 Affinity

`Affinity` is internal and follows SQLite's five-affinity model:

- type names containing `INT` -> `INTEGER`;
- containing `CHAR`, `CLOB` or `TEXT` -> `TEXT`;
- empty or containing `BLOB` -> `BLOB`;
- containing `REAL`, `FLOA` or `DOUB` -> `REAL`;
- otherwise -> `NUMERIC`.

`applyAffinity` is used when storing into ordinary tables. It converts toward
the column affinity when SQLite would do so losslessly enough for storage:
numeric text may become integer or real, integer real values may become
integer under numeric/integer affinity, and numbers may become text under text
affinity. `DATETIME` and `BLOB` values are left alone by ordinary affinity.

`applyComparisonAffinity` is separate. It implements SQLite's comparison-time
rules, including the narrower numeric conversion for text.

### 4.2 STRICT types

Strict tables use `StrictType`:

| Strict type | Accepted declared names |
| --- | --- |
| `Any` | `ANY` |
| `Integer` | `INT`, `INTEGER` |
| `Real` | `REAL` |
| `Text` | `TEXT` |
| `Blob` | `BLOB` |
| `Datetime` | `DATETIME` |

`DATETIME` in a `STRICT` table is a libsql extension. `strictCoerce` accepts:

- `DATETIME` values directly;
- text parseable by `parseDatetime`;
- integers, interpreted as microseconds since the epoch.

It rejects real values for `DATETIME`. For other strict types it follows
SQLite-style lossless coercion. Failures raise
`ConstraintViolation` with `Constraint::Datatype`, except for rowid datatype
mismatches that use SQLite's `"datatype mismatch"` text.

### 4.3 Datetime text forms

`parseDatetime` accepts:

- `YYYY-MM-DD`;
- `YYYY-MM-DD HH:MM`;
- `YYYY-MM-DD HH:MM:SS`;
- optional fractional seconds up to six digits;
- `T` in place of the space;
- optional trailing `Z`.

Values are stored as UTC microseconds. `formatDatetime` produces
`YYYY-MM-DD HH:MM:SS[.ffffff]`. `formatDatetimeIso` produces
`YYYY-MM-DDTHH:MM:SS[.ffffff]Z` into a caller-supplied buffer of at least
32 bytes.

### 4.4 Text, UTF-8 and patterns

`length()` uses `utf8Length`, modeled on SQLite's `SQLITE_SKIP_UTF8` rule:
a lead byte `>= 0xC0` consumes following continuation bytes, while malformed
bytes still count as characters. `LIKE` and `GLOB` use `likeMatch` and
`globMatch`; `LIKE` is ASCII-case-insensitive unless
`PRAGMA case_sensitive_like` is set.

### 4.5 Subtypes

`Value` carries a SQLite-style subtype byte that is never stored. Setters clear
it. `kJsonSubtype` is `'J'`; JSON functions and `FOR JSON` set it on text that
contains JSON so nested JSON is embedded raw instead of quoted as a string.

---

## 5. Function registry and built-ins

The function system is declared in
[`../src/internal/func.hpp`](../src/internal/func.hpp). A process-wide
`FunctionRegistry::builtins()` owns:

- scalar `FunctionDef`s;
- aggregate factories;
- table-valued `TableFunctionDef`s.

Functions resolve during planning by name and arity. The executor then calls a
function pointer; it does not look names up at runtime. Flags include:

- `kDeterministic`, required in generated columns, indexes and checks;
- `kInvertible`, allowing aggregate window frames to slide with `inverse`;
- `kNeedsCollation`, so the caller sets `FuncContext::collation`;
- `kSubtypeAware`, for JSON subtype propagation.

`FuncContext` also stores:

- the connection state for `changes()`, `last_insert_rowid()` and pragmas;
- the collation for the current call;
- a statement-stable `nowUsec` timestamp for date/time functions;
- two reusable scratch strings.

### 5.1 Core and math functions

[`../src/internal/func_core.cpp`](../src/internal/func_core.cpp) registers the
core SQLite-style scalars, including `abs`, `changes`, `char`, `coalesce`,
`concat`, `concat_ws`, `glob`, `hex`, `ifnull`, `iif`, `instr`,
`last_insert_rowid`, `length`, `like`, `likelihood`, `likely`, `lower`,
`ltrim`, scalar `min`/`max`, `nullif`, `octet_length`, `quote`, `random`,
`randomblob`, `replace`, `round`, `rtrim`, `sign`, `soundex`,
`sqlite_compileoption_get`, `sqlite_compileoption_used`, `sqlite_source_id`,
`sqlite_version`, `libsql_version`, `substr`, `substring`, `total_changes`,
`trim`, `typeof`, `unhex`, `unicode`, `unlikely`, `upper` and `zeroblob`.

`load_extension` is registered so SQL parses like SQLite, but it raises the
runtime error `"not authorized"`.

Math functions registered from the same file include `acos`, `acosh`, `asin`,
`asinh`, `atan`, `atan2`, `atanh`, `ceil`, `ceiling`, `cos`, `cosh`,
`degrees`, `exp`, `floor`, `ln`, `log`, `log10`, `log2`, `mod`, `pi`, `pow`,
`power`, `radians`, `sin`, `sinh`, `sqrt`, `tan`, `tanh` and `trunc`.

### 5.2 Aggregates and window use

[`../src/internal/func_aggregate.cpp`](../src/internal/func_aggregate.cpp)
registers `count`, `sum`, `total`, `avg`, aggregate `min`/`max`,
`group_concat` and `string_agg`. Aggregates own an `AggregateState` per group
or per window frame. `count`, `sum`, `total`, `avg`, `group_concat` and
`string_agg` are marked invertible; aggregate `min`/`max` need collation.

The planner also recognizes built-in window-only names:
`row_number`, `rank`, `dense_rank`, `percent_rank`, `cume_dist`, `ntile`,
`lag`, `lead`, `first_value`, `last_value` and `nth_value`.

### 5.3 Date, printf and JSON functions

[`../src/internal/func_date.cpp`](../src/internal/func_date.cpp) registers
`julianday`, `unixepoch`, `date`, `time`, `datetime`, `strftime`, `timediff`,
`current_date`, `current_time` and `current_timestamp`.

[`../src/internal/func_printf.cpp`](../src/internal/func_printf.cpp) registers
`printf` and `format`, backed by `sqlitePrintf`.

[`../src/internal/func_json.cpp`](../src/internal/func_json.cpp) implements the
JSON scalar, aggregate and table-valued functions. Scalars include `json`,
`jsonb`, `json_array`, `jsonb_array`, `json_array_length`,
`json_error_position`, `json_extract`, `jsonb_extract`, `json_insert`,
`jsonb_insert`, `json_object`, `jsonb_object`, `json_patch`, `jsonb_patch`,
`json_quote`, `json_remove`, `jsonb_remove`, `json_replace`,
`jsonb_replace`, `json_set`, `jsonb_set`, `json_type`, `json_valid`, and the
operators `->` and `->>`. Aggregates are `json_group_array`,
`jsonb_group_array`, `json_group_object` and `jsonb_group_object`.

Table-valued `json_each` and `json_tree` expose columns:

```text
key, value, type, atom, id, parent, fullkey, path, json, root
```

The hidden columns start at index 8, so `json` and `root` are bound from table
function arguments.

---

## 6. Encodings

The encoder is in [`../src/internal/encoding.hpp`](../src/internal/encoding.hpp)
and [`../src/internal/encoding.cpp`](../src/internal/encoding.cpp). There are
two independent encodings:

- **key encoding**, which preserves SQL order under unsigned byte comparison;
- **row encoding**, which is compact and self-describing but makes no ordering
  promise.

Malformed records throw `ErrorCode::Internal` with `"malformed record: ..."` or
`"malformed record"`.

### 6.1 Key tags

Ascending key components start with one tag byte:

| Storage class | Tag |
| --- | --- |
| NULL | `0x01` |
| INTEGER and REAL | `0x02` |
| DATETIME | `0x04` |
| TEXT | `0x05` |
| BLOB | `0x06` |

Descending components are bitwise complements of the complete ascending
component, so their tag bytes are in `0xf9..0xfe`.

### 6.2 Numbers

Integers and reals intentionally share tag `0x02`, so `1` and `1.0` encode to
the same key and uniqueness checks match SQLite equality.

A numeric key is:

```text
0x02 | sortable-double-8 | tail-2
```

For a real, `tail` is zero. For an integer, `sortable-double-8` is the largest
double less than or equal to that integer, and `tail` is the integer minus that
double's integer value. The tail is below 2048 because double spacing near
`2^63` is 2048. This `(double, tail)` pair orders integers and reals together
exactly enough for SQLite comparison.

Double order is made byte-sortable by normalizing `-0.0` to `+0.0`, copying the
IEEE-754 bits, inverting all bits for negative values, and setting the sign
bit for non-negative values.

### 6.3 Datetime, text and blob keys

`DATETIME` is tag `0x04` followed by an 8-byte big-endian microsecond count
with the sign bit flipped.

Text and blob keys are terminated byte strings. A zero byte inside the payload
is escaped as `00 ff`; the terminator is `00 00`. `NOCASE` folds ASCII bytes
inside the encoded run. `RTRIM` strips trailing spaces before encoding.

Composite keys concatenate encoded components without a separator because each
component is self-delimiting.

### 6.4 Rowid keys

Tables keyed by rowid use an 8-byte raw key:

```text
big-endian(uint64(rowid) xor 0x8000000000000000)
```

There is no tag. This makes signed rowids sort in numeric order.

### 6.5 Row records

Rows use (format 2, recorded in the catalog as `f`; a store with another row
format is refused):

```text
varint column-count
varint header-bytes      bytes of the type codes that follow
varint type-code ...     one per column
payload ...              one per column, in order, sizes implied by the codes
```

| Type code | Class | Payload |
| --- | --- | --- |
| `0` | NULL | none |
| `1`-`6` | INTEGER | 1, 2, 3, 4, 6 or 8 bytes, little-endian, sign-extended |
| `7` | REAL | raw 8-byte `double` |
| `8`, `9` | INTEGER 0, 1 | none |
| `10` | DATETIME | 8 bytes of microseconds, little-endian |
| even `N >= 12` | BLOB | `(N - 12) / 2` bytes |
| odd `N >= 13` | TEXT | `(N - 13) / 2` bytes |

Knowing where the payloads start, a reader walks the codes and the payloads
together and steps over a column without looking at its bytes, so decoding
only the used columns (`decodeRowInto` with a mask) costs a byte per skipped
column. `decodeRowInto` fills missing trailing columns with NULL. Callers that
know a column was added later may replace those NULLs with constant defaults.
`encodeRowInto` takes a mask of columns to store as NULL whatever they hold
(the rowid alias, virtual generated columns).

### 6.6 Table and index payload layout

For rowid-keyed tables, the table sub-database key is the rowid key and the
payload is the row record.

For rowid tables clustered by another declared primary key, the table key is
the declared primary-key encoding and the payload begins with the hidden rowid
as a zigzag varint, followed by the row record.

For `WITHOUT ROWID` tables, the table key is the declared primary-key encoding
and there is no hidden rowid prefix.

When writing rows, `Writer::writeRow` stores NULL for an `INTEGER PRIMARY KEY`
alias column and for virtual generated columns in the payload. Loading a row
reconstructs the alias from the rowid and recomputes virtual generated columns.

Index entries live in an index sub-database. The key is:

```text
encoded index part 0
encoded index part 1
...
table storage key
```

The value is a bit set, usually empty: bit *k* says component *k* (the index
parts, then -- for a clustered or `WITHOUT ROWID` table -- the parts of the
row key) holds a REAL that is a whole number. Key encoding gives `1` and `1.0`
the same bytes, so without the bit an index-only read (§12.3) would return the
REAL as an INTEGER; `-0.0` and `-2^63` are treated the same way
(`decodeKeyInto` reads `-2^63` back as an INTEGER). An `UPDATE` compares the old
and new entry *and* value before deciding an entry is unchanged (§14.3), and
`PRAGMA integrity_check` compares both. Unique checks probe by the encoded part prefix. If any
unique-index part is NULL, `findConflict` treats it as non-conflicting, matching
SQLite's UNIQUE semantics.

---

## 7. Catalog, schema and store layout

The catalog API is in
[`../src/internal/catalog.hpp`](../src/internal/catalog.hpp) and
[`../src/internal/catalog.cpp`](../src/internal/catalog.cpp). The decoded
schema model is in [`../src/internal/schema.hpp`](../src/internal/schema.hpp)
and [`../src/internal/schema.cpp`](../src/internal/schema.cpp).

### 7.1 Reserved sub-database names

All catalog records live in the sub-database:

```text
sql_catalog
```

Table rows live in:

```text
t#<object-id>
```

Index entries live in:

```text
i#<object-id>
```

Names carry numeric object ids, not SQL object names, so renaming a table or
index is a catalog edit and does not move row data.

### 7.2 Catalog keys

The `sql_catalog` key prefixes are:

| Prefix | Meaning |
| --- | --- |
| `'o' + lower(name)` | table, index or view object record |
| `'g' + lower(name)` | trigger record |
| `'a' + varint(table-id)` | AUTOINCREMENT or clustered-rowid high-water mark |
| `'p' + name` | persistent pragma (`user_version`, `application_id`) |
| `'v'` | schema version |
| `'n'` | next object id |

Object and trigger records use catalog record format `1`:

```text
varint(1)
kind-byte
string name
string table
varint object-id
string sql
```

For object records, `kind` is `'t'`, `'i'` or `'v'`. Trigger records are stored
under prefix `'g'` and use kind `'g'`.

On load, `loadSchema` rejects legacy 0.1 records with prefixes `'t'`, `'i'` or
`'r'` and raises `Internal` with the message that the store was written by
libsql 0.1. There is no automatic upgrade path in 0.2.0.

### 7.3 Schema cache and stamps

Each `StoreHandle` owns a `SchemaCache`:

- mutex;
- shared decoded `Schema`;
- `validAtTxn`;
- `catalogSeen`.

A `Catalog` opens against a libnosql transaction and the cache. It reuses a
cached `Schema` when the catalog version visible at the transaction snapshot
matches. DDL calls `Catalog::edit`, which clones the schema, increments its
version, writes `'v'`, assigns a fresh `Schema::stamp`, and resets table/index
handle caches.

`Schema::settle` sorts tables case-insensitively, sets object positions, and
links each table to its indexes.

### 7.4 Decoded schema objects

A `Table` records:

- name, stored SQL, object id and data sub-database;
- `strict`, `withoutRowid`, `autoincrement`;
- columns, primary-key positions, primary-key descending flags and collations;
- primary-key conflict algorithm;
- `rowidAlias` for ascending `INTEGER PRIMARY KEY`;
- foreign keys and checks;
- index pointers;
- cache position.

Important predicates:

- `hasRowid()` is false only for `WITHOUT ROWID`;
- `keyedByRowid()` is true for rowid tables with no declared key or with an
  `INTEGER PRIMARY KEY` alias;
- `payloadRowid()` is true for rowid tables clustered by another declared key.

`IndexPart::column` is:

- `>= 0` for a table column;
- `-1` for an expression;
- `-2` for the rowid.

`Index::origin` is `0` for `CREATE INDEX`, `1` for a UNIQUE constraint and
`2` for a primary-key index where one is materialized.

### 7.5 Building tables

`buildTable` validates a `CreateTableStmt` and stores expressions as SQL text:

- default expressions;
- generated-column expressions;
- check constraints;
- expression-index parts and partial-index predicates later in `buildIndex`.

`STRICT` columns must have one of the accepted strict type names. An ascending
single-column primary key declared exactly `INTEGER` aliases the rowid unless
the table is `WITHOUT ROWID`. `AUTOINCREMENT` is accepted only on that alias.

Primary-key columns are marked `NOT NULL`, except the rowid alias. This is a
deliberate divergence from SQLite's historical rowid-table NULL primary-key
bug.

`implicitIndexes` builds `sqlite_autoindex_<table>_<n>` entries for UNIQUE
constraints and, where needed, primary-key uniqueness. For a rowid table
clustered by a declared primary key, SQLite's autoindex numbering still counts
the key even though libsql enforces it through clustering rather than a
separate index.

### 7.6 Rowid allocation

`Catalog::nextRowid` follows two rules:

- rowid-keyed tables use one past the largest rowid key in use, and also past
  the AUTOINCREMENT high-water mark when `AUTOINCREMENT` is set;
- clustered rowid tables cannot discover the largest hidden rowid from the key,
  so they use the persisted `'a' + id` sequence as a high-water mark.

`noteRowid` updates the in-memory max for rowid-keyed tables and the persisted
sequence when the table is `AUTOINCREMENT` or `payloadRowid()`. `flush` writes
dirty sequences after statements and before commit.

---

## 8. Lexer

The lexer is in [`../src/internal/lexer.hpp`](../src/internal/lexer.hpp) and
[`../src/internal/lexer.cpp`](../src/internal/lexer.cpp). It removes whitespace
and comments and returns a token vector ending with `Tok::End`.

Token kinds are:

- `Identifier` with a `quoted` flag;
- `Number`, whose `value` is an integer or real;
- `String`;
- `BlobLiteral`;
- `Parameter`;
- `Punct`;
- `End`.

Recognized identifier spellings include bare identifiers, double-quoted
identifiers, backtick identifiers and `[bracketed]` identifiers. Double-quoted
strings are not accepted as string literals.

String and quoted identifier escaping use doubled delimiters. Blob literals are
`X'...'` or `x'...'` and must contain an even number of hex digits. Numeric
literals include decimal integers, reals with exponent syntax and hexadecimal
integer literals up to 16 hex digits. The lexer marks the token
`9223372036854775808` specially so unary minus can produce `INT64_MIN`.

Parameters include:

- `?`;
- `?NNN`;
- `:name`;
- `@name`;
- `$name`, including SQLite's `$a::b(c)` suffix form;
- `#name`.

Punctuation includes `->>` and the two-character operators `<>`, `!=`, `<=`,
`>=`, `==`, `||`, `<<`, `>>` and `->`.

Preparing is kept cheap because it is paid by every one-off `exec`:

- `tokenizeInto` builds tokens in place into a vector the parser borrows from
  a per-thread pool (`borrowTokens`/`returnTokens`; at most four vectors of up
  to 4,096 tokens are kept, larger ones are freed);
- each token carries an `ops` bit set saying which operator family it can
  start or continue (`kOpOr`, `kOpCompare`, `kOpAdd`, ...), so each level of
  the expression grammar returns at once when the next token is not one of its
  operators instead of trying each spelling;
- identifiers carry `reserved` and `joinWord` flags and their operator bits
  computed once by the lexer from one static table of the words that have any
  (an open-addressing table keyed by the folded spelling), and `fold`, their
  first eight characters uppercased and packed into a word; `isKeyword` on a
  string literal compares the length and that word, with the literal's folded
  form a compile-time constant.

`isCompleteStatement` is a token-level `sqlite3_complete` equivalent. It
requires a semicolon outside quotes and treats semicolons inside a
`CREATE TRIGGER ... BEGIN ... END;` body as part of the trigger until `END;`.

---

## 9. Parser and AST

The parser is in [`../src/internal/parser.cpp`](../src/internal/parser.cpp) and
the included grammar files:

- [`../src/internal/parser_select.inc`](../src/internal/parser_select.inc);
- [`../src/internal/parser_expr.inc`](../src/internal/parser_expr.inc);
- [`../src/internal/parser_dml.inc`](../src/internal/parser_dml.inc);
- [`../src/internal/parser_ddl.inc`](../src/internal/parser_ddl.inc).

The AST is declared in [`../src/internal/ast.hpp`](../src/internal/ast.hpp).

### 9.1 Parser limits

The parser enforces:

- `kMaxNesting = 200`;
- `kMaxStatementBytes = 1 << 20`;
- `kMaxTokens = 262144`;
- `kMaxCompound = 500`;
- `kMaxVariable = 32766`;
- expression height `kMaxExprHeight = 2048`.

Expression height is intentionally larger than SQLite's nested-expression
limit so long flat generated predicates can still parse.

### 9.2 Keywords and names

Reserved words and join words are looked up by lowercasing short words into a
fixed buffer and probing an `unordered_set`. The reserved set includes
`nothing`, so `ON CONFLICT DO NOTHING` parses like SQLite.

Quoted identifiers never match keywords. A bare identifier that is not in the
reserved set may be a name even if SQLite treats it as a fallback keyword.

### 9.3 Statements represented by the AST

`StatementBody` is a `std::variant` over:

- `SelectStmt`;
- `InsertStmt`;
- `UpdateStmt`;
- `DeleteStmt`;
- `CreateTableStmt`;
- `CreateIndexStmt`;
- `CreateViewStmt`;
- `CreateTriggerStmt`;
- `DropStmt`;
- `AlterTableStmt`;
- `TransactionStmt`;
- `PragmaStmt`;
- `AttachStmt`;
- `DetachStmt`;
- `AnalyzeStmt`;
- `ReindexStmt`;
- `VacuumStmt`;
- `ExplainStmt`.

The parser accepts `CREATE VIRTUAL TABLE ... USING module` only far enough to
raise `Unsupported` with `"no such module: <module>"`.

### 9.4 SELECT AST

`SelectStmt` has an optional `WITH` clause, one or more `SelectCore`s,
compound operators, top-level `ORDER BY`, `LIMIT`, `OFFSET`, and optional
`JsonOptions` for `FOR JSON`.

`SelectCore` stores `DISTINCT`, result columns, `FROM`, `WHERE`, `GROUP BY`,
`HAVING`, named windows and `VALUES` rows. `FromItem` covers tables,
sub-queries, table-valued functions and parenthesized join groups, including
join type, `NATURAL`, `ON`, `USING`, `INDEXED BY` and `NOT INDEXED`.

### 9.5 DML and DDL AST

`InsertStmt` supports `OR` conflict algorithms, `DEFAULT VALUES`, `INSERT ...
SELECT`, `VALUES`, upsert clauses and `RETURNING`.

`UpdateStmt` supports `UPDATE ... FROM`, row-value assignments, `ORDER BY`,
`LIMIT`, `INDEXED BY`/`NOT INDEXED` and `RETURNING`.

`DeleteStmt` supports `ORDER BY`, `LIMIT`, `INDEXED BY`/`NOT INDEXED` and
`RETURNING`.

All three take a leading `WITH`. For `INSERT ... SELECT` the parser attaches
it to the source query (`attachWith`), where a `WITH` of the inner `SELECT`
shadows the outer one; `UPDATE` and `DELETE` keep it on the statement and pass
it to `planScan`, so their `WHERE`, `FROM` and assignments can name the CTEs.

DDL AST nodes record all column/table constraints, generated columns,
`STRICT`, `WITHOUT ROWID`, partial/expression indexes, views, triggers and
`ALTER TABLE` actions.

---

## 10. Expression evaluation

Expression evaluation is implemented in
[`../src/internal/eval.cpp`](../src/internal/eval.cpp). The planner annotates
`Expr` nodes; evaluation then dispatches by `ExprKind`.

Important behavior:

- `Literal`, `Column`, `Slot`, `Aggregate` and `Window` return existing values.
- Unbound prepared-statement parameters read as NULL; one-off exec checks that
  positional bindings supply every parameter slot.
- `AND` and `OR` implement SQLite three-valued logic.
- `IS`, `IS NOT`, `IS [NOT] DISTINCT FROM`, `ISNULL` and `NOTNULL` are modeled
  by `ExprKind::Is` and are null-safe.
- Row-value comparisons evaluate lexicographically with SQL NULL behavior.
- `IN (list)` and `IN (select)` preserve SQLite's NULL result cases.
- `LIKE`, `GLOB`, `REGEXP` and `MATCH` share `ExprKind::Like`; `LIKE`/`GLOB`
  use built-ins, while `REGEXP`/`MATCH` require corresponding functions.
- `CAST` uses `castTo`.
- `CASE`, `coalesce`, `ifnull`, `iif`, `likely`, `unlikely` and `likelihood`
  are lazy where SQLite requires it.
- `RAISE()` throws `TriggerIgnore` or `RaiseError` and is allowed only in
  trigger scopes.

Comparisons copy an operand only when affinity actually converts it. A
comparison whose operands are each a column, literal or parameter is
classified once (`Expr::fastShape`): from the operands' comparison affinities
it records whether two numbers, and whether two texts under BINARY, would
compare as they are. `holds` and `eval` then compare such values directly
(`directCompare`) without evaluating the operand nodes; anything else --
mixed classes, DATETIME operands, collations -- takes the general path.

TEXT may hold NUL bytes. Functions follow SQLite on where the text "ends":
`length`, `substr`, `hex`, `quote`, `instr` and `trim`'s subject use the full
bytes, while `LIKE`/`GLOB` (operators and functions), `trim`'s character set
and the printf `%s`/`%q`/`%Q`/`%w` conversions stop at the first NUL, as C
strings would. `replace` reads its pattern as a C string too: an empty pattern, or one
starting with NUL, returns the subject as it is (a BLOB comes back as TEXT).

Arithmetic uses SQLite-like numeric operands. Integer arithmetic stays integer
when possible and promotes or produces runtime errors where SQLite does.
Concatenation appends text conversions, while bit operations use integer
conversion.

Sub-queries call `runSubquery`. Uncorrelated sub-queries cache their result
for the current statement execution, keyed by `Ctx::run`.

---

## 11. Planning and name resolution

SELECT planning lives in
[`../src/internal/plan_select.cpp`](../src/internal/plan_select.cpp). Statement
planning for DML and dispatch lives in
[`../src/internal/statement.cpp`](../src/internal/statement.cpp).

### 11.1 Scopes and sources

A `Scope` links to its parent, current `CorePlan`, visible sources, CTEs,
result columns and trigger flag. Name resolution searches:

1. visible sources in the current scope;
2. result aliases when `kAllowAlias` is set;
3. parent scopes for correlated references;
4. CTE definitions;
5. schema tables and table-valued pragma functions where appropriate.

`SourceNode` is the planned source:

- `Table`;
- `Rows` for materialized sub-query, view or CTE rows;
- `Function` for table-valued functions;
- `Recursive` for a recursive CTE reference;
- `Single` for no `FROM`;
- `Pseudo` for `NEW`, `OLD`, `excluded` and write rows.

Table sources record `decodeWidth`, generated virtual expressions,
`INDEXED BY`/`NOT INDEXED`, access path, and join metadata.

### 11.2 Views, schema tables and pragmas

Views are stored as SQL. Planning a view re-parses its stored `CREATE VIEW`
text, plans the select, and materializes it as a `Rows` source.

`sqlite_schema`, `sqlite_master`, `sqlite_temp_schema`,
`sqlite_temp_master` and `sqlite_sequence` are represented as generated
`Rows` sub-query plans. `pragma_*` table-valued functions are also generated
rows: the planner validates the pragma name, resolves up to two arguments, and
adds `arg` and `schema` hidden-style output columns.

### 11.3 Joins and filters

`planFrom` flattens parenthesized joins, attaches `ON`/`USING`/`NATURAL` terms
to the correct source, and builds coalescing links for `USING` and `NATURAL`.
For RIGHT and FULL joins, output of a coalesced column uses a synthetic
`coalesce` expression where needed.

`planFilters` splits top-level `AND` terms. For reorderable joins, all sources
must be `Table` or `Rows`, all joins after the first must be inner joins, and
there must be no RIGHT/FULL join. It then greedily picks the next source by:

1. `accessRank`;
2. number of terms completed by placing the source;
3. written order as a tie-breaker.

For non-reorderable plans, filters are assigned to the first source level where
all referenced sources are ready, except right/full joins may require final
filters.

### 11.4 Source caching

`markCacheable` marks inner table sources that do not depend on earlier
sources, outer scopes, volatile functions, correlated sub-queries or
`RAISE()`. During execution the source is read once and replayed from a
`RowStore` if it has no more than `kSourceCacheRows = 65536` rows. Larger
sources fall back to rescanning.

### 11.5 Output names

Result names match SQLite's rules implemented by `outputName`:

- plain column references use the declared column name for top-level results;
- expression columns use the source text span;
- `VALUES` columns are `column1`, `column2`, ...
- sub-query column names use the sub-query-visible form and duplicate names
  get `:1`, `:2`, ... where CREATE TABLE AS SELECT needs it.

`SelectPlan::qualified` also records `alias.column` for plain column results
so `Row::at("t.c")` remains supported.

---

## 12. Scanning and access paths

Access planning and source scanning are in
[`../src/internal/access.cpp`](../src/internal/access.cpp).

### 12.1 Access path kinds

`AccessPath::Kind` is:

- `Scan`, a full table scan;
- `Rowid`, equality or range over a rowid-keyed table;
- `Key`, leading declared-key columns of a clustered table;
- `Index`, an index sub-database.

`planAccess` considers comparisons, `IN` lists and `BETWEEN` terms whose value
side is ready. It tracks affinity and collation per usable term. Candidate key
parts are the rowid, the declared primary key, or an index's parts. The best
candidate prefers more pinned equality parts, then range use, then unique full
matches. Partial indexes are written and enforced but are not used for reads
in this implementation.

`INDEXED BY` forces the named index. If no plan can use it, planning raises
`InvalidArgument` with `"no query solution"`. `NOT INDEXED` clears available
index terms for that source.

### 12.2 Runtime scanning

`openSource` initializes scan state:

- `Single` and `Pseudo` need no cursor;
- `Rows` runs its sub-query and then iterates the materialized `RowStore`;
- `Function` opens a table-function cursor and binds hidden arguments;
- `Table` opens either the table data DB or selected index DB.

For equality prefixes and range bounds, `encodePart` applies comparison
affinity and key encoding. Non-integer rowid equality probes become no-match.
Non-integer rowid range bounds are rounded or treated as lying outside integer
rowids as appropriate.

Index scans decode the table storage key by skipping index key parts, then
fetch the row payload from the table DB. An index entry whose row is missing is
skipped defensively.

`loadRow` decodes the columns the statement reads -- `SourceNode::used`, set
by `useColumn` wherever name resolution, `*` expansion or `USING` binds a
column; writes and virtual generated columns call `useAllColumns` -- and steps
over the others without building values (`decodeRowInto` with a mask). It stops
at `decodeWidth`, reconstructs rowid values (the `INTEGER PRIMARY KEY` alias is
never decoded from the payload; it is the rowid), fills constant defaults for
missing trailing columns, and computes virtual generated columns in two passes.
It copies the row key into `rowKey` only for RIGHT and FULL joins.

Scans step the libnosql cursor with `next(key, value)`, which moves and returns
the entry in one call, and keep the entry in `entryKey`/`entryValue`. A source
keeps its cursor from one open to the next while it still reads the same
database in the same transaction (`nosql::Cursor::boundTo`), and index reads
fetch rows through a second kept cursor (`rowCursor`, `seekExact(key, value)`):
libnosql answers a seek that falls inside the cursor's current leaf without
descending from the root, and stops trying for a while on a cursor whose seeks
keep missing.

**Two-phase rows.** At the first open in `nest`, `decideSplit` gives a table
source two column masks when its level has terms that can reject rows: the
columns those terms read (and, for the innermost source of a plain core under
`ORDER BY ... LIMIT`, the columns its sort-key outputs read) are decoded by
`loadRow`; the rest of `used` waits in `restRecord` until `finishRow`. `nest`
calls `finishRow` once the row has passed its filters and before descending;
`project` calls it after a top-N sink has said the row can place
(`RowSink::rejectKeys`/`rejects`). The invariant is that a row is complete
whenever anything beyond its own level's filters reads it: the cache fill,
the right-join remainder, emission and DML all see whole rows. Equality terms
the access path pins count as columns to decode but not as a reason to split.
Terms with sub-queries, generated columns and index-only reads disable the
split.

### 12.3 Exact windows

The window a source opens is exact for some conjuncts, and those are recorded
in `SourceNode::enforced` for this open; `nest` builds the level's filter list
without them (`liveFilters`, `liveOn`) and never evaluates them per row. The
NULL row of an outer join is still checked against every filter.

- Equality on every pinned key or index part is exact: `usable` already
  required the comparison to use the part's collation and not to convert the
  column, and key equality is SQL equality for the encoded value.
- Rowid ranges are made exact by `rowidBound`: `>`/`>=`/`<`/`<=` against an
  INTEGER or REAL become the smallest and largest admissible integer
  (clamped to the int64 range, or an empty window). A bound that is not a
  number leaves that side open and re-checked.
- Ranges on an ascending BINARY key or index part are exact: a strict low
  bound seeks to the key successor of the value, an inclusive high bound
  ends at that successor, and a window with only a high bound starts past the
  part's NULL entries. DESC and collated parts keep the old, wider window and
  re-check every row.

A BETWEEN supplies both bounds; it is enforced only when the window took both
from it (`enforceRange`), since a stricter separate bound may have replaced one
side.

### 12.4 Index-only reads

`decideCovering` runs at a source's first open, when every reference to it --
including correlated sub-queries planned after the access path -- has been
resolved. An index covers the source when every used column is an index part
with BINARY collation (folded NOCASE/RTRIM text cannot be read back) or a part
of the row key, and the rowid, if needed, is in the key (rowid-keyed tables).
Then the source reads only the index (`coverState` 2, `loadEntry` decoding the
entry's parts into the columns `coverColumns` names and applying the REAL bits
of the entry's value), and a plain `Scan` switches to the covering index with
the fewest parts. `EXPLAIN QUERY PLAN` calls `decideCovering` itself and says
`USING COVERING INDEX`.

`SELECT DISTINCT` over one table with no filters and only plain column outputs
sets `distinctColumns`; when a covering scan's leading parts are exactly those
columns, `advanceEntry` seeks to the successor of each entry's distinct prefix
instead of stepping (`skipComponents`).

A core that is `count(*)` over one table and nothing else (no filters, no
grouping, no bare column) is answered from the table's entry count
(`CorePlan::countRows`, libnosql's O(1) `Db::count`). With a `WHERE` and
nothing else reading the row (`CorePlan::countWindow`), `countByWindow` opens
the source and, when its window enforces every term (no IN probes, no point
lookup, no fallback scan), asks libnosql for the window's entry count
(`Db::countRange(lo, hi)`), which descends by the per-subtree counts every
branch page keeps: O(log n).

The same counts serve `OFFSET`: for a plain one-table core under `LIMIT ...
OFFSET`, `runSelect` hands the core the `LimitSink`'s remaining offset, and
when the source's window decides every row, `skipEntries` moves the cursor
past that many entries (`Cursor::skip`) and counts them off the sink's offset,
instead of producing rows for the sink to drop.

---

## 13. SELECT execution

SELECT execution is in
[`../src/internal/run_select.cpp`](../src/internal/run_select.cpp). It is a
pipeline:

1. nested-loop source iteration;
2. projection or aggregate accumulation;
3. window evaluation where present;
4. DISTINCT;
5. compound operator processing;
6. ORDER BY;
7. OFFSET/LIMIT;
8. optional `FOR JSON` presentation.

### 13.1 Joins

`nest` recursively opens each source and tests source `ON` terms and level
filters. LEFT and FULL joins emit one NULL-padded row when no right-side row
matched. RIGHT and FULL joins track matched right-side row keys, then
`rightRemainder` emits unmatched right-side rows with sources to the left set
to NULL.

Every recursion checks `Database::interrupt()` through the session's atomic
flag and raises `ErrorCode::Interrupted` when set.

An inner table source whose rows do not depend on the sources before it is
read once per execution into `cache` and replayed (`markCacheable`). Its
equality terms `inner = outer` -- inner side reading only this source, outer
side only earlier ones, both deterministic and free of sub-queries and
DATETIME operands -- become a hash key (`hashInner`/`hashOuter`): rows are
bucketed in a `FlatKeySet` as they are cached, keyed by the inner sides after
the comparison's affinity and under its collation's key encoding, and each
outer row visits only the bucket of its own key, in cache order. Every join
term is still evaluated on each visited row, so the hash can only skip rows
that cannot match. This is SQLite's automatic index.

### 13.2 Projection and DISTINCT

`project` evaluates visible columns plus hidden sort keys. DISTINCT builds a
key from visible values using the result expression's collation and stores it
in a `FlatKeySet`.

An output that is a bare column of the innermost source, read by no other
output and with no sub-query among the outputs (`decideSwaps`), takes its
value by `Value::swap` when it is text or a blob: the slot is reloaded for
the next row anyway, and the two buffers trade back and forth, so neither the
bytes nor an allocation are paid per row.

`FlatKeySet` ([`../src/internal/flat_hash.hpp`](../src/internal/flat_hash.hpp))
is the executor's set of encoded keys: keys packed into one byte buffer, an
open-addressing table of 64-bit slots holding a member index and the high half
of its hash, and a fixed-load hash for short keys. Members are numbered in
insertion order, so it is also a key-to-index map. Clearing keeps the memory;
`releaseAbove(scratchRetain)` frees it after an unusually large run.

### 13.3 Aggregation

Each group is a `Group` containing:

- the encoded group key;
- a snapshot of source/aggregate/window frame slots;
- one `AggSlot` per aggregate.

`GroupTable` keeps reusable group objects and a `FlatKeySet` index whose
member numbers are the groups' positions (the keys live in the set). It
resets `used` and clears the index between runs. If a run was much smaller
than a previous one, or the index outgrew `scratchRetain`, it drops excess
group objects and frees the index.

Aggregate `FILTER` is applied before stepping. Aggregate `DISTINCT` uses an
encoded argument key. Aggregate `ORDER BY` stores `OrderedStep` records, sorts
them at finish time and then steps the aggregate state in order.

The single `min()`/`max()` bare-column rule is implemented by updating the
group sample when the extreme aggregate's value changes.

### 13.4 Windows

`WindowRunner` materializes the core rows, groups window calls by compatible
window specs, sorts partitions as needed and writes results back into window
slots before projection. It supports:

- named windows and inherited base specs;
- `PARTITION BY`;
- `ORDER BY`;
- `ROWS`, `RANGE` and `GROUPS` frames;
- `UNBOUNDED`, `CURRENT ROW`, `<expr> PRECEDING` and `<expr> FOLLOWING`;
- `EXCLUDE NO OTHERS`, `CURRENT ROW`, `GROUP` and `TIES`;
- aggregate windows and built-in window functions.

When an aggregate is invertible and the frame can slide without an exclusion,
the runner uses `inverse`; otherwise it recomputes the frame state.

### 13.5 Compound SELECT, sorting and LIMIT

Compound operators are evaluated left to right:

- `UNION ALL` appends rows;
- `UNION`, `INTERSECT` and `EXCEPT` use encoded row keys in ordered maps/sets
  and rebuild rows in key order.

`sortRows` builds an index vector over a `RowStore`. It uses `partial_sort`
when a finite `LIMIT + OFFSET` needs only a prefix; otherwise it sorts the
whole index. Null ordering follows SQLite's default and explicit
`NULLS FIRST`/`NULLS LAST`; ties keep arrival order, so every sort is stable.

An `ORDER BY` over one core collects through `SortCollector`:

- With a finite `LIMIT + OFFSET` = *k*, it prunes to the best *k* rows
  whenever it holds max(2*k*, 1024), and after the first prune rejects a row
  no better than the *k*-th with one comparison (`cannotPlace`; a tie loses
  to the earlier arrival). Memory is bounded by the limit.
- Without one, it estimates the bytes it holds; past the connection's
  `sortMemory` (64 MiB, `Options::sortMemoryLimit`) it sorts what it has and
  appends it as a run to a `std::tmpfile()` (deleted when closed): per row a
  varint length, then per value its subtype byte and `putValue` encoding, so
  JSON subtypes survive. `emit` merges the runs and the in-memory rest with a
  heap, ties going to the earlier run. `PRAGMA temp_store = MEMORY` disables
  spilling.

Afterwards the plan's `sorted` store gives its cells back if they hold more
than `scratchRetain` bytes (`releaseAbove`). Compound selects, grouping, `DISTINCT` and window
partitions stay in memory.

Recursive CTEs use a queue:

- FIFO by default;
- ordered by recursive-CTE `ORDER BY` when present;
- `UNION` tracks a `seen` set, `UNION ALL` does not;
- recursive `LIMIT` and `OFFSET` apply to rows taken from the queue.

---

## 14. DML, constraints, triggers and foreign keys

The shared write engine is `Writer` in
[`../src/internal/dml.hpp`](../src/internal/dml.hpp) and
[`../src/internal/dml.cpp`](../src/internal/dml.cpp). DML plans are in
[`../src/internal/statement.cpp`](../src/internal/statement.cpp).

### 14.1 Write planning

`Writer::plan` allocates new, old and excluded row slots for a table. It parses
and resolves:

- non-constant defaults;
- generated columns, for both new and old rows;
- check constraints;
- expression-index parts;
- partial-index predicates.

It gathers triggers for the table from every open store's catalog. If
`PRAGMA foreign_keys` is on, it also plans parent and child foreign-key
relationships, including indexes used for lookup and child writers used by
actions.

### 14.2 INSERT order

The insert path follows the order documented in the source:

1. defaults and affinity;
2. BEFORE triggers;
3. rowid assignment;
4. generated columns;
5. NOT NULL and CHECK;
6. primary-key and unique checks, including upsert or conflict handling;
7. row and index writes;
8. foreign-key checks/actions;
9. AFTER triggers.

`initRow` fills constant and expression defaults. `applyColumnRules` applies
strict coercion or ordinary affinity and turns NaN reals into NULL. If a rowid
table has no rowid value, `Catalog::nextRowid` assigns one.

Primary-key conflict checks may be deferred to the final `InsertUnique` table
put when the only possible result is an error. This avoids a redundant lookup
but still preserves error precedence against UNIQUE indexes.

### 14.3 UPDATE and DELETE

UPDATE and DELETE first build a SELECT over the target table, including
`UPDATE ... FROM`, ORDER BY and LIMIT when present. The result contains the old
target row and, for UPDATE, computed new SET values. Matched rows are fully
materialized before writes begin, so reads from the target see the pre-statement
state.

For each index, `Writer::update` computes the entry the old row had and the
one the new row needs (partial-index predicate and expression parts included).
When they are identical and the row's storage key did not move, the entry is
left in place and a unique index skips its conflict probe, so updating columns
no index covers writes only the row.

`UPDATE ... FROM` tracks storage keys and updates each target row at most once.
Triggers or cascades that remove a later matched row are handled by reloading
and skipping if the row no longer exists.

### 14.4 Constraints and conflict algorithms

`Conflict` values are `Default`, `Rollback`, `Abort`, `Fail`, `Ignore` and
`Replace`. `effective(statement, constraint)` chooses the statement-level
algorithm when present, otherwise the constraint's algorithm, otherwise ABORT.

`conflictError` throws:

- `FailError` for FAIL, preserving previous row changes in the statement;
- `RollbackError` for ROLLBACK, dooming the whole transaction;
- ordinary `Error` for ABORT and other constraint failures.

`OR IGNORE` paths return `false` from the writer to skip the row. `OR REPLACE`
removes conflicting rows, firing recursive triggers only when
`recursive_triggers` allows them, then retries.

### 14.5 Upsert

Upsert planning maps conflict targets to either:

- the primary key;
- a matching UNIQUE index;
- any uniqueness constraint when no target is given.

`excluded` is a pseudo source that is qualified-only. DO UPDATE evaluates SET
expressions against the existing row and `excluded`; DO NOTHING returns
without a change.

### 14.6 Triggers

Triggers are planned lazily per recursion depth into `TriggerInstance`
objects. Each body statement gets a `Program` with `NEW` and `OLD` pseudo
sources at fixed slots. `WHEN` is planned as a separate expression program.

`Writer::fire` filters by trigger time, event and `UPDATE OF`. Without
`PRAGMA recursive_triggers`, a trigger body is not re-entered while it is
already active. With recursion enabled, depth is capped at 1000.

`RAISE(IGNORE)` throws `TriggerIgnore` and skips the current row. Other
`RAISE` forms throw `RaiseError` with `Constraint::Trigger`.

Views can be modified only through `INSTEAD OF` triggers. The planner builds a
table-shaped stand-in for view columns so the same writer machinery can carry
`NEW` and `OLD`.

### 14.7 Foreign keys

Foreign keys are active only when `ConnectionState::foreignKeys` is true.
`PRAGMA foreign_keys` is a no-op inside an explicit transaction, as in SQLite.

Parent resolution accepts either the parent's primary key or a matching UNIQUE
index with no partial predicate. Missing or invalid parent definitions raise
runtime foreign-key mismatch errors.

Child checks skip rows with any NULL child key. Violations increment either the
immediate or deferred counter. Immediate counters are checked at statement end;
deferred counters are checked at transaction commit.

Parent actions support `NO ACTION`, `RESTRICT`, `CASCADE`, `SET NULL` and
`SET DEFAULT`. Cascades and set actions call back into `Writer::remove` or
`Writer::update`, so triggers, constraints and further foreign-key actions run
through the same code path.

---

## 15. DDL, schema tables, pragmas and maintenance statements

DDL plans are in [`../src/internal/ddl.cpp`](../src/internal/ddl.cpp). Schema
tables and pragmas are in [`../src/internal/pragma.cpp`](../src/internal/pragma.cpp).

### 15.1 CREATE

`CREATE TABLE`:

- checks the object namespace shared by tables, indexes and views;
- rejects reserved `sqlite_*` names except `sqlite_stat1`;
- stores normalized `CREATE TABLE ...` SQL;
- creates implicit indexes;
- validates defaults, checks and generated expressions by planning a scratch
  writer.

`CREATE TABLE ... AS SELECT` plans the SELECT, builds a table with result names
and declaration types, then inserts materialized result rows through ordinary
INSERT code.

`CREATE INDEX` validates the target table, rejects views and reserved system
tables, stores normalized SQL, adds an index object and backfills entries.

`CREATE VIEW` stores normalized SQL and plans the view immediately to validate
it and its optional column list.

`CREATE TRIGGER` validates table/view rules: `INSTEAD OF` only on views,
`BEFORE`/`AFTER` only on tables, and no triggers on system tables.

### 15.2 DROP

Unqualified DROP searches temp first, then main, then attached stores. Dropping
a table also drops its indexes and triggers. With foreign keys enabled, if the
table is a parent, DDL first runs `DELETE FROM schema.table` so child actions
fire before the table disappears.

Implicit indexes associated with UNIQUE or PRIMARY KEY constraints cannot be
dropped directly.

Dropping a view also drops triggers on that view.

### 15.3 ALTER TABLE

ALTER TABLE works by rewriting stored CREATE SQL and decoding it again:

- `RENAME TO` rewrites the table, explicit indexes, autoindex names, foreign
  keys, triggers and views that mention the table, then validates views.
- `RENAME COLUMN` rewrites the table definition, explicit indexes, triggers,
  child foreign-key references and validates views. The new column name is
  quoted where needed by `quoteIdentifier`.
- `ADD COLUMN` inserts the column definition before the closing parenthesis and
  enforces SQLite restrictions: no primary key, no unique, no stored generated
  column, only constant defaults, no NOT NULL with NULL default, and no
  REFERENCES column with a non-NULL default when foreign keys are on.
- `DROP COLUMN` rejects dropping the last column, primary-key columns, indexed
  columns, foreign-key columns, columns referenced by generated columns or
  checks, then rewrites every row payload without the column.

`legacy_alter_table` is stored in connection state but the rewrite code always
updates dependent SQL as implemented.

### 15.4 ANALYZE and REINDEX

`ANALYZE` creates `sqlite_stat1(tbl,idx,stat)` when needed. It counts rows for
tables and scans index entries to compute average rows per distinct prefix.
Rows for analyzed tables are deleted from `sqlite_stat1` and replaced.

`REINDEX` clears matching index sub-databases and backfills them through the
writer. Its target can be empty, an index, a table, or a collation name.

### 15.5 Schema tables

`sqlite_schema` and `sqlite_master` are generated rows with columns:

```text
type, name, tbl_name, rootpage, sql
```

Objects are listed in creation-id order. `rootpage` is the table or index
object id for table/index rows and `0` for views and triggers. `sqlite_temp_*`
variants read the temp store. `sqlite_sequence` exists only when the selected
store has at least one AUTOINCREMENT table; it is generated from catalog
sequence records.

### 15.6 Pragmas

The pragma implementation recognizes the names in `kPragmas`. Unknown pragmas
are ignored, as SQLite does, except unknown `pragma_*` table-valued functions
raise `NoSuchTable`.

Connection flags include:

- `foreign_keys`;
- `recursive_triggers`;
- `case_sensitive_like`;
- `defer_foreign_keys`;
- `ignore_check_constraints`;
- `query_only`;
- `reverse_unordered_selects`;
- `legacy_alter_table`.

Persistent pragmas are `user_version` and `application_id`, stored in catalog
`'p'` records as signed 32-bit values.

Store-reporting pragmas derive values from libnosql where possible:
`page_size`, `page_count`, `freelist_count`, `max_page_count`, `mmap_size` and
`data_version`. `journal_mode` reports `"wal"`, `locking_mode` reports
`"normal"`, and `encoding` reports `"UTF-8"`.

`function_list`, `module_list`, `pragma_list`, `collation_list`,
`database_list`, `table_list`, `table_info`, `table_xinfo`, `index_list`,
`index_info`, `index_xinfo`, `foreign_key_list`, `foreign_key_check`,
`integrity_check` and `quick_check` produce rows. `busy_timeout` bounds the
wait for the writer slot (§16.3). `temp_store` reads and sets whether an
`ORDER BY` may spill (0/`DEFAULT` and 1/`FILE` allow it, 2/`MEMORY` does not).

`integrity_check` walks every table and, for each of its indexes, recomputes
the entry each row implies (through `Writer::verifyEntries`, the same code that
maintains entries) and looks it up, reporting "row N missing from index X";
it then compares the index's entry count with the number of rows that should
have one ("wrong # of entries in index X"). It stops adding messages at 100.
`quick_check` only walks the trees.

### 15.7 EXPLAIN, VACUUM, ATTACH and DETACH

[`../src/internal/explain.cpp`](../src/internal/explain.cpp) implements both
`EXPLAIN QUERY PLAN` and `EXPLAIN`. There is no virtual machine bytecode, so
`EXPLAIN` returns a simple `Init`, one `Plan` row per plan line, and `Halt`.
`EXPLAIN QUERY PLAN` returns `id`, `parent`, `notused`, `detail`.

`ATTACH` and `DETACH` are connection-level statements in
[`../src/database.cpp`](../src/database.cpp), not `ExecPlan`s. They cannot run
inside an active SQL transaction. `VACUUM INTO` copies all sub-databases to a
new libnosql file. In-place `VACUUM` is a no-op because libnosql already
reuses free pages and the store may be open in other handles.

---

## 16. Transactions, savepoints and connection state

Connection and transaction code is in
[`../src/database.cpp`](../src/database.cpp).

### 16.1 TxnImpl

`TxnImpl` is the internal transaction object. It is pooled per `DatabaseImpl`
with `kPoolSize = 4`. It carries:

- root libnosql transactions per store;
- reusable `Catalog` objects;
- nested `Level`s for savepoints and statement atomicity;
- a `Session`;
- a per-transaction `FuncContext`;
- a per-transaction `BatchCache`;
- flags for read-only, SQL transaction, savepoint-opened transaction,
  started, writer and doomed.

`park` releases roots and nested levels, drops the `db` pointer to avoid a
cycle, and returns the object to the pool when it is uniquely owned.

### 16.2 Statement atomicity

When a write statement runs inside a larger transaction, `runOne` installs
`Session::beginAtomic`. DML calls `ensureAtomic` before the first write only
when the statement can fail after partial work. Single safe writes skip the
nested transaction when they have at most one row, no triggers, no foreign
keys, no view writes, no REPLACE handling and no REPLACE conflict policy on
unique constraints.

DDL and pragma writes open the nested level immediately when guarded.

On ordinary `Error`, a nested statement level aborts. On `FailError`, the level
commits and the statement stops. On `RollbackError`, the level aborts and the
transaction is marked doomed. Non-SQL exceptions mark the transaction doomed
when a guarded statement did not have its own nested level.

### 16.3 SQL transaction control

`BEGIN DEFERRED` creates a `TxnImpl` but may not start libnosql roots until the
first statement. `BEGIN IMMEDIATE` and `BEGIN EXCLUSIVE` start a writer
immediately. `BEGIN EXCLUSIVE` does not block readers because libnosql readers
are MVCC snapshots.

Every writer start goes through `writerOf`, which calls libnosql's
`writeTxn(timeout)` with the connection's `busyTimeout`: 0 waits for the
writer slot indefinitely, a positive value waits that many milliseconds on
libnosql's timed writer mutex and then raises `Busy` (`"database is locked"`).

`SAVEPOINT` pushes a nested level. If there is no active SQL transaction,
SAVEPOINT opens one and marks it `bySavepoint`. `RELEASE` commits nested
levels through the named savepoint; releasing the outer savepoint commits and
finishes a savepoint-opened transaction. `ROLLBACK TO` aborts through the named
savepoint and recreates that savepoint level.

`upgrade` turns a deferred read transaction into a writer if the main store's
writer generation still follows the snapshot. Otherwise it raises `Busy`.

### 16.4 Connection state

`ConnectionState` in
[`../src/internal/exec.hpp`](../src/internal/exec.hpp) stores pragma flags,
`cacheSize`, the atomics `busyTimeout`, `tempStore`, `sortMemory` and
`scratchRetain` (read by statements on other threads), and atomic counters:

- `changes`;
- `totalChanges`;
- `lastInsertRowid`;
- `runCounter`.

`Database::interrupt` sets an atomic flag. SELECT loops read it and raise
`Interrupted`.

---

## 17. `FOR JSON` and the JSON renderer

The public JSON API is declared in
[`../include/sql/json.hpp`](../include/sql/json.hpp) and implemented in
[`../src/json.cpp`](../src/json.cpp). SQL `FOR JSON` is parsed in
[`../src/internal/parser_select.inc`](../src/internal/parser_select.inc),
planned in [`../src/internal/plan_select.cpp`](../src/internal/plan_select.cpp)
and rendered in [`../src/internal/run_select.cpp`](../src/internal/run_select.cpp).

### 17.1 SQL syntax

The parser accepts:

```sql
FOR JSON {AUTO | PATH}
         [, ROOT[('name')]]
         [, INCLUDE_NULL_VALUES]
         [, WITHOUT_ARRAY_WRAPPER]
```

`ROOT` and `WITHOUT_ARRAY_WRAPPER` together raise `InvalidArgument`.

`presentAsJson` changes the select plan's public shape to one column named
`json`. The original row shape is saved in `innerColumns`, `innerNames` and
`innerQualified`. At runtime, `runSelect` collects inner rows into a temporary
`Result`, renders them with a thread-local `JsonWriter`, wraps the text in a
`Value`, sets subtype `'J'`, and emits one row.

### 17.2 Property paths

For `PATH`, column aliases are property paths. A dot creates a nested object.
Each row becomes one object.

For `AUTO`, planning computes JSON paths from the first SELECT core and the
FROM clause as written, not the join order after reordering. Source aliases
form prefixes. For explicit expressions, the deepest source referenced by the
expression decides the nesting prefix. Stars expand according to visible source
columns and `USING` coalescing rules.

### 17.3 Renderer behavior

`JsonOptions` stores mode, root name, null inclusion and array-wrapper options.
`JsonWriter` is move-only, reusable and not thread-safe. It caches the derived
shape plan and its output buffer. It can render to its own buffer, append to a
caller buffer, measure output size, and compact retained memory.

Text values with subtype `'J'` are embedded as JSON. Other text is quoted and
escaped. `DATETIME` renders as ISO-8601 UTC. BLOB values render as base64.

`AUTO` collapses consecutive rows that agree on enclosing object columns. It
does not sort; callers must `ORDER BY` the outer keys if they want predictable
nested grouping.

---

## 18. Public C++ API

Public headers live under [`../include/sql/`](../include/sql/).

### 18.1 Database

`Database::Options` exposes:

- `maxSize`, default 64 GiB;
- `readOnly`;
- `createIfMissing`;
- `durable`;
- `cacheReadChecksums`, default on: a read snapshot trusts a page's checksum
  once verified until the page is replaced (writers always re-verify);
- `busyTimeout`, the initial `PRAGMA busy_timeout`;
- `sortMemoryLimit`, the bytes an `ORDER BY` holds before spilling (§13.5);
- `memoryProfile` (`Compact`, `Balanced`, `Throughput`) setting
  `statementCacheSize`, `scratchRetain`, `sortMemoryLimit` and
  `arenaBlockSize` together; each can also be set alone, and an explicit
  setting wins (README, "Memory").

`Database` supports:

- `exec` with positional or named parameters;
- `prepare`;
- `begin` and `beginRead`;
- `openReader`;
- `changes`, `totalChanges`, `lastInsertRowid`, `inTransaction`;
- `interrupt`;
- schema inspection (`tables`, `table`, `indexes`);
- access to the underlying `nosql::Env`;
- `close`.

`version()` returns `"0.2.0"`. `complete(text)` calls the lexer-level complete
statement check.

### 18.2 Transactions

`Transaction` is move-only and rolls back when destroyed without commit. It can
execute SQL with positional or named parameters, commit, roll back, and expose
the underlying main-store `nosql::Txn`. If nested statement/savepoint
transactions are active, `Transaction::txn()` returns the innermost valid main
nested transaction.

### 18.3 Statements and results

`Statement` is move-only and not safe to execute concurrently. It exposes:

- original SQL;
- parameter count, names and indexes;
- lazy `columnNames`;
- `writes`;
- bind by index, name, whole vector or named list;
- `clear`;
- `exec` standalone or inside a `Transaction`;
- `each(fn)` / `each(txn, fn)`, which streams rows to a callback as a
  `RowView` -- valid only during the call -- without building a `Result`.

A statement is not re-entrant: running it from its own `each` callback raises
`InvalidArgument` (`"statement is already running"`).

`Result` owns rows and column names. A `Row` holds its values in `Values`, a
small vector with four values inline; a wider row's values live in cell chunks
its `Result` owns (64 values, doubling to 4,096), so a wide result costs a few
allocations rather than one per row. A `Row` copied out of a `Result`, and a
copied `Result`, own their values; the `Result`'s moves keep the chunks with
the rows that point into them. Rows are built in place by
`ResultBuilder`, sinks move values out of the executor's row (`RowSink::take`),
and a prepared SELECT reserves room for the row count of its previous run (up
to 65,536). `Row::at(name)` first searches reported
column names, then qualified `alias.column` names. DML results carry
`changes()` and `lastInsertId()`.

### 18.4 Schema inspection structures

`ColumnInfo` reports name, strict storage type (or `Type::Null` for ANY and
ordinary tables), declared type, affinity, collation, primary-key/not-null/
unique/default/generated flags and default details.

`TableInfo` reports table name, columns, primary-key column positions,
`strict`, `withoutRowid` and stored SQL.

`IndexInfo` reports name, table, column names or expression SQL, uniqueness,
partial-index flag and stored SQL.

---

## 19. Errors

Errors are declared in [`../include/sql/error.hpp`](../include/sql/error.hpp)
and stringified in [`../src/value.cpp`](../src/value.cpp). Every SQL-layer
failure throws `sql::Error`, except underlying libnosql failures, which keep
arriving as libnosql errors.

`ErrorCode` values are:

```text
Ok, SyntaxError, Unsupported, InvalidArgument, NoSuchTable, TableExists,
NoSuchColumn, AmbiguousColumn, NoSuchIndex, IndexExists, TypeMismatch,
ConstraintViolation, BadTransaction, ReadOnly, Internal, Runtime, Busy,
Interrupted
```

Constraint failures also carry a `Constraint`:

```text
None, PrimaryKey, Unique, NotNull, Check, ForeignKey, Trigger, Datatype
```

Planning errors generally use `InvalidArgument`. Runtime SQL errors use
`Runtime`. Read-only state uses `ReadOnly`. Constraint errors use
`ConstraintViolation` with the extended constraint kind. Unknown virtual table
modules and unsupported schema-version writes use `Unsupported`.

---

## 20. The `sql` command-line tool

The command-line front end is
[`../tools/sql_tool.cpp`](../tools/sql_tool.cpp). It supports:

```text
sql <database> [options]
```

Options:

- `-c SQL`, repeatable;
- `-f FILE`, repeatable;
- `-i` to continue reading stdin after `-c`/`-f`;
- `--mode table|csv|list`;
- `--no-header`;
- `--readonly`;
- `-h`/`--help`.

Interactive input uses `sql::complete`, so trigger bodies do not prematurely
run on inner semicolons.

Dot commands include:

- `.tables`;
- `.schema [TABLE]`, printing stored `sqlite_schema.sql`;
- `.dump [TABLE]`, emitting schema and data in one transaction;
- `.indexes [TABLE]`;
- `.mode`;
- `.headers`;
- `.help`;
- `.quit` / `.exit`.

Exit status is `0` for success, `1` for usage errors and `2` for statement or
store errors.

---

## 21. Build system and source layout

[`../CMakeLists.txt`](../CMakeLists.txt) defines project version `0.2.0` and
requires a 64-bit C++17 target. Options include:

- `SQL_BUILD_TESTS` (default on);
- `SQL_BUILD_EXAMPLES` (default on);
- `SQL_BUILD_TOOLS` (default on);
- `SQL_BUILD_BENCHMARKS` (default off);
- `SQL_BUILD_CONFORMANCE` (default off);
- `SQL_ENABLE_ASAN` (default off);
- `SQL_BUILD_PYTHON` (default off and marked experimental).

libsql prefers an installed `nosql::nosql` and otherwise adds the sibling
libnosql checkout from `SQL_NOSQL_DIR`, defaulting to `../libnosql`.

The `sql` library sources are:

- public implementation: [`../src/value.cpp`](../src/value.cpp),
  [`../src/database.cpp`](../src/database.cpp),
  [`../src/json.cpp`](../src/json.cpp);
- internals: access, catalog, numeric formatting, DDL, DML, encoding,
  evaluation, explain, function modules, lexer, parser, planning, pragma,
  select runner, schema, statement and types.

Public installation exports the `sql::sql` target and installs
[`../include/sql/`](../include/sql/).

---

## 22. Testing and conformance

The conformance harness is documented in
[`../tests/conformance/README.md`](../tests/conformance/README.md).

### 22.1 Unit and integration tests

The regular CMake test tree covers the public API, types, DDL, queries,
preparation and planning, joins, grouping, composite keys, JSON, SQLite
behavior cases, function modules and the Python binding when enabled.

### 22.2 SQLite conformance harness

When `SQL_BUILD_CONFORMANCE=ON`, CMake downloads the SQLite 3.45.1 amalgamation
into the build tree and links it only into test executables. The harness
provides:

- `slt_runner`, which can run sqllogictest files against either SQLite or
  libsql;
- `difftest`, which compares scripts statement by statement;
- a random SQL generator and fuzzer;
- targeted scripts under
  [`../tests/conformance/scripts/`](../tests/conformance/scripts/);
- a skip list for deliberate divergences.

Useful `slt_runner` options include `--verbose`, `--max-failures`,
`--summary-json` and `--skip-list`. `difftest --fuzz` writes reproducers for
mismatches and compares unordered query results as row multisets unless a
top-level `ORDER BY` exists.

The migration note records the completion evidence: the full sqllogictest
corpus, 5,939,546 records, passed; targeted trigger/foreign-key/view/
transaction/window scripts matched SQLite; and the differential fuzzer was run
against SQLite 3.45.1.

---

## 23. Long-run behavior and memory reuse

Several data structures are designed to warm up and then stop allocating:

- `RowStore::reset` keeps its cell vector and each `Value`'s internal buffers.
- `RowStore::shrink` gives back memory after unusually large runs.
- `GroupTable` reuses `Group` and aggregate state objects and drops excessive
  retained groups after small runs.
- `JsonWriter` keeps its render plan and buffer and exposes `compact`.
- `Program::frame` persists on prepared statements and cached batches.
- `BatchCache` keeps recent one-off texts.
- `TxnImpl` objects, catalogs and function contexts are pooled.
- Source caches have a hard row cap (`kSourceCacheRows = 65536`).

Connection counters are atomic because a main handle and reader handles may run
transactions on different threads. A single `Database` or `Statement` object is
still not a concurrent-execution object; use one reader handle per thread.

An `ORDER BY` larger than the sort memory spills to a temporary file (§13.5).
Grouping, `DISTINCT`, compound intermediates, materialized sub-queries and
recursive CTE queues stay in memory.

---

## 24. Limitations and deliberate divergences

The target is SQLite 3.45.1 behavior plus documented libsql extensions, but
these differences are intentional or known:

1. `DATETIME` is a sixth storage class. `typeof(datetime-value)` returns
   `'datetime'`; it sorts between numbers and text.
2. `DATETIME` is accepted as a `STRICT` column type.
3. `FOR JSON` is a libsql extension.
4. `Database::exec` runs a batch without SQL transaction control in one
   transaction, rather than autocommitting each statement like `sqlite3_exec`.
5. Primary-key columns are always NOT NULL except the rowid alias.
6. Tables with declared primary keys are clustered by the declared key, so
   unordered scans often come out in key order. SQL still treats unordered row
   order as unspecified.
7. Rowid-keyed tables follow SQLite's one-past-largest rule and may reuse the
   top rowid after delete. Clustered rowid tables use a persisted hidden-rowid
   high-water mark. AUTOINCREMENT never reuses rowids.
8. Stores written by libsql 0.1 are refused, not upgraded.
9. `EXPLAIN` is a plan listing, not SQLite bytecode. `EXPLAIN QUERY PLAN`
   wording is approximate.
10. Double-quoted strings are never string literals (`DQS=0` behavior).
11. `CREATE VIRTUAL TABLE` modules are not implemented. The parser reports
    `Unsupported` naming the requested module.
12. `load_extension()` is not authorized.
13. Attached stores commit one after another, not atomically as one file.
14. Only `ORDER BY` spills to disk; other materializations stay in memory.
15. `BEGIN EXCLUSIVE` does not block readers; libnosql readers are snapshots.
16. `sqlite_schema.rootpage` is an object id for tables and indexes, not a
    SQLite page number.
17. `PRAGMA busy_timeout = 0`, the default, waits for the writer slot
    indefinitely rather than failing at once.
18. In-place `VACUUM` is a no-op; `VACUUM INTO` copies the store.

The engine does not implement SQLite's file format, C API, WAL/journal modes,
virtual table modules, extension loading or network protocols.

---

## 25. Rebuilding from scratch: suggested order

1. Implement `Value`, datetime parsing/formatting, affinity, strict coercion,
   comparison and collations.
2. Implement key and row encodings and property-test them against
   `compareValues`.
3. Implement the catalog format, schema objects, table/index builders and
   libnosql sub-database layout.
4. Build the lexer and parser with the limits in [§9.1](#91-parser-limits).
5. Implement the function registry and core deterministic functions required
   by schema validation.
6. Implement expression evaluation without sub-queries, then add sub-query
   execution and caching.
7. Implement SELECT planning for single-table scans, then joins, CTEs,
   aggregates, compounds, windows and `FOR JSON`.
8. Implement access paths and index maintenance.
9. Implement DML through `Writer`, including constraints, conflict algorithms,
   upsert, triggers and foreign keys.
10. Implement DDL and schema-table/pragma generation.
11. Add transaction management, statement atomicity, prepared statements,
    schema-stamp invalidation and public API wrappers.
12. Add the JSON renderer, CLI, conformance harness and long-run memory tests.

At each stage, compare against SQLite 3.45.1 where SQLite behavior is the
contract, and add divergence tests where libsql intentionally differs.

---

## 26. Design decisions and rejected alternatives

### 26.1 Store SQL, not a binary schema

The catalog stores normalized CREATE SQL and reparses it into schema objects.
That makes `sqlite_schema.sql` truthful and lets `ALTER TABLE` rewrite the same
source SQLite users see. The cost is extra parser use during schema load and
DDL, which is acceptable because schemas are cached by version.

### 26.2 Cluster declared primary keys

Tables with declared non-rowid primary keys are clustered by that key instead
of maintaining a separate primary-key index over rowid storage. This gives the
desired key-order locality and keeps primary-key lookups direct. The tradeoff
is that hidden rowids for such tables need a persisted high-water mark.

### 26.3 Use nested libnosql transactions for statement atomicity

The old alternative was to rely on "check everything before writing" for every
statement. That does not hold with triggers, cascades, REPLACE, upsert and
multi-row statements. Lazy nested transactions keep the common single-safe-row
path cheap while giving correct rollback semantics for complex writes.

### 26.4 Reuse plans with schema stamps

Plans are cached on prepared statements and recent one-off batches. A schema
pointer alone is insufficient inside a transaction because DDL edits the
transaction's schema clone. Stamps make invalidation precise without forcing a
full schema reload after every statement.

### 26.5 Encode numbers into one key domain

Using separate integer and real tags would make indexes disagree with SQLite
equality (`1` versus `1.0`). The `(sortable double, tail)` numeric encoding is
more complex but lets UNIQUE, ORDER BY and range scans use one bytewise key
order.

### 26.6 Keep most large intermediates in memory

Only `ORDER BY` spills to a temporary file, since it is the one intermediate
whose size routinely follows the table's. Grouping, `DISTINCT`, window
partitions, materialized sub-queries and recursive queues stay in memory,
which keeps them small and predictable, but large analytic queries can
consume memory proportional to their group, partition or queue size.

### 26.7 No SQLite bytecode VM

The executor is a C++ plan tree with nested loops, row stores and expression
trees, not a virtual machine. `EXPLAIN` therefore exposes a plan listing. This
keeps the engine compact but means tools expecting SQLite opcode listings will
not get them.

---

## 27. Glossary

**Access path**
The chosen way to read a table source: full scan, rowid lookup/range, clustered
primary-key lookup/range or secondary index lookup/range.

**Catalog**
The transaction-local object that opens `sql_catalog`, decodes schemas, writes
DDL changes and hands out table/index sub-database handles.

**Clustered table**
A table whose data sub-database key is its declared primary key instead of an
integer rowid. It may still have a hidden rowid in the payload if it is not
`WITHOUT ROWID`.

**Frame**
The flat `Value` array owned by a `Program`. Every source column, computed
value, aggregate/window result and pseudo row occupies a fixed slot.

**Program**
A planned statement: original text, owned AST, schema pins, frame and either a
`SelectPlan` or non-SELECT `ExecPlan`.

**RowStore**
A reusable flat row buffer used for materialized sub-queries, source caches,
sorts, compound SELECTs, window inputs and JSON collection.

**Schema stamp**
A process-wide monotonically increasing value assigned to a decoded schema
whenever DDL edits it. Plans compare both schema pointer and stamp.

**Session**
The per-execution view of connection state, open stores, catalogs, current
transaction properties and statement-atomicity hook.

**SourceNode**
A planned FROM item or pseudo source: table, materialized rows, table function,
recursive CTE row, single empty row or write pseudo row.

**Storage key**
The key in a table's data sub-database: either an 8-byte rowid key or encoded
declared primary-key components.

**Writer**
The DML helper that computes rows, enforces constraints, updates indexes,
fires triggers, applies foreign-key actions and writes table payloads.
