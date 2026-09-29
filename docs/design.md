# libsql: design and implementation reference

This document describes libsql completely enough that a person 
could rebuild it from scratch without reading the code: the problem it solves,
the shape of every layer, every on-disk byte, every algorithm, the semantics
that were chosen where SQL leaves room, the reasons behind those choices, the
tests that keep them honest, and the things deliberately left out.

It describes the library as it is in September 2026, after composite primary
keys were added. The companion document [`python/docs/design.md`](../python/docs/design.md)
covers the Python binding and the `BlobStorage` changes made for training-data
workloads; this one stops at the C++ engine, the command-line tool and the
test suite, and only points at the binding where the two meet.

Paths are relative to the `libsql` checkout. libnosql is the sibling checkout
at `../libnosql`.

---

## Contents

1. [Problem statement and goals](#1-problem-statement-and-goals)
2. [Architecture](#2-architecture)
3. [The storage engine underneath](#3-the-storage-engine-underneath)
4. [Values and types](#4-values-and-types)
5. [Encodings](#5-encodings)
6. [The catalog](#6-the-catalog)
7. [How tables and indexes are laid out in the store](#7-how-tables-and-indexes-are-laid-out-in-the-store)
8. [The lexer](#8-the-lexer)
9. [The parser and the AST](#9-the-parser-and-the-ast)
10. [Expression evaluation](#10-expression-evaluation)
11. [Binding and planning](#11-binding-and-planning)
12. [Scanning a source](#12-scanning-a-source)
13. [Constraints](#13-constraints)
14. [Statement execution](#14-statement-execution)
15. [The public API layer](#15-the-public-api-layer)
16. [Errors](#16-errors)
17. [The `sql` command-line tool](#17-the-sql-command-line-tool)
18. [Build system and layout](#18-build-system-and-layout)
19. [Testing](#19-testing)
20. [Long-run behaviour](#20-long-run-behaviour)
21. [Limitations and non-goals](#21-limitations-and-non-goals)
22. [Rebuilding from scratch: suggested order](#22-rebuilding-from-scratch-suggested-order)
23. [Design decisions and rejected alternatives](#23-design-decisions-and-rejected-alternatives)
24. [Glossary](#24-glossary)

---

## 1. Problem statement and goals

libnosql is a single-file, ACID, copy-on-write B+tree key/value store with
named sub-databases, one writer and many concurrent snapshot readers. It has no
notion of a row, a column, a type or a query. libsql adds exactly those on top
of it, so that an application can keep a relational catalogue in the same file
as its bulk data and query it with ordinary SQL, while inheriting the store's
guarantees rather than reimplementing them.

The goals, in priority order:

1. **Correctness under SQL semantics.** Three-valued logic, typed columns,
   NULL handling in constraints and aggregates, and stable results must match
   what a SQLite or PostgreSQL user expects for the supported subset. Where
   the subset is not supported the engine must say so with `Unsupported`,
   never misread a statement.
2. **Inherit the store's guarantees.** Every statement runs inside a libnosql
   transaction. Atomicity, durability, MVCC snapshots and single-writer
   serialisation come from below and are never re-derived above.
3. **Bounded resource use over long runs.** A process is expected to keep a
   database open for months. Nothing may grow with the number of statements
   run; a stale index entry is a bug of the same class as a wrong answer.
4. **Small.** The entire engine is about 4,500 lines of C++17 with no
   dependency beyond libnosql and the standard library. One person can hold
   the whole thing in their head. The planner is deliberately naive.
5. **Sharable store.** Other libnosql layers, `BlobStorage` in particular,
   must be able to live in the same file and commit in the same transaction
   as the rows describing their payloads.

Non-goals: performance parity with a real database, a network protocol,
concurrent writers, SQL completeness. See [§21](#21-limitations-and-non-goals).

---

## 2. Architecture

```
  application
     │  sql::Database / Transaction / Statement / Result   (include/sql/*.hpp)
     ▼
  src/database.cpp        opens the Env, owns transactions, parses, dispatches
     │
     ├─► src/internal/lexer.*      text → tokens
     ├─► src/internal/parser.*     tokens → std::vector<Statement>  (AST in ast.hpp)
     │
     ▼
  src/internal/executor.*  binds columns, plans access, walks the store,
     │                     enforces constraints, builds a Result
     ├─► src/internal/catalog.*    schema records, sub-database naming, rowid counter
     └─► src/internal/encoding.*   order-preserving key bytes, compact row bytes
              │
              ▼
        nosql::Env / Txn / Db      ordered byte-string keys, prefix and range scans
```

Data flows in one direction. The public layer never touches bytes; the
executor never touches text; the encoder never knows about tables. The
catalog is the only component that knows how sub-databases are named.

Everything in `src/internal/` is in namespace `sql::internal` and is not
installed. The three public headers are `sql/value.hpp`, `sql/error.hpp` and
`sql/sql.hpp`; the last includes the other two, so users include only it.

### 2.1 Life of a statement

`db.exec("SELECT ... WHERE id = ?", {Value(4)})`:

1. `Database::exec` checks the handle is open, tokenises and parses the text
   into a vector of statements, and asks whether any of them mutates.
2. It opens a libnosql write transaction if any statement mutates, otherwise
   a read transaction (a snapshot).
3. It builds a `Catalog` from that transaction. The catalog reads every
   table and index record once, so all planning inside the transaction sees
   one consistent schema.
4. For each statement it calls `execute`, which dispatches on the variant.
   For a SELECT this binds every column reference to a slot in a flat row
   buffer, numbers the aggregates, plans an access path per source table,
   runs the nested loops, groups or sorts as needed and fills a `Result`.
5. The transaction is committed (writer) or aborted (reader) and the last
   statement's `Result` is returned. Results own their values, so the
   transaction may end before the caller looks at them.

Anything that throws between steps 2 and 5 destroys the transaction without a
commit, which libnosql treats as an abort. A statement that fails part way
through a multi-row INSERT leaves nothing behind.

---

## 3. The storage engine underneath

libsql relies on a small subset of libnosql. Anyone porting the engine to a
different key/value store needs to provide exactly this:

| Need | libnosql call | Used for |
| --- | --- | --- |
| open/close a store, configure max size, read-only, durability | `Env::configure()...open(path)` | `Database::Options::open` |
| a write transaction, exclusive across the process | `Env::writeTxn()` | any mutating batch |
| a read transaction on a consistent snapshot | `Env::readTxn()` | any read-only batch |
| commit / abort | `Txn::commit()`, `Txn::abort()`, destructor aborts | end of a batch |
| named sub-databases, created on demand | `Txn::db(name, DbFlags::Create)` | catalog, one per table, one per index |
| existence check and drop of a sub-database | `Txn::hasDb`, `Txn::dropDb` | `Catalog` constructor, DROP TABLE/INDEX |
| point get with borrowed result | `Db::get(key) -> optional<Slice>` | key lookups, catalog reads |
| upsert and insert-if-absent | `Db::put(k, v, PutMode::Upsert / InsertUnique)` | rows, index entries, catalog |
| erase | `Db::erase(key)` | rows, index entries, catalog |
| iteration in byte order over all keys, a prefix, `[lo, end)`, `[lo, hi)` | `Db::all/prefix/from/between` | scans and windows |
| a `Slice` that views bytes without copying | `nosql::Slice`, `.view()` | everywhere |

Properties the engine depends on and that a replacement must preserve:

- **Keys compare as unsigned byte strings**, shortest-prefix-first. Every
  ordering guarantee in [§5.1](#51-key-encoding) is about `memcmp`.
- **Iteration sees the transaction's own writes.** UPDATE and DELETE
  materialise their match set before writing precisely because scanning
  while mutating would be undefined; INSERT does not scan.
- **`get` returns a borrowed view valid until the next write in the same
  transaction or the end of the transaction.** The executor decodes into
  owned `Value`s immediately.
- **The store rejects a write inside a read transaction.** libsql checks
  read-only-ness itself before running, but the store is the backstop.

Configuration libsql pins when it opens an `Env` (`src/database.cpp`):

| Option | Value | Why |
| --- | --- | --- |
| `maxDbs` | 512 | one sub-database per table plus one per index plus the catalog; 128 was too low for real schemas |
| `maxSize` | caller's, default 64 GiB | hard bound on the file; grows on demand |
| `sync` | `Durability::Safe` when `durable(true)` (default), `Durability::None` otherwise | `durable(false)` is for bulk loads and scratch stores |
| `readOnly`, `createIfMissing` | caller's | passed straight through |

The sub-database names `sql_catalog`, `tbl:*` and `idx:*` are reserved; other
layers sharing the store pick other names. libsql never lists sub-databases,
so a foreign one is simply never noticed.

Errors from the store arrive as `nosql::Error` and are not translated,
except `InsertUnique` failures, which libsql detects from the `false` return
of `put` and turns into `ConstraintViolation`.

---

## 4. Values and types

`include/sql/value.hpp`, `src/value.cpp`.

### 4.1 Storage classes

```cpp
enum class Type : uint8_t { Null = 0, Integer, Real, Datetime, Text, Blob };
```

| Class | C++ payload | Notes |
| --- | --- | --- |
| `Null` | `std::monostate` | the absence of a value; also the type of a column declared `NULL`, meaning "store anything as-is" |
| `Integer` | `int64_t` | every C++ integral type, `bool` included, lands here |
| `Real` | `double` | `float` widens |
| `Datetime` | `struct Datetime { int64_t usec; }` | microseconds since 1970-01-01T00:00:00Z; numeric order equals calendar order |
| `Text` | `std::string` | any bytes, compared byte-wise; UTF-8 by convention |
| `Blob` | `std::vector<std::byte>` | opaque bytes |

The enum order is the sort order of unlike classes and is also the index of
the `std::variant` inside `Value`, so `type()` is `Type(v_.index())`. That
coupling is intentional and noted in the header; do not reorder either.

Declared column type names accepted by `typeFromName` (case-insensitive):

| Spelling | Class |
| --- | --- |
| `NULL` | Null |
| `INT`, `INTEGER`, `BIGINT`, `SMALLINT` | Integer |
| `REAL`, `FLOAT`, `DOUBLE`, `NUMERIC` | Real |
| `TEXT`, `VARCHAR`, `CHAR`, `STRING` | Text |
| `DATETIME`, `TIMESTAMP`, `DATE` | Datetime |
| `BLOB`, `BINARY` | Blob |

A parenthesised width after a type name (`VARCHAR(255)`) is parsed and
discarded.

### 4.2 The `Value` class

A `Value` is one cell. Default-constructed it is NULL. Constructors exist for
every payload; `Value(const char*)` treats a null pointer as the empty string
rather than NULL, because a null `char*` reaching a database is almost always
a bug and silently storing NULL would hide it.

Accessors come in two flavours:

- **strict** (`integer()`, `real()`, `datetime()`, `text()`, `blob()`): the
  stored class must match or `TypeMismatch` is thrown. These are what the
  engine uses internally after it has established the type.
- **lenient** (`toInteger()`, `toReal()`, `toText()`, `toLiteral()`): convert.
  `toText()` renders NULL as the empty string, blobs as lowercase hex,
  datetimes as `YYYY-MM-DD HH:MM:SS[.ffffff]`, and reals with the shortest
  `%.{15,16,17}g` spelling that round-trips through `strtod`, so `1.5` prints
  as `1.5` and `0.1+0.2` prints as `0.30000000000000004` rather than lying.
  `toLiteral()` produces SQL source: `NULL`, `42`, `1.5`, `'it''s'`,
  `'2024-01-02 03:04:05'`, `x'00ff'`.

`truthy()` gives SQL truth for a WHERE clause: NULL is false, zero (integer,
real, or a datetime at the epoch) is false, empty text and empty blob are
false, everything else is true.

`operator==` is identity, not SQL equality: two NULLs are equal. It exists so
`Value` can sit in containers and tests can compare; the engine's comparisons
go through `compare`.

### 4.3 Ordering: `compare`

```cpp
int compare(const Value& a, const Value& b) noexcept;   // <0, 0, >0
```

Rank by class first: NULL (0) < Integer and Real together (1) < Datetime (2)
< Text (3) < Blob (4). Within a rank:

- Integer vs Integer: numeric. Real vs Real: numeric. Integer vs Real: the
  integer is widened to `double` and compared numerically. This is the one
  place unlike classes interleave, and it is why the key encoding, which
  cannot interleave them, is only safe because columns are typed
  ([§5.1](#51-key-encoding)).
- Datetime: by `usec`.
- Text: `std::string::compare`, which is unsigned byte order.
- Blob: `memcmp` over the common length, then by length.

Two NULLs compare equal here. Callers that need SQL semantics (where NULL
compared to anything is NULL) check `isNull()` first; `relation` in the
executor does exactly that.

### 4.4 Conversion: `cast`

`Value::cast(Type target)` is the single conversion routine. It is what an
INSERT applies on the way into a typed column, what a literal compared to a
column goes through (affinity, [§10.4](#104-affinity)), and what the lenient
accessors use. NULL passes through unchanged; so does a value already of the
target class; so does any value when the target is `Null`.

| From \ To | Integer | Real | Datetime | Text | Blob |
| --- | --- | --- | --- | --- | --- |
| Integer | — | `double(i)` | `Datetime{i}` (microseconds) | decimal | ✗ |
| Real | truncates toward zero; out of `int64` range ✗ | — | ✗ | shortest round-trip | ✗ |
| Datetime | `usec` | `double(usec)` | — | `YYYY-MM-DD HH:MM:SS[.ffffff]` | ✗ |
| Text | `strtoll`, whole string must parse | `strtod`, whole string must parse | `parseDatetime` | — | same bytes |
| Blob | ✗ | ✗ | ✗ | same bytes | — |

✗ throws `TypeMismatch` with a message naming both classes. Note the
asymmetries: text `'5'` becomes integer 5 but integer 5 does not become a
datetime through text; Real never becomes a Datetime (a double of
microseconds is a category error in practice); Blob and Text convert into
each other by reinterpretation, so `x'6869'` cast to TEXT is `hi`.

### 4.5 Datetime parsing and formatting

`parseDatetime` accepts, after trimming spaces and an optional trailing `Z`
or `z`:

```
YYYY-MM-DD
YYYY-MM-DD HH:MM
YYYY-MM-DD HH:MM:SS
YYYY-MM-DD HH:MM:SS.f{1,6}
```

with `T` or `t` allowed instead of the space. The fraction is right-padded to
six digits (`.5` is 500000 µs). Ranges checked: month 1–12, day 1–31, hour
≤ 23, minute ≤ 59, second ≤ 60 (leap second allowed). Day-of-month is not
validated against the month, so `2024-02-31` is accepted and normalises to
March 2. Years are four digits and proleptic Gregorian; the civil-day
arithmetic is Howard Hinnant's `days_from_civil` / `civil_from_days`, with a
floor division helper because `/` truncates toward zero for negative epochs.
No time zones are understood; everything is UTC.

`formatDatetime` always prints seconds and appends `.ffffff` only when the
fraction is nonzero, so a value parsed from `2024-01-02` prints as
`2024-01-02 00:00:00`.

`formatDatetimeIso` is the same instant as `YYYY-MM-DDTHH:MM:SS[.ffffff]Z`,
written into a caller-supplied buffer of at least 32 bytes and returning the
length. It exists so that the JSON writer ([§14.9](#149-for-json)) can render a
datetime without allocating a string for it; years are clamped to four digits
so the promised bound holds.

---

## 5. Encodings

`src/internal/encoding.hpp`, `src/internal/encoding.cpp`. Two independent
serialisations with different promises.

### 5.1 Key encoding

**Promise:** for any two values `a`, `b` of the same declared class,
`memcmp(encodeKey(a), encodeKey(b))` orders them exactly as `compare(a, b)`.
Encoded values concatenate without a separator into a composite key that
sorts as the tuple sorts (lexicographically, first component most
significant). Every encoded value is self-delimiting, so a composite can be
decoded or skipped component by component.

Each value is a one-byte tag followed by a payload:

| Tag | Class | Payload |
| --- | --- | --- |
| `0x01` | Null | none |
| `0x02` | Integer | 8 bytes big-endian of `uint64(i) XOR 0x8000000000000000` |
| `0x03` | Real | 8 bytes big-endian of the sortable transform below |
| `0x04` | Datetime | as Integer, over `usec` |
| `0x05` | Text | escaped bytes, see below |
| `0x06` | Blob | escaped bytes, see below |

The tags follow the class ranking of `compare`, so a NULL key component sorts
before any value and a text before any blob. Tags start at `0x01` so that no
encoded value begins with `0x00`; `0x00` and `0xff` are reserved as window
delimiters ([§12.2](#122-windows)).

**Integers.** Flipping the sign bit turns two's-complement order into
unsigned order: `INT64_MIN` becomes `0x00..00`, `-1` becomes `0x7f..ff`, `0`
becomes `0x80..00`. Big-endian so that `memcmp` reads the most significant
byte first.

**Reals.** IEEE-754 doubles sort correctly as sign-magnitude integers only
for non-negatives; negatives run backwards. The transform is:

```
bits = bit pattern of the double
if sign bit set:  ~bits            (invert everything: more negative → smaller)
else:             bits | signbit   (lift positives above every negative)
```

`-0.0` and `+0.0` encode differently (`-0.0` sorts just below `+0.0`), which
is harmless because they never collide as distinct stored keys in a typed
column and `compare` treats them as equal. NaN encodes to something ordered
after +inf; the engine never produces NaN keys because `cast` rejects
non-numeric text and arithmetic on NULL yields NULL.

**Text and blobs.** Variable length, so they need a terminator that sorts
below every content byte, and the content must not contain the terminator.
The scheme:

```
each byte b:   emit b;  if b == 0x00 also emit 0xff
end:           emit 0x00 0x00
```

A literal zero becomes `00 ff` and the terminator is `00 00`. Order survives
because after a shared prefix the next bytes are compared: a terminator `00
00` is below a continuation `00 ff` (the string with an embedded NUL is
longer, so sorts after the one that ended), and both `00`-prefixed forms are
below any ordinary byte `01..ff`. The decoder rejects `00` followed by
anything other than `00` or `ff` as corruption.

**Why this and not length-prefixing.** A length prefix destroys
lexicographic order (`"b"` would sort before `"aa"` because `1 < 2`). The
escape scheme costs one extra byte per embedded NUL and two per string, and
prefix scans over the encoded form correspond exactly to prefix predicates on
the value, which is what makes a leading-columns index lookup a byte-range
scan.

**The one gap.** `compare` interleaves Integer and Real numerically; the
tags `0x02` and `0x03` put every integer before every real. If a column could
hold both, `WHERE x > 1.5` through an index would miss integer 2. The engine
closes the gap by construction: every value written to a column is `cast` to
the declared class ([§13.1](#131-finalizerow)), and lookup bounds are cast to
the column's class before encoding ([§12.1](#121-encoding-a-bound)). A
column declared `NULL` (no type) can hold mixed classes, and for that reason
`NULL` columns get affinity `Type::Null`, meaning "no conversion", and the
planner's bounds are still correct because every candidate row is re-checked
by the predicate. The plan may be too wide but never too narrow.

**Helpers.**

- `appendKey(out, v)`, `encodeKey(v)`, `encodeKey(vector<Value>)`.
- `decodeKey(string_view& in)` consumes one component and returns it.
- `skipKeys(in, n)` consumes `n` components without materialising them; used
  to step past the indexed columns of an index entry to reach the primary
  key.
- `successor(prefix)`: the smallest byte string greater than every string
  starting with `prefix`. Increment the last byte that is not `0xff`,
  dropping trailing `0xff` bytes; if every byte is `0xff` return the empty
  string, which callers read as "no upper bound". Used to close a window
  that covers "everything starting with this prefix".

### 5.2 Row encoding

**Promise:** compact, self-describing, no ordering promise whatsoever. A row
is what sits in the value slot of a table's sub-database.

```
row     := varint(count) value*
value   := tag payload
tag     := uint8(Type)                        -- 0 Null, 1 Integer, 2 Real, 3 Datetime, 4 Text, 5 Blob
payload := (Null)      nothing
           (Integer)   varint(zigzag(i))
           (Datetime)  varint(zigzag(usec))
           (Real)      8 bytes, the double's in-memory representation
           (Text|Blob) varint(len) bytes
varint  := LEB128, little-endian base-128, high bit = continuation, ≤ 10 bytes
zigzag  := (i << 1) ^ (i >> 63)               -- small magnitudes stay short
```

The real is written with `memcpy`, so it is host-endian. All supported
targets are little-endian and a store file is not meant to travel between
architectures of different endianness; if that ever mattered, the fix is to
byte-swap in `putValue`/`getValue`, which touches nothing else.

`decodeRow` sanity-checks the count against the remaining bytes and every
reader throws `Internal("malformed record: ...")` on truncation or an unknown
tag. The executor pads a decoded row with NULLs up to the table's column
count (`columns.resize(t.columns.size())`), which is what would make adding a
trailing column cheap if `ALTER TABLE` were ever implemented.

The same primitives (`putVarint`, `putString`, `putValue`) serialise catalog
records ([§6.2](#62-record-formats)).

---

## 6. The catalog

`src/internal/catalog.hpp`, `src/internal/catalog.cpp`.

### 6.1 In-memory model

```cpp
struct Index {
    std::string name, table;
    std::vector<int> columns;     // positions in Table::columns, in index order
    bool unique = false;
};

struct Table {
    std::string name;
    std::vector<ColumnInfo> columns;   // public struct: name, type, primaryKey, notNull, unique, hasDefault, defaultValue
    std::vector<int> primaryKey;       // positions in key order; empty → implicit rowid
    std::vector<Index> indexes;

    int find(std::string_view column) const;   // case-insensitive; understands "rowid"; -1 if absent
    bool hasRowid() const;                     // primaryKey.empty()
    std::vector<int> keySlots() const;         // primaryKey, or {columns.size()} for rowid
    int autoKeySlot() const;                   // the slot an assigned key lands in
    Type slotType(int slot) const;             // declared type; Integer for the rowid slot
    bool autoKey() const;                      // rowid, or exactly one INTEGER key column
    std::size_t width() const;                 // columns.size() + (hasRowid() ? 1 : 0)
};
```

**Slots.** The executor works on a flat `std::vector<Value>` per table (a
"row buffer") whose layout is the declared columns in order, followed, for a
rowid table only, by one extra slot holding the rowid. `width()` is the
length of that buffer. In a join the buffers of the participating tables are
laid end to end and each table has a `base` offset ([§11.1](#111-sourceplan)).

**Keys.** `keySlots()` lists the slots whose encoded values, concatenated,
form the byte key a row is stored under. For a rowid table that is the single
extra slot; for a declared key it is the primary key positions in the order
they were declared in `PRIMARY KEY (a, b)`, which need not be column order.
`autoKey()` is true when the engine may hand out the key itself: a rowid, or
a single-column primary key of class Integer. Composite keys and non-integer
keys are never assigned.

The decoded schema is a `Schema`: an immutable snapshot holding the tables
sorted by lowercased name, each table settled with its sub-database name
(`dataDb`), rowid record key and position (`id`), and each index with its
sub-database name and a global position. Snapshots are shared through
`std::shared_ptr<const Schema>` and never modified; a version number persisted
in the catalog (the `'v'` record, §6.2) is bumped by every DDL statement and
identifies each snapshot.

A `Catalog` is constructed once per transaction from the `nosql::Txn&` and the
database's `SchemaCache`. It takes the cached snapshot without touching the
store when the transaction's snapshot is the commit at which the cache was last
known current -- every commit made through libsql records that -- and
otherwise reads the version record, reusing the cache when the version matches
and decoding privately when it does not (a reader on an older snapshot never
replaces what newer transactions use). Only this process writes the store, so
the in-memory notion of "current" is authoritative. Beside the snapshot the
`Catalog` holds the transaction's own `nosql::Db` handles, opened once per
table or index and indexed by position, and its rowid counters.

Mutating methods (`addTable`, `dropTable`, `addIndex`, `dropIndex`) copy the
snapshot on the first DDL of a transaction, edit the copy, write the records
and the incremented version, and let later statements in the same
transaction see the change. `committed(txnid)` after a successful commit
publishes the edited snapshot (or, for a plain write, marks the current one as
valid at the new commit); a rolled-back transaction publishes nothing.

Lookups: `find(name)` / `get(name)` (throws `NoSuchTable`) bisect the sorted
tables case-insensitively, `tableNames()` is sorted by lowercased name,
`findIndex(name)` and `allIndexes()` walk every table because index names are
global.

### 6.2 Record formats

Everything lives in one sub-database named `sql_catalog`. Keys are one
record-kind byte followed by the lowercased object name, so listing tables is
a prefix scan on `"t"`:

| Key | Value |
| --- | --- |
| `'t' + lower(table)` | table record |
| `'i' + lower(index)` | index record |
| `'r' + lower(table)` | `varint(last rowid handed out)`; absent means 0 |

**Table record, version 2 (written today):**

```
varint(2)                          format version
string(name)                       as declared, original case
varint(ncols)
ncols × {
  string(colname)
  uint8(Type)
  uint8(flags)                     bit0 NOT NULL, bit1 UNIQUE, bit2 has DEFAULT
  [value(default)]                 only if bit2; row-encoding `value`
}
varint(nkeys)
nkeys × varint(position)           primary key columns, in key order
```

**Table record, version 1 (still read):** identical up to and including the
columns, then a single `varint(position + 1)` where 0 means "no primary key".
The reader accepts either version and, for version 1, builds a one-element or
empty `primaryKey`. The next time the record is written (any schema change
touching that table; today only DROP TABLE removes it, so in practice a v1
record persists until the table is dropped) it is written as version 2.
`ColumnInfo::primaryKey` is not stored; it is set on load from the key list.

**Index record, version 1:**

```
varint(1)
string(name)
string(table)
varint(ncols)  ncols × varint(position)
uint8(unique)
```

A record with an unexpected version throws `Internal("catalog written by a
different libsql version")`. There is no migration machinery; a new version
is expected to keep reading the old ones, as version 2 does.

### 6.3 Sub-database naming

| Object | Sub-database |
| --- | --- |
| catalog | `sql_catalog` |
| rows of table `T` | `tbl:` + lower(`T`) |
| entries of index `I` | `idx:` + lower(`I`) |

Names are lowercased because identifiers are case-insensitive: `Book` and
`book` are the same table. Case is preserved in the record's `name` field for
display.

### 6.4 The rowid counter

`nextRowid(table)` reads the `'r'` record (0 if absent) the first time a
transaction asks, then works from a per-table counter held in the `Catalog`,
adds one and returns it. `noteRowid(table, used)` raises the counter to
`used` if `used` is larger and positive. INSERT calls `noteRowid` after every
row with an automatic key, so an explicitly supplied key of 100 makes the
next assigned one 101. `Catalog::flush()`, called after every statement,
writes a changed counter back as one `put`, so a 1,000-row INSERT costs one
record write rather than 3,000. Keys are never reused after DELETE, which is
what makes them safe to hand out as identities. The counter is a plain record
in the same transaction, so it rolls back with everything else.

---

## 7. How tables and indexes are laid out in the store

### 7.1 Rows

```
sub-database  tbl:<table>
key           encodeKey(row[k]) for k in keySlots(), concatenated
value         encodeRow(row[0 .. ncols-1])        -- the declared columns only
```

For a rowid table the key is the 9-byte encoded integer and the value holds
every declared column. For a declared primary key the key columns appear in
both the key and the value. That duplication costs a few bytes per row and
buys simplicity: `decodeRow` yields the whole declared row without having to
merge decoded key components back into their positions, and the row format
is independent of the key choice. The one exception is the rowid, which is
not a declared column and lives only in the key; `loadRow` decodes it from
the key into the extra slot.

Because keys are order-preserving, a table's rows are physically clustered
in primary-key order, which is what makes a leading-columns key predicate a
range scan ([§11.3](#113-planaccess)).

### 7.2 Index entries

```
sub-database  idx:<index>
key           encodeKey(row[c]) for c in index.columns  ++  <encoded primary key of the row>
value         empty
```

The primary key is appended so that entries for equal indexed values are
distinct and so that the row can be fetched. Reading an entry:
`skipKeys(view, index.columns.size())` leaves the view positioned on the
primary key bytes, which are used verbatim as the lookup key in `tbl:`.

NULLs are indexed (tag `0x01`), so a scan over an index visits NULL rows
first. The planner never generates a bound for NULL (a NULL pinned value
means "no rows", [§12.3](#123-key-access)), so those entries are reached
only by full index walks, which the planner does not do (a Scan reads the
table, not an index).

UNIQUE is enforced at insertion by scanning the prefix of the new entry's
indexed columns ([§13.2](#132-unique-indexes)); the store itself does not
know the index is unique.

### 7.3 Space and cost model

| Operation | Store operations |
| --- | --- |
| INSERT one row into a table with *n* indexes | 1 `put(InsertUnique)` + *n* `put`, plus for each unique index one prefix scan; plus 2 catalog reads/writes if the key is automatic |
| UPDATE one row | *n* `erase` + 1 `put` (or `erase` + `put(InsertUnique)` if the key changed) + *n* `put` |
| DELETE one row | *n* `erase` + 1 `erase` |
| point lookup by full key | 1 `get` |
| index lookup | one range iteration over `idx:` + one `get` per hit |
| key-prefix lookup | one range iteration over `tbl:` |
| full scan | one iteration over `tbl:` |

Every index entry adds roughly (sum of encoded index columns + encoded key +
B+tree overhead) bytes per row.

---

## 8. The lexer

`src/internal/lexer.hpp`, `src/internal/lexer.cpp`. `tokenize(text)` turns
the whole input into a `std::vector<Token>` ending in a `Tok::End`, throwing
`SyntaxError` with a byte offset on the first problem. The parser then works
on tokens only and never re-reads the source.

```cpp
enum class Tok { End, Identifier, Number, String, BlobLiteral, Parameter, Punct };
struct Token {
    Tok kind; std::string text; Value value; bool quoted; std::size_t offset;
    bool isKeyword(std::string_view w) const;   // unquoted Identifier equal ignoring case
    bool isPunct(std::string_view p) const;
};
```

**There is no keyword token kind.** Every bare word is an `Identifier`, and
the parser asks `isKeyword("SELECT")` where it expects a keyword. This is
what lets a column be called `order` when quoted: a quoted identifier never
matches a keyword. The cost is that the parser must decide, at a few places,
whether a bare identifier is an alias or the start of the next clause; a
small reserved-word list handles that ([§9.2](#92-reserved-words)).

Rules, in the order they are tried at each position:

1. Whitespace: skipped.
2. `--` to end of line and `/* ... */`: skipped. An unterminated block
   comment is an error.
3. `X'hex'` or `x'hex'`: a `BlobLiteral`. Even digit count, hex digits only.
   Checked before identifiers so that `x` alone still lexes as a name.
4. `[A-Za-z_][A-Za-z0-9_$]*`: `Identifier`, unquoted. `$` is allowed inside
   for compatibility with generated names.
5. `"..."` or `` `...` ``: quoted `Identifier`; a doubled delimiter is an
   escaped delimiter.
6. `[...]`: quoted `Identifier` (SQL Server style); no escape.
7. `'...'`: `String`, `''` escapes a quote. The `value` is Text.
8. Numbers: digits, optional `.digits`, optional exponent `e[+-]digits`; also
   `.5`. Integers are `strtoll`ed; on `ERANGE` the literal becomes a Real
   rather than wrapping. Anything with a point or exponent is a Real. No
   leading sign: `-` is a unary operator in the parser.
9. `?`: `Parameter`.
10. Two-character punctuation: `<> != <= >= == ||`.
11. One-character punctuation: `( ) , . ; * + - / % = < >`.
12. Anything else: `SyntaxError("unexpected character")`.

Helpers `equalsNoCase` and `toLower` live here and are used everywhere
identifiers are compared. They are ASCII-only on purpose; identifier
case-folding for non-ASCII letters is not attempted.

---

## 9. The parser and the AST

`src/internal/ast.hpp`, `src/internal/parser.hpp`, `src/internal/parser.cpp`.

```cpp
std::vector<Statement> parse(std::string_view text, std::size_t* parameters = nullptr);
bool mutates(const Statement&) noexcept;   // everything but SELECT
```

`parse` returns every `;`-separated statement in the text. Empty statements
(stray semicolons) are skipped; an input with no statement at all is a
`SyntaxError`. Parameters (`?`) are numbered left to right across the whole
text, so a two-statement batch with one `?` each takes two parameters.

### 9.1 The AST

Statements are a `std::variant` of eight plain structs; expressions are a
single `Expr` node type with a `kind` discriminator and every field any kind
might need. The flat design keeps the executor's evaluator a single switch
and avoids a class hierarchy for what is a dozen shapes.

```cpp
enum class ExprKind { Literal, Column, Parameter, Unary, Binary, IsNull, InList, Between, Like, Aggregate };
enum class UnaryOp  { Negate, Plus, Not };
enum class BinaryOp { Or, And, Eq, Ne, Lt, Le, Gt, Ge, Add, Sub, Mul, Div, Mod, Concat };
enum class AggregateOp { Count, Sum, Avg, Min, Max };

struct Expr {
    ExprKind kind;
    Value literal;                 // Literal
    std::string table, column;     // Column; table may be an alias or empty
    std::size_t parameter;         // Parameter: 0-based
    UnaryOp unary; BinaryOp binary;
    ExprPtr lhs, rhs;              // operands; Aggregate uses lhs (null for COUNT(*))
    std::vector<ExprPtr> list;     // InList items; Between {lo, hi}
    bool negated;                  // IS NOT NULL, NOT IN, NOT BETWEEN, NOT LIKE
    AggregateOp aggregate;
    // filled in by the planner:
    int slot = -1;                 // Column: index into the row buffer
    int aggregateSlot = -1;        // Aggregate: index into the group's accumulators
    Type columnType = Type::Null;  // Column: declared type, the affinity for comparisons
    std::string describe() const;  // "a.b", "COUNT(*)", "x + 1" -- names unaliased result columns
};
```

`describe()` renders an expression back to SQL-ish text and is used as the
default result-column name (`SELECT price * 2 FROM t` yields a column named
`price * 2`), for `ORDER BY` alias matching, and in error messages.

Statement structs:

| Struct | Fields |
| --- | --- |
| `CreateTableStmt` | `name`, `ifNotExists`, `columns` (`ColumnInfo` with inline constraints), `tablePrimaryKey` (names, key order), `uniques` (list of name lists) |
| `DropTableStmt` | `name`, `ifExists` |
| `CreateIndexStmt` | `name`, `table`, `columns` (names), `unique`, `ifNotExists` |
| `DropIndexStmt` | `name`, `ifExists` |
| `InsertStmt` | `table`, `columns` (empty = all, in order), `rows` (vector of vector of `ExprPtr`) |
| `SelectStmt` | `items` (`expr`/`alias`/`aliased`/`star`/`starTable`), `sources` (`table`/`alias`/`on`; first has no `on`), `where`, `groupBy`, `having`, `order` (`expr`/`descending`/`output`), `limit` (-1 = none), `offset`, `forJson` (optional `JsonOptions`) |
| `UpdateStmt` | `table`, `assignments` (name, expr), `where` |
| `DeleteStmt` | `table`, `where` |

The executor takes the statement by non-const reference because planning
writes `slot`, `aggregateSlot`, `columnType` and `OrderTerm::output` into
the tree, and because each statement carries its plan (`SelectStmt::plan`,
`InsertStmt::plan`, `UpdateStmt::plan`, `DeleteStmt::plan`, see
[§11.1](#111-sourceplan)). A prepared statement keeps its AST; it is
re-planned, overwriting those fields, whenever it runs against a schema
snapshot other than the one its plan was made for, which is why it keeps
working after a schema change.

### 9.2 Reserved words

Only for one purpose: deciding whether a bare identifier after an expression
or a table name is an alias. `SELECT a FROM t WHERE x` must not read `WHERE`
as an alias of `t`. The list:

```
SELECT FROM WHERE INSERT INTO VALUES UPDATE SET DELETE CREATE DROP TABLE INDEX ON
INNER LEFT RIGHT FULL OUTER CROSS JOIN ORDER GROUP HAVING BY LIMIT OFFSET AS AND OR
NOT IS IN BETWEEN LIKE ASC DESC NULL PRIMARY KEY UNIQUE DEFAULT IF EXISTS FOR
```

`FOR` is on the list only so that `FROM t FOR JSON AUTO` does not read `FOR` as
the table's alias; `JSON`, `AUTO`, `PATH` and the modifier words are not, since
they can only appear where nothing else may.

A quoted identifier is never reserved. `freshName()` is used for table,
index and alias names and refuses a reserved word; `identifier()` is used
for column names inside lists, where any bare word is fine.

### 9.3 Statement grammar

```
batch        := ( statement? ';' )* statement? 
statement    := createTable | dropTable | createIndex | dropIndex | insert | select | update | delete

createTable  := CREATE TABLE [IF NOT EXISTS] name '(' tableItem (',' tableItem)* ')'
tableItem    := columnDef
              | PRIMARY KEY '(' identifier (',' identifier)* ')'      -- at most once
              | UNIQUE '(' identifier (',' identifier)* ')'           -- any number
columnDef    := name typeName [ '(' anything ')' ] columnConstraint*
columnConstraint := PRIMARY KEY | UNIQUE | NOT NULL | NULL | DEFAULT constant
constant     := ['-' | '+'] ( Number | String | BlobLiteral | NULL )

dropTable    := DROP TABLE [IF EXISTS] name
createIndex  := CREATE [UNIQUE] INDEX [IF NOT EXISTS] name ON name '(' identifier (',' identifier)* ')'
dropIndex    := DROP INDEX [IF EXISTS] name

insert       := INSERT INTO name [ '(' identifier (',' identifier)* ')' ]
                VALUES '(' [expr (',' expr)*] ')' ( ',' '(' [expr (',' expr)*] ')' )*

select       := SELECT item (',' item)*
                FROM source ( (INNER JOIN | CROSS JOIN | JOIN) source [ON expr] )*
                [WHERE expr] [GROUP BY expr (',' expr)*] [HAVING expr]
                [ORDER BY expr [ASC|DESC] (',' expr [ASC|DESC])*]
                [LIMIT Integer] [OFFSET Integer] [forJson]
item         := '*' | name '.' '*' | expr [ [AS] alias ]
source       := name [ [AS] alias ]
forJson      := FOR JSON (AUTO | PATH) (',' jsonOption)*
jsonOption   := ROOT [ '(' String ')' ] | INCLUDE_NULL_VALUES | WITHOUT_ARRAY_WRAPPER

update       := UPDATE name SET identifier ('=' | '==') expr (',' identifier ('=' | '==') expr)* [WHERE expr]
delete       := DELETE FROM name [WHERE expr]
```

Notes and deliberate rejections:

- `CREATE UNIQUE TABLE` is a syntax error with a pointed message.
- `LEFT`, `RIGHT`, `FULL`, `OUTER` after the join list, and a comma after a
  source, throw `Unsupported` with a message naming the alternative. `CROSS
  JOIN` and bare `JOIN` are accepted as inner joins; a join without `ON` is a
  cross product.
- `LIMIT` and `OFFSET` take integer literals only (no parameters, no
  expressions); negative values are `InvalidArgument`.
- `FOR JSON` must name `AUTO` or `PATH`; `ROOT` without parentheses means the
  name `root`, and `ROOT` with `WITHOUT_ARRAY_WRAPPER` is `InvalidArgument`,
  as it is in T-SQL. `Item::aliased` records whether the alias was written
  out, which is how [§14.9](#149-for-json) tells `SELECT c.name` (property
  `name`) from `SELECT c.name AS "c.name"` (property `name` nested under `c`).
- In `createTable`, the table-level constraints are checked against the
  declared column names in the parser (`NoSuchColumn` on a miss). Duplicate
  names within a list and the interaction with inline `PRIMARY KEY` are the
  executor's business ([§14.1](#141-create-table)).
- A `DEFAULT` must be a signed literal; expressions are rejected because a
  default must be settled at CREATE time and there is nothing to evaluate
  against.
- `INSERT ... VALUES ()` with an empty tuple is accepted by the grammar and
  rejected by the executor unless the target column list is also empty,
  which it cannot be for a table with columns. In effect it errors with a
  count mismatch.

### 9.4 Expression grammar and precedence

Recursive descent, loosest binding first:

```
expr           := orExpr
orExpr         := andExpr (OR andExpr)*
andExpr        := notExpr (AND notExpr)*
notExpr        := NOT notExpr | comparison
comparison     := additive ( IS [NOT] NULL
                           | [NOT] IN '(' [expr (',' expr)*] ')'
                           | [NOT] BETWEEN additive AND additive
                           | [NOT] LIKE additive
                           | ('=' | '==' | '<>' | '!=' | '<' | '<=' | '>' | '>=') additive )*
additive       := multiplicative ( ('+' | '-' | '||') multiplicative )*
multiplicative := unary ( ('*' | '/' | '%') unary )*
unary          := ('-' | '+') unary | primary
primary        := '(' expr ')' | Number | String | BlobLiteral | '?'
                | NULL | TRUE | FALSE
                | name '(' ... ')'                     -- aggregate call
                | name | name '.' name
```

Points worth knowing:

- Comparison operators are left-associative and chain (`a = b = c` parses as
  `(a = b) = c`), matching SQLite.
- `NOT` between an operand and `IN`/`BETWEEN`/`LIKE` is handled inside
  `comparison` with backtracking: if `NOT` is consumed but none of the three
  follows, the parser rewinds so the outer `notExpr` can take it. That is the
  only backtrack in the parser.
- `BETWEEN`'s bounds are parsed at `additive` level so that the `AND`
  separating them is not swallowed by `andExpr`.
- `TRUE`/`FALSE` become integer literals 1 and 0.
- `||` is string concatenation at additive precedence.
- The only callable names are `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`. Anything
  else with a `(` is `Unsupported("unknown function")`. `COUNT(*)` is an
  `Aggregate` node with null `lhs`; `DISTINCT` inside an aggregate is
  `Unsupported`.
- A `Column` node records `table` (alias or table name, possibly empty) and
  `column`. Resolution happens at planning time.

---

## 10. Expression evaluation

`evaluate(expr, row, params, aggregates)` in `executor.cpp` returns a `Value`.
`row` is the flat buffer of the current candidate; `params` the bound
parameters; `aggregates` a pointer to the finished aggregate values of the
group being projected, or null wherever an aggregate is meaningless (WHERE,
ON, INSERT values, GROUP BY keys).

### 10.1 Node semantics

| Kind | Result |
| --- | --- |
| Literal | the literal |
| Column | `row[slot]`; an unbound slot (`-1`, as for a column reference inside INSERT values) or one outside the buffer is `NoSuchColumn("... is not available here")`. A slot that is inside the buffer but belongs to a join source not yet filled reads whatever is there: NULL on the first pass, a stale value later (see §21) |
| Parameter | `params[i]`, or `InvalidArgument` naming how many were bound |
| Aggregate | `(*aggregates)[aggregateSlot]`; with null `aggregates` throws `InvalidArgument("... is only allowed in SELECT, HAVING and ORDER BY")` |
| Unary Not | NULL → NULL; else `!truthy()` |
| Unary Plus / Negate | NULL → NULL; else numeric operand, negated for `-` |
| Binary And / Or | three-valued with short-circuit; see below |
| Binary Concat | NULL if either side NULL; else `a.toText() + b.toText()` |
| Binary comparison | NULL if either side NULL after affinity; else `compare` → 0/1 |
| Binary arithmetic | see §10.3 |
| IsNull | `isNull() != negated` — never NULL itself |
| InList | see §10.2 |
| Between | NULL if any of value/lo/hi NULL after affinity; else `lo <= v <= hi` xor negated |
| Like | NULL if value or pattern NULL; else `likeMatch` xor negated |

**AND / OR.** `a AND b`: evaluate `a`; if false return false without
evaluating `b`; evaluate `b`; if false return false; if either was NULL
return NULL; else true. `OR` mirrors with true. This gives the standard
Kleene tables and means a parameter-count error inside a short-circuited
branch is not raised, which matches most engines.

**Results of comparisons are Integer 0/1**, not a boolean class. `SELECT a =
b` returns an integer column.

### 10.2 IN

`v IN (list)`: NULL `v` → NULL. Each item is evaluated, given `v`'s column
affinity, and compared; the first equal item returns `!negated`. If no item
matched and some item was NULL the result is NULL, else `negated`. This is
the SQL standard result (`1 NOT IN (2, NULL)` is NULL, not true).

### 10.3 Arithmetic

Operands go through `numericOperand`: Integer and Real pass, Datetime
becomes its microsecond count, Text is tried as Integer then Real, anything
else is `TypeMismatch("cannot do arithmetic on ...")`. If both operands end
up Integer the operation is 64-bit integer arithmetic with wrap-around on
overflow (no check) and **division or modulo by zero yields NULL** rather
than trapping. Otherwise both are widened to double; `%` is `fmod`; `/ 0.0`
yields NULL rather than infinity.

### 10.4 Affinity

When a comparison, `IN` or `BETWEEN` has a `Column` on one side, the other
side is cast to that column's declared type before comparing. This is what
makes `WHERE released > '2024-01-01'` a datetime comparison and `WHERE id =
'4'` an integer one. The rules:

- `affinityOf(lhs, rhs)`: the left operand's `columnType` if it is a Column,
  else the right's, else `Type::Null` (no conversion). For a column vs
  column comparison of different types the left wins; both sides are then
  cast to that type.
- `applyAffinity(v, t)`: no-op for `t == Null`, NULL `v`, or a matching
  class; otherwise `v.cast(t)`, and **a failed cast is swallowed**. The
  value is left as it was and the comparison proceeds by class rank, so
  `id = 'abc'` on an integer column is simply false for every row rather
  than an error. This is important for the planner: a bound that fails to
  cast is dropped and the scan widens; the predicate still evaluates
  consistently.

`columnType` for the rowid slot is Integer; for a column declared `NULL` it
is `Type::Null`, so no affinity applies and mixed-class values compare by
rank.

### 10.5 LIKE

`likeMatch(pattern, text)`: `%` matches any run including empty, `_` matches
exactly one byte, everything else matches itself with ASCII case folding.
Implemented as the classic single-backtrack-point wildcard matcher (linear in
the common case, worst case O(n·m)). No escape character. Both sides are
rendered with `toText()`, so `LIKE` on an integer column works on its
decimal spelling.

### 10.6 `holds`

`holds(expr, row, params, aggregates)` is `!expr || evaluate(...).truthy()`.
A missing WHERE is true; a NULL result is false. Used for WHERE, ON and
HAVING.

---

## 11. Binding and planning

### 11.1 SourcePlan

One per table in a statement:

```cpp
struct SourcePlan {
    const Table* table; std::string alias; std::size_t base; Expr* on;
    enum class Kind { Scan, Key, Index } kind = Kind::Scan;
    const Index* index = nullptr;
    std::vector<const Expr*> equals;   // values pinning the leading key/index columns
    const Expr* low = nullptr;         // bound on the column after the pinned run
    const Expr* high = nullptr;
    std::string prefix, lo, hi;        // the encoded bounds of the scan in progress
};
```

The three strings are the scan's scratch: the bounds are encoded into them at
the start of each scan and stay put while the scan -- and any scan nested
inside it -- runs, so a join's inner probe allocates nothing for its window.

**Plans are cached.** Everything planning derives from the statement text
and the schema is kept on the statement: a `SelectPlan` (sources, output
expressions, column names, FOR JSON paths, aggregate numbering, and the row
buffer, sort keys and group key it reuses) or a `WritePlan` (the table,
target slots, the scanned source for UPDATE/DELETE and the row, key and
payload buffers). The plan carries the `shared_ptr<const Schema>` it was
made against and is reused exactly when the transaction's schema is the
same object, so a prepared statement plans once and re-plans only after a
DDL statement or when run against an older snapshot. The plan pins its
schema, so the `Table*` inside it stays valid however many DDL statements
happen elsewhere. Planning writes into the AST as before; those annotations
are stable for the same schema.

`base` is where this table's slots start in the shared row buffer. In
`SELECT ... FROM a JOIN b JOIN c`, `a` has base 0, `b` has base `a.width()`,
`c` has base `a.width() + b.width()`, and the buffer is the sum. A single-
table statement has one source at base 0.

### 11.2 Binding columns

`bindColumns(expr, sources)` walks an expression tree and, for every `Column`
node, calls `resolveColumn(sources, table, column)`:

- If the node has a table qualifier it must equal (case-insensitively) a
  source's alias or table name; other sources are skipped.
- Among the candidate sources, `Table::find(column)` locates the column
  (including `rowid` for rowid tables). If two candidates match, throw
  `AmbiguousColumn`; if none, `NoSuchColumn` with the qualified name.
- The slot is `base + local` and `columnType` is `slotType(local)`.

A qualifier matches either the alias or the underlying table name, so `SELECT
book.title FROM book b` works. Two sources with the same alias are rejected
in `runSelect` before binding.

### 11.3 planAccess

`planAccess(sp, available)` picks an access path for one source from a list
of conjuncts (the WHERE clause split on top-level `AND`, plus this source's
own ON clause split the same way). It fills three arrays indexed by the
table's local slot: `equal[k]`, `low[k]`, `high[k]`, each holding the
expression on the *other* side of a usable comparison, or null.

A conjunct is usable for slot `k` when:

1. it is a `Binary` node with operator `=`, `<`, `<=`, `>`, `>=` (not `<>`,
   not `IN`, not `BETWEEN`: the parser leaves `BETWEEN` as its own node, so
   `a BETWEEN 3 AND 5` is not used for planning — a known cheap improvement);
2. one side is a `Column` whose slot lies inside this source's `[base, base +
   width)`; if the column is on the right, the operator is mirrored;
3. the other side is *ready*: every column it references has a slot below
   `sp.base`, that is, belongs to a source earlier in the join order. Literals
   and parameters are always ready. This is what makes `ON b.a_id = a.id`
   into a lookup on `b` when `a` is outer: `a.id` is ready when `b` is
   planned.

Later conjuncts overwrite earlier ones for the same slot and operator class;
only one bound per direction is kept. Then the choice, in strict order:

```
keySlots   = t.keySlots()
keyPinned  = length of the longest leading run of keySlots with equal[] set
if keyPinned == keySlots.size():        → Key, equals = all key slots           (point lookup)
bestIndex  = the index with the longest leading run of pinned columns (> 0)
if keyPinned > 0 and keyPinned >= bestLeading:
                                        → Key, equals = leading run, low/high on the next key column
if bestIndex:                           → Index, equals = leading run, low/high on the next index column
if low or high on keySlots[0]:          → Key, no equals, low/high on the first key column
first index with low or high on its first column:
                                        → Index, no equals, low/high on that column
else                                    → Scan
```

Rationale for the order: a full-key match is one `get` and cannot be beaten.
A partial key run beats an index run of equal length because rows come
straight out of the `tbl:` window instead of via a second `get` per hit. A
longer index run beats a shorter key run because it is narrower. Ranges
without any equality are the last resort before a scan, and the key's range
is preferred over an index's for the same reason as above. There are no
statistics; "longest leading run" is the whole cost model. The planner never
combines two indexes, never uses an index for `ORDER BY`, and never considers
`OR`.

The plan is only ever allowed to be **too wide**: whatever it returns, the
full WHERE (and ON) is re-evaluated on every candidate row. This invariant is
what makes the planner safe to keep simple and is asserted by the test
suites' "indexed answer equals scanned answer" checks.

### 11.4 Join order

Sources are visited in the order written. There is no reordering. The inner
source of a nested loop is planned with the outer sources' columns available,
so the user controls the plan by writing the driving table first.

---

## 12. Scanning a source

`scanSource(cat, sp, row, params, visit)` fills `row[sp.base ...]` with each
candidate row of one source and calls `visit()`; it stops early and returns
false when `visit` returns false (used by LIMIT without ORDER BY).

### 12.1 Encoding a bound

`encodeBound(expr, targetType, row, params, out)` evaluates `expr` against the
current row (so outer-join columns are read from the buffer), returns false
for NULL, casts to `targetType`, appends the key encoding to `out`, and
returns false if the cast throws. A false return means "this bound cannot be
used"; the caller widens.

`encodePrefix(sp, columns, row, params, prefix)` does this for each of
`sp.equals` in turn with the matching column's type, and fails on the first
unusable one. `pinnedToNull(sp, row, params)` reports whether any pinned
value is NULL.

### 12.2 Windows

Given a `prefix` (the encoded pinned columns, possibly empty), optional
`low` and `high` expressions on the next column of type `nextType`:

```
lo = prefix + encode(low)    if low usable, else prefix
hi = prefix + encode(high) + 0xff   if high usable
   = successor(prefix)             otherwise ("everything with this prefix"); empty = unbounded
iterate [lo, hi)   -- or [lo, end) when hi is empty
```

Why `+ 0xff` on the high bound: the bound is inclusive (`<=`) at the byte
level, and both `<` and `<=` are treated as `<=` because the predicate is
re-checked. The next byte after an encoded component is either another
component's tag (`0x01..0x06`) or, for an index entry, the primary key's
tag, or nothing. Appending `0xff` produces a key greater than every entry
whose next-column value equals the bound and less than every entry whose
value is greater, because no encoded byte sequence after a complete
component begins with `0xff`. Text components end in `00 00`, so an
appended `0xff` after that also works.

Why `successor(prefix)` on the low side is not needed: `prefix` itself is
below every key beginning with `prefix`, so it is a correct inclusive lower
bound.

### 12.3 Key access

```
if kind == Key:
    if encodePrefix succeeds:
        if every key column is pinned:  get(prefix); load the row if present; done
        else: window over tbl: with prefix and low/high on keySlots[equals.size()]
              iterate, loadRow(key, payload) for each, visit
        done
    else if any pinned value is NULL: done with no rows   (NULL equals nothing)
    else fall through to a full scan                      (a bound that would not cast)
```

`loadRow(t, key, payload, row, base)` decodes the payload into
`row[base .. base+ncols)`, pads with NULL to the column count, and for a
rowid table decodes the key into `row[base + ncols]`.

### 12.4 Index access

```
if kind == Index and encodePrefix succeeds:
    window over idx: with prefix and low/high on index.columns[equals.size()]
    for each entry: skipKeys(view, ncols_indexed) → primary key bytes
                    payload = tbl.get(pk);  if absent, skip (defensive)
                    loadRow; visit
    done
```

An index entry with no row should never exist; skipping rather than throwing
keeps a read working if a bug ever leaves one behind, and the soak test's
consistency check ([§20](#20-long-run-behaviour)) is what catches it.

If the prefix fails to encode the code falls through to a full scan (there
is no NULL shortcut here; a NULL pinned index value falls to the scan, where
the predicate rejects everything — correct, just slower than it could be).

### 12.5 Full scan

`for (key, payload) in tbl.all(): loadRow; visit`. In primary-key order.

---

## 13. Constraints

### 13.1 finalizeRow

Called on every row about to be written (INSERT and UPDATE):

1. For each declared column, `row[i] = row[i].cast(column.type)`; a failing
   cast rethrows as `TypeMismatch` with `(column T.c)` appended.
2. If the column is `NOT NULL` and the value is NULL: `ConstraintViolation("NOT
   NULL constraint failed: T.c")`.
3. For a rowid table, cast the rowid slot to Integer.
4. For every key slot, NULL is `ConstraintViolation("PRIMARY KEY may not be
   NULL")`. In practice declared key columns already have `notNull` set at
   CREATE time, so step 2 fires first with the more specific message; this
   check is the backstop and the one that fires for a rowid that some caller
   set to NULL, which cannot happen through SQL.

Step 1 is what closes the integer/real gap in the key encoding: a column
never holds both.

### 13.2 Unique indexes

`addIndexEntry(cat, t, ix, row, keyBytes)`:

```
prefix = encode(row[c]) for c in ix.columns
if ix.unique and no indexed value is NULL:
    for each entry in idx.prefix(prefix):
        rest = entry after skipping ncols components   (the primary key)
        if rest != keyBytes: ConstraintViolation("UNIQUE constraint failed: T.a, T.b")
put(prefix + keyBytes, empty)
```

NULLs are exempt from uniqueness, as in SQL. The comparison against the
row's own key is what lets UPDATE re-insert an unchanged unique value
without colliding with itself: `removeIndexEntries` runs before
`addIndexEntries`, but the check is cheap insurance and also covers the
CREATE UNIQUE INDEX backfill, where the entry being added is for a row that
is already present.

The prefix scan visits only entries with exactly this indexed tuple because
the encoding is self-delimiting: `prefix` for `('a', 1)` cannot be a prefix
of the encoding of `('a', 10)` (the integer is fixed width) nor of `('ab',
1)` (the text terminator differs).

### 13.3 Primary key uniqueness

Enforced by the store: `put(key, payload, PutMode::InsertUnique)` returns
false if the key exists, and libsql throws `ConstraintViolation("PRIMARY KEY
must be unique: T")`. On UPDATE the old key is erased first when it changes,
so moving a row onto its own old key is fine and moving it onto another
row's key fails.

---

## 14. Statement execution

`execute(catalog, stmt, params)` dispatches on the variant to one `run*`
function. Each returns a `Result` built through `ResultBuilder`, the only
thing allowed to construct `Row`s and set `changes()` / `lastInsertId()`.

### 14.1 CREATE TABLE

1. If the table exists: return silently with `IF NOT EXISTS`, else
   `TableExists`.
2. No columns: `InvalidArgument`.
3. Walk the columns: duplicate name (case-insensitive) → `InvalidArgument`;
   a column named `rowid` → `InvalidArgument("'rowid' is reserved")`; an
   inline `PRIMARY KEY` when one was already seen, or when a table-level
   `PRIMARY KEY (...)` is also present → `InvalidArgument("a table may
   declare only one PRIMARY KEY")`; a `DEFAULT` is cast to the column's
   type now (so `DEFAULT 'x'` on an INTEGER column fails at CREATE, not at
   INSERT).
4. Resolve the table-level key list to positions: unknown name →
   `NoSuchColumn`; the same column twice → `InvalidArgument`. Set
   `primaryKey` to that list (or to the single inline position).
5. Every key column gets `primaryKey = true` and `notNull = true`.
6. Collect unique indexes to create: one per column-level `UNIQUE`, one per
   table-level `UNIQUE (...)`. Each is named `<table>_<col>[_<col>...]_unique`
   and skipped when its column set is a permutation of the primary key (the
   key already guarantees it), or when an index of the same name was already
   collected in this statement (a repeated `UNIQUE (c)`). If an index of that
   name already exists elsewhere in the database, `IndexExists`.
7. `cat.addTable(t)` writes the record and creates `tbl:`; then each
   collected index is added with `cat.addIndex`, which creates `idx:`.

A UNIQUE on a single column that is part of a composite key is *not*
redundant (it is stronger) and is kept.

### 14.2 DROP TABLE

Missing table: silent with `IF EXISTS`, else `NoSuchTable`. `cat.dropTable`
erases every index record and drops each `idx:` sub-database, erases the
table record and the rowid record, drops `tbl:`.

### 14.3 CREATE INDEX

Existing name: silent with `IF NOT EXISTS`, else `IndexExists`. The table
must exist. Each column name must be a declared column (not `rowid`;
`find` would return the rowid slot, which is rejected because it is `>=
columns.size()`). `cat.addIndex` records it, then every existing row is read
with a full scan and `addIndexEntry` is called for it, so a `CREATE UNIQUE
INDEX` over duplicate data fails with `ConstraintViolation` and the whole
statement, index record included, rolls back.

### 14.4 DROP INDEX

Missing: silent with `IF EXISTS`, else `NoSuchIndex`. Erases the record and
drops `idx:`. There is no way to drop the implicit `_unique` indexes other
than by name, which works.

### 14.5 INSERT

1. Resolve the target list: empty means every declared column in order;
   otherwise each name through `find` (so `rowid` is a legal target on a
   rowid table).
2. For each value tuple: count mismatch → `InvalidArgument`. Build a row
   buffer of `width()`: defaults first, then evaluate each target expression
   against an empty row (a column reference here fails with "not available
   here").
3. If `autoKey()` and the key slot is NULL: `row[slot] = nextRowid()`.
4. `finalizeRow`.
5. If `autoKey()`: `noteRowid(row[slot])` so the counter stays ahead of
   explicit values.
6. `key = rowKey(t, row)`; `put(key, encodeRow(declared columns),
   InsertUnique)` or `ConstraintViolation`; `addIndexEntries`.
7. `changes` counts rows; `lastInsertId` is the last automatic-key row's
   integer (0 if the table has a composite or non-integer key).

Multi-row INSERT is a loop over step 2–6 inside one transaction; a failure
on the third row rolls back the first two.

### 14.6 UPDATE

1. Bind the WHERE against a single source at base 0 and plan it
   (`singleSource`). Resolve every assignment target (`rowid` allowed on a
   rowid table) and bind its expression.
2. **Materialise** the match set: run the scan, keep a copy of every row for
   which WHERE holds. This is essential: the writes below would otherwise be
   visible to the scan producing them (a row whose key moves forward would
   be visited again).
3. For each matched `before`: copy to `after`; evaluate each assignment
   against `before` (so `SET a = b, b = a` swaps); `finalizeRow(after)`;
   `noteRowid` if `autoKey`.
4. `removeIndexEntries(before)`; if the key is unchanged, upsert the
   payload; else erase the old key and `put(InsertUnique)` the new one
   (`ConstraintViolation` on collision); `addIndexEntries(after)`.
5. `changes` = matched rows, whether or not anything actually differed.

### 14.7 DELETE

Plan and materialise as UPDATE, then for each row `removeIndexEntries` and
erase the key. `DELETE FROM t` with no WHERE walks and erases every row
(there is no truncate fast path; `DROP TABLE` + `CREATE TABLE` is that).

### 14.8 SELECT

The largest function. In order:

**Sources.** For each FROM/JOIN source, look up the table, reject a repeated
alias, assign `base` cumulatively. `width` is the total.

**Projection.** For each item: a non-star is bound and its alias recorded (the
parser already filled `alias` with `describe()` when none was given). A star
expands, per matching source, into one synthetic `Column` node per declared
column (never the rowid) with the slot pre-set; the result name is
`alias.column` when there is more than one source, else `column`. `t.*`
naming a source that is not in FROM is `NoSuchTable`.

**Binding** of WHERE, every ON, GROUP BY terms and HAVING.

**ORDER BY resolution.** A term that is a bare integer literal `n` selects
result column `n` (1-based; out of range is `InvalidArgument`). A term that
is an unqualified column name matching a result column's name (alias or
derived) selects that output. These set `OrderTerm::output` and are never
evaluated. Any other term is bound like an expression and evaluated against
the row (or group sample). This is how `ORDER BY total` works when `total`
is `SUM(amount) AS total`.

**Aggregates.** WHERE, ON and GROUP BY may not contain aggregates
(`InvalidArgument`). `collectAggregates` numbers every aggregate node in the
select list, HAVING and evaluated ORDER BY terms, left to right, storing the
number in `aggregateSlot`; a nested aggregate is `Unsupported`. The query is
*grouped* if it has GROUP BY, or any aggregate, or a HAVING.

**Planning.** The WHERE is split into conjuncts once. For each source, in
order, those conjuncts plus the source's own ON conjuncts are passed to
`planAccess`. A source's ON clause should reference only earlier sources'
columns (or its own). A reference to a later source is **not** detected: the
row buffer already has a slot for it, so the ON clause reads NULL on the
first evaluation and a stale value from a previous inner iteration after
that. `FROM a JOIN b ON b.y = c.z JOIN c ON ...` therefore returns nothing
rather than an error. Detecting this at bind time (a slot at or beyond the
next source's `base` inside an ON clause) would be a few lines; it has not
been done.

**Nested loops.** A recursive lambda `nest(depth)`:

```
nest(d):
    if d == sources.size():                      -- a complete candidate row
        if not holds(WHERE): continue
        if grouped:  fold the row into its group; continue
        projected = evaluate every output
        if sorted:   stash (sortKeys, projected)
        else:        emit unless still inside OFFSET; stop early once LIMIT+OFFSET rows were seen
    else:
        scanSource(sources[d], visit = { if holds(sources[d].on): nest(d+1) })
```

ON is checked as soon as the source it belongs to has been filled, WHERE only
for complete rows. For an inner join this is equivalent and prunes earlier.

**Grouping.** The group key is the concatenated key encoding of the GROUP BY
values, so groups are keyed by value not by text and NULL forms its own
group. Groups live in a `std::map<std::string, Group>`, which means they
come out in key-encoding order, which for typed columns is value order:
that is why "groups come out in key order unless ORDER BY says otherwise".
A `Group` holds the **first row seen** (`sample`) and one `Accumulator` per
numbered aggregate. A bare column outside GROUP BY reads from `sample`, the
documented "first row of its group" reading rather than an error.

`Accumulator` keeps `count` (rows for `COUNT(*)`, non-NULL inputs
otherwise), an integer sum and a real sum with a flag saying whether any
input was real, and `best` for MIN/MAX. `finish` yields: COUNT → count; SUM →
NULL if nothing counted, else the integer sum unless any input was real;
AVG → NULL or `realSum / count`; MIN/MAX → `best` (NULL if nothing seen).
Text and datetime MIN/MAX work through `compare`.

After the loops: if the query is grouped but has no GROUP BY and saw no rows,
one empty group is synthesised so `SELECT COUNT(*) FROM empty` returns 0 and
`SELECT SUM(x) FROM empty` returns NULL. With a GROUP BY an empty input
yields no rows. Each group is finished, HAVING is evaluated against the
sample row with the finished aggregates available, and surviving groups are
projected and stashed.

**Sorting and LIMIT.** Stashed rows are `stable_sort`ed by the sort keys,
comparing with `compare` and honouring per-term `DESC`. Stability makes
ties deterministic (scan order). Then `[offset, offset + limit)` is emitted.
In the unsorted, ungrouped case the rows were already emitted with LIMIT
applied during the scan and the function returned early.

**Result column names** are the aliases/derived names; duplicates are
allowed (`Row::at(name)` returns the first).

### 14.9 FOR JSON

A trailing `FOR JSON {AUTO|PATH} [, ROOT[('name')]] [, INCLUDE_NULL_VALUES]
[, WITHOUT_ARRAY_WRAPPER]` parses into `std::optional<JsonOptions>
SelectStmt::forJson`. `ROOT` together with `WITHOUT_ARRAY_WRAPPER` is rejected
at parse time, as T-SQL rejects it. `FOR` joins the reserved-word list, without
which `FROM t FOR JSON AUTO` would read `FOR` as the table's alias.

`runSelect` splits in two. `runSelectRows` is the function described above,
with one addition: an optional out-parameter that comes back holding the
**property path** of each result column. The paths are kept apart from the
column names because `ORDER BY` resolves against the names, which must not
move. `runSelect` then runs the rows through the JSON writer and replaces the
result with a single row of a single `TEXT` column named `json`.

Paths are built like this:

* The property is the column's own name when it was not aliased — `c.name`
  contributes `name`, not the `c.name` the result set labels it with, so a
  table only ever appears as a level of nesting. An explicit `AS` wins, which
  is what `SelectStmt::Item::aliased` records.
* Under `AUTO`, the property is prefixed with the aliases of sources 1..d,
  where d is the last source the expression reads (`deepestSource`). Source 0
  is the top of the document; literals and aggregates answer 0 and stay there.
* Under `PATH`, the path is the property as written, so the dots a caller put
  in an alias are the nesting.
* A star across a join keeps its `alias.column` naming in both modes;
  flattening it would collide on every column name the two tables share.

**The writer** (`src/json.cpp`, `include/sql/json.hpp`) turns `(Result, paths)`
into a document. It first builds a tree from the paths: node 0 is the row
object, a dot descends, and each node holds the result columns that sit
directly on it. The tree is cached, together with a copy of the paths it was
built from, and rebuilt only when the paths actually change — every
`string_view` in the plan points into that copy, which is what makes the cache
safe.

`PATH` walks the tree once per row, emitting nested objects. A nested object
whose whole subtree is NULL is left out along with its property, unless
`INCLUDE_NULL_VALUES` was asked for.

`AUTO` walks it recursively over row *ranges*. At a node, a run of consecutive
rows that agree on that node's own columns becomes one element, and each child
is emitted as an array rendered from exactly that run — so the rows already
agree on every enclosing object by construction, which is the whole of T-SQL's
AUTO rule. A node with no children never collapses: one row is one object
there, as it is in a plain result set. Because the grouping is over
*consecutive* rows, `ORDER BY` decides the nesting.

**Why it is written this way.** The engine is meant to stay up for months, so
the renderer is built to the same no-growth rule as the rest of it
(§20):

* Everything goes through a sink that either appends to a string or just counts
  bytes. Rendering runs the identical emit twice — count, reserve exactly,
  write — so the output buffer is grown once, to the right size, instead of
  doubling its way there and leaving the abandoned blocks behind. The counting
  pass touches no allocator at all.
* Nothing is formatted into a temporary `std::string`. Integers and reals go
  through `std::to_chars` into a stack buffer (with a `%.15g`/`%.16g`/`%.17g`
  round-trip search where the library predates floating-point `to_chars`),
  datetimes through `formatDatetimeIso`, which was added to `value.hpp` for
  this and writes into a 32-byte caller buffer, and blobs base64 four
  characters at a time. Text escaping emits the runs that need no escaping
  whole rather than a character at a time.
* `JsonWriter` is the reusable form: it keeps the output buffer and the plan,
  so a warmed-up writer allocates nothing per query. `appendTo` does the same
  onto a buffer the caller owns and recycles. The `FOR JSON` clause keeps one
  `static thread_local JsonWriter` for the life of the process, which is where
  the plan cache pays: the same prepared query re-renders without rebuilding
  anything.
* `compact()` exists for the one case the above does not cover — a single
  enormous document would otherwise pin its buffer forever.

`tests/test_soak.cpp::jsonRenderingSettles` holds this to the same standard as
the other workloads: measured over thousands of renders through both the clause
and a reused writer, live bytes, live blocks and allocator footprint all come
out at zero growth, and the writer's buffer never exceeds what the warm-up
already asked for.

Formatting is otherwise unremarkable: JSON numbers for integers and reals
(non-finite reals become `null`, which JSON has no other spelling for),
ISO-8601 in UTC for datetimes, base64 for blobs, and `TEXT` escaped and passed
through as UTF-8. An empty result renders as `[]` rather than as no rows,
which is where this parts company with T-SQL.

---

## 15. The public API layer

`src/database.cpp`. Three pimpl structs, all held by `shared_ptr`:

```cpp
struct DatabaseImpl  { nosql::Env env; std::filesystem::path path; bool readOnly; };
struct TxnImpl       { std::shared_ptr<DatabaseImpl> db; nosql::Txn txn; bool readOnly; };
struct StatementImpl { std::shared_ptr<DatabaseImpl> db; std::string text;
                       std::vector<internal::Statement> parsed; bool mutating; Params bound; };
```

**Ownership.** A `Transaction` and a `Statement` each hold a shared pointer
to the `DatabaseImpl`, so the store stays open while either exists even if
the `Database` object has been destroyed or `close()`d. `Database::close()`
only drops its own reference. This is what the Python binding leans on for
its "close is a request" semantics.

**`Database::exec(text, params)`**: open-check; parse; `mutating = any
statement mutates`; if mutating and the database is read-only →
`ReadOnly`; open `writeTxn()` or `readTxn()`; build a `Catalog`; run each
statement; commit or abort; return the last `Result`. An exception
propagates after the `Txn` destructor aborts.

**`Database::prepare(text)`**: parse once, remember `mutating` and allocate
`bound` with one NULL per `?`. `Statement::bind(i, v)` is 1-based and
range-checked; `bind(params)` requires the exact count; `clear()` resets to
NULL. `exec()` runs in its own transaction exactly like `Database::exec`;
`exec(txn)` runs inside the given transaction and rejects a mutating
statement in a read-only one. Planning happens on every run, against the
current catalog, so a prepared statement survives schema changes and picks
up new indexes. `sql()`, `parameters()`, `writes()` expose the metadata.

**`Database::begin()`** opens a write transaction (blocks on the store's
writer lock until any other writer finishes, including one in another
process). **`beginRead()`** opens a snapshot. `Transaction::exec` parses and
runs in that transaction, rejecting writes on a read-only one. `commit()`
commits (no-op for a reader) and invalidates the handle; `rollback()` and
the destructor invalidate it, which aborts the store transaction. Using a
finished transaction throws `BadTransaction`. `Transaction::txn()` exposes
the `nosql::Txn` for sharing with `BlobStorage`.

**`Result` and `Row`.** A `Result` owns its rows and a `shared_ptr` to the
column-name vector; each `Row` shares that pointer, so `row["name"]` works
after the `Result` is gone as long as the `Row` copy lives. Name lookup is
case-insensitive, linear, first match. `Row::at(i)` and `Result::at(i)`
range-check with `InvalidArgument`. `changes()` is rows inserted/updated/
deleted; `lastInsertId()` is described in [§14.5](#145-insert).

**Schema introspection.** `tables()` (names, sorted case-insensitively),
`table(name)` → `TableInfo { name, columns, primaryKey /*vector<int>*/ }`,
`indexes(table = "")` → `IndexInfo { name, table, columns /*names*/, unique
}` for all or one table. Each runs in a fresh read transaction.

**`Database::env()`** returns the store for co-tenants; `version()` returns
`"0.1.0"`.

**Thread safety.** None of the public objects are internally synchronised.
Different threads may use different `Transaction`s (libnosql serialises
writers and lets readers run concurrently); one `Statement` may not be run
from two threads at once because it owns its bound parameters.

---

## 16. Errors

`include/sql/error.hpp`: `sql::Error : std::runtime_error` with an
`ErrorCode`. Every failure the SQL layer detects throws this; storage
failures (`MapFull`, I/O, corruption detected by the store) arrive as
`nosql::Error` and are not wrapped, so callers that care distinguish them by
type.

| Code | Raised when |
| --- | --- |
| `SyntaxError` | lexer or parser could not read the text; message includes what was found and often the byte offset |
| `Unsupported` | valid SQL the engine does not implement: outer joins, unknown functions, DISTINCT, unknown type names |
| `InvalidArgument` | parameter count/index, value count vs column count, duplicate column/alias, negative LIMIT, bad ORDER BY position, aggregates in the wrong clause, malformed schema, closed handle |
| `NoSuchTable`, `TableExists`, `NoSuchColumn`, `AmbiguousColumn`, `NoSuchIndex`, `IndexExists` | schema resolution |
| `TypeMismatch` | a `cast` that cannot work: storing text in INTEGER, arithmetic on a blob, strict accessor on the wrong class, unparseable datetime |
| `ConstraintViolation` | NOT NULL, PRIMARY KEY (NULL or duplicate), UNIQUE |
| `BadTransaction` | a finished `Transaction` was used |
| `ReadOnly` | a write against a read-only database or read transaction |
| `Internal` | a malformed stored record or a catalog version mismatch; should never happen |

Messages are one line, lower-case, name the offending object (`T.c`), and
are meant to be shown to a user as-is.

---

## 17. The `sql` command-line tool

`tools/sql_tool.cpp`, target `sql_cli`, binary `sql`.

```
sql <database> [-c SQL]... [-f FILE]... [-i] [--mode table|csv|list] [--no-header] [--readonly]
```

Behaviour:

- Each `-c` and each `-f` file is one batch passed whole to `Database::exec`,
  so a file's statements share one transaction and fail together. Batches
  run in command-line order.
- With no `-c`/`-f`, or with `-i`, statements are read from stdin. Lines are
  accumulated until the buffer ends in a `;` that is outside quotes and
  comments (`complete()` tracks `'`, `"`, `` ` ``, `--`, `/* */`), then run
  as one batch. On a terminal a `sql>` / `...>` prompt is shown; from a pipe
  there is none. Remaining text at EOF is run.
- A line starting with `.` while no statement is pending is a dot command:
  `.tables`, `.schema [T]` (emits `CREATE TABLE` with inline constraints, a
  trailing `PRIMARY KEY (a, b)` for a composite key, and a `CREATE [UNIQUE]
  INDEX` per index, including the implicit `_unique` ones), `.indexes [T]`,
  `.mode`, `.headers on|off`, `.help`, `.quit`/`.exit`.
- Output modes: `table` (ASCII box, NULL shown as `NULL`), `csv` (RFC-4180
  quoting, NULL empty), `list` (`|`-separated, NULL empty). The default is
  `table` at the prompt and `list` when any `-c`/`-f` batch was given and no
  `--mode` was, so piped output is parseable. Every result with columns is
  followed by `N rows`; a result with changes by `N rows changed`.
- `--readonly` opens with `readOnly(true)` and `createIfMissing(false)`; a
  failure to open is reported and exits with status 2.
- An unknown option or a second positional argument is a usage error (exit
  1) and prints the usage text.
- Exit status: 0 ok, 1 usage error, 2 if any batch threw. Errors go to
  stderr as `sql: <message>`; the tool continues with the next batch or line.
  Errors inside the interactive prompt do not affect the exit status.

---

## 18. Build system and layout

```
libsql/
  CMakeLists.txt            library, options, install/export
  include/sql/{sql,value,error,json}.hpp
  src/{value,database,json}.cpp
  src/internal/{lexer,parser,ast,encoding,catalog,executor}.{hpp,cpp}
  tests/                    sql_test.hpp harness, test_main.cpp, test_*.cpp
  examples/                 quickstart.cpp, blobs.cpp, json.cpp
  tools/                    sql_tool.cpp
  python/                   pybind11 binding; see python/docs/design.md
  docs/design.md            this file
  README.md
```

CMake ≥ 3.16, C++17, 64-bit only (checked). Options: `SQL_BUILD_TESTS`,
`SQL_BUILD_EXAMPLES`, `SQL_BUILD_TOOLS` (all ON), `SQL_BUILD_PYTHON`,
`SQL_ENABLE_ASAN` (OFF). Warnings: `-Wall -Wextra -Wpedantic -Wshadow` (or
`/W4 /permissive-`), and the code builds clean under them.

libnosql is found as an installed package `nosql::nosql`; failing that the
sibling checkout at `SQL_NOSQL_DIR` (default `../libnosql`) is added as a
subdirectory with its own tests, examples and tools switched off. When
`SQL_BUILD_PYTHON` is on, `CMAKE_POSITION_INDEPENDENT_CODE` is set globally
before libnosql is added, because the extension module is a shared object.

The library target `sql` (alias `sql::sql`) is installed with an export set
and a generated `sqlConfig.cmake` that `find_dependency(nosql)`s. Public
include directory is `include/`; `src/` is private, so `internal/` headers
are not visible to users even in-tree.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`python/vendor.sh` copies `src/`, `include/` and the top-level
`CMakeLists.txt` of both libsql and libnosql into `python/_vendor/` so the
Python package can be built from `python/` alone. It must be re-run after
engine changes; `diff -rq src python/_vendor/libsql/src` verifies.

---

## 19. Testing

### 19.1 Harness

`tests/sql_test.hpp` + `tests/test_main.cpp`, no third-party dependency.
`TEST(name) { ... }` registers a function; each suite is its own executable
whose `main` calls `tst::runAll("suite")`, printing PASS/FAIL per case with
timing and returning nonzero on any failure. Macros: `CHECK(cond)`,
`CHECK_EQ(a, b)` (prints both sides through `tst::show`, which renders
`Value`s as literals), `CHECK_THROWS(expr, ErrorCode)` (fails on no throw
or on the wrong code and reports the actual message).

`tst::Scratch` creates a unique temp directory per test and removes it on
destruction; `scratch.open()` gives a fresh `Database`. `tst::only(result)`
returns the single cell of a 1×1 result as text; `tst::flatten(result)`
renders rows as `a|b;c|d` with `NULL` spelled out, which is how most
expectations are written.

### 19.2 Suites

| Suite | What it pins down |
| --- | --- |
| `types` | datetime round trips and chronological sorting, `Value` type reporting, casts following the declared type, class ranking in `compare`, every type stored and read back, text coerced into the declared column type, blobs with embedded zeros, large blobs |
| `ddl` | create/drop tables and indexes, `IF [NOT] EXISTS`, schema introspection, schema persistence across reopen, backfill, backfill refusing to break UNIQUE, malformed schemas, table-level single-column PK, **reading a version-1 table record** (rewrites a fresh record into the old layout through `db.env()` and checks it loads) |
| `dml` | integer key assignment, defaults and NULLs, NOT NULL and UNIQUE, duplicate keys, INSERT shape checks, UPDATE rewriting rows and moving the key, constraints on UPDATE, DELETE counts and index consistency, rowid tables, positional parameters, commit and rollback, read transactions seeing a snapshot and refusing writes, a failed statement rolling back its whole batch |
| `query` | columns and stars, result column naming, the comparison and pattern operators, NULL comparing to nothing, ORDER BY with LIMIT/OFFSET, arithmetic and concatenation, qualification by table and alias, syntax diagnostics, quoted identifiers escaping keywords, comments |
| `join` | joins on the inner table's key in either direction, per-table star expansion, WHERE narrowing a join, three-table joins, joins over an indexed column, CROSS JOIN, bad-name diagnostics, sorting and paging join results |
| `index` | "twins": the same rows in an indexed and an unindexed table, every query checked to give the same answer through both; edits keep them in agreement; mistyped bounds widen; unique indexes; composite index prefixes |
| `group` | whole-table aggregates, aggregating nothing, SUM keeping its type, GROUP BY one column, by expressions and several columns, GROUP BY without aggregates acting as DISTINCT, HAVING, ordering and paging groups, ORDER BY alias and position, aggregates inside expressions, bare columns reporting the group's first row, aggregates over a join, aggregates rejected in WHERE/ON/GROUP BY |
| `prepare` | metadata (`sql`, `parameters`, `writes`), repeated runs with fresh bindings, 1-based checked binding, unbound parameters as NULL, running inside a caller's transaction, refusing writes in read-only contexts, errors at prepare time, following the schema it is run against, outliving the `Database` handle |
| `blobs` | sharing the store with `nosql::BlobStorage` under one commit, `env()` refused once closed |
| `composite` | composite primary keys: schema reporting, persistence, wholeness and uniqueness, key lookups vs a rowid twin (point, prefix, prefix+range, first-column range, trailing-column-only, extra predicates, OR), NULL/mistyped bounds, edits through the key including collisions, secondary and unique indexes on keyed tables, joins on whole and partial keys, GROUP BY / ORDER BY / LIMIT, prepared statements, malformed key declarations, table-level `UNIQUE`, redundant `UNIQUE` elision, single-column keys unchanged |
| `json` | `FOR JSON AUTO` flat and nesting a join, `PATH` with dotted aliases, `INCLUDE_NULL_VALUES`, `ROOT` with and without a name, `WITHOUT_ARRAY_WRAPPER` and its clash with `ROOT`, empty results, ORDER/LIMIT/OFFSET, aggregates staying at the top, escaping and every storage class, base64 padding, clause diagnostics, `JsonWriter` reuse, exact measurement, explicit paths, a change of result shape, `FOR` reserved but quotable |
| `soak` | long-run stability; see §20 |

The "twins" technique (`test_index.cpp`, `test_composite.cpp`) is the
suite's most important idea: because the planner is only ever allowed to be
too wide, any query answered through an index or key must equal the same
query answered by a full scan. Each `agree()` also asserts the query matched
at least one row, so a typo cannot pass vacuously.

Run everything with `ctest --test-dir build`. An ASan/UBSan configuration
(`-DSQL_ENABLE_ASAN=ON`) is expected to pass clean as well.

### 19.3 Python tests

`python/tests/test_libsql.py` (unittest) covers the binding, including
`db.table()` reporting `primary_key` as a list. Run with
`PYTHONPATH=<build>/python python3 -m unittest discover -s python/tests`.

---

## 20. Long-run behaviour

`tests/test_soak.cpp`. The claim under test is "a fixed data size costs a
fixed amount of memory, forever". Three figures are measured:

- **live bytes and blocks**: `operator new`/`delete` in every plain form are
  replaced with versions that store the size ahead of the block and keep
  atomic counters, so the count is exact. All plain forms must be replaced
  together because the standard allows a block from `nothrow new` to come
  back through sized `delete`.
- **allocator footprint**: `mallinfo2().uordblks + hblkhd`, sampled
  separately so that fragmentation (heap grows while live bytes are flat)
  is caught.
- **file size** on disk.

Each test runs a warm-up phase to reach the plateau, samples, runs a much
longer measured phase (`LIBSQL_SOAK_SCALE`, default 1, multiplies it), samples
again, and requires the figures to be unchanged within a small fixed slack
that does not scale with iterations. Workloads: one-off `exec`, re-run
prepared statements, prepare-and-discard, bounded random insert/update/
delete churn, 512 KiB blob rewrite, join with GROUP BY, `FOR JSON` and a
reused `JsonWriter` over a join (§14.9), CREATE/load/DROP cycles, and a
growing-database test that requires the second half of the growth to cost no
more than the first (convergence, not flatness).

At every sample point `checkIndexesAgreeWithScan` runs `SELECT a, COUNT(*)
... GROUP BY a` and then, for each non-NULL `a`, `SELECT id FROM t WHERE a
= ?` (an index lookup) and requires the counts to match. A leaked or stale
index entry shows up here.

Under a sanitizer (`__SANITIZE_ADDRESS__` or the Clang feature macro) the
soak runs a short version and reports without asserting: the sanitizer
replaces the allocator whose figures are being measured, and there it is
hunting memory errors instead.

The two growths that are allowed and documented: a deeper tree dirties more
pages per commit and libnosql keeps some buffers for reuse (bounded by its
`bufferCache`), and a single transaction holds its dirty pages until commit
(bounded by `dirtyLimit`, which raises `OutOfMemory` rather than growing
without bound).

`benchmarks/bench_sql` (`README.md`, "Performance") measures the steady
state the soak asserts on: it counts the allocations each workload makes per
repetition, so a statement shape that starts allocating per row shows up as
a number, not a failed assertion hours later. The figures there are the ones
to compare against after a change to the executor or the catalog.

---

## 21. Limitations and non-goals

Reported as `Unsupported` (or `SyntaxError` where the grammar never gets
there):

- outer joins, comma joins, `USING`, `NATURAL`
- sub-queries anywhere, `UNION`/`INTERSECT`/`EXCEPT`, `DISTINCT` (in the
  select list or inside aggregates), `EXISTS`
- `ALTER TABLE`, views, triggers, foreign keys, `CHECK` constraints,
  collations, `AUTOINCREMENT` keyword (an INTEGER PRIMARY KEY behaves as
  such implicitly)
- scalar functions of any kind (`LOWER`, `LENGTH`, `COALESCE`, `CAST`, date
  functions); `CASE`
- `LIMIT`/`OFFSET` as parameters or expressions
- `INSERT ... SELECT`, `INSERT OR REPLACE`/`ON CONFLICT`, `RETURNING`
- multi-table UPDATE/DELETE, UPDATE with a FROM clause
- transactions in SQL text (`BEGIN`/`COMMIT` statements); use the API

Known semantic edges, kept deliberately:

- LIKE has no `ESCAPE` and is ASCII case-insensitive only.
- Integer arithmetic wraps on overflow; division by zero is NULL.
- Day-of-month is not validated against the month.
- A column selected outside `GROUP BY` yields the group's first row, not an
  error.
- `rowid` is not selectable on a table with a declared primary key.
- An ON clause that names a column of a *later* join source is not rejected;
  it evaluates against NULL or a stale value and typically matches nothing.
  Write joins so that each ON refers only to sources already listed.
- The planner uses one access path per source, chosen by leading-run length
  only; `BETWEEN`, `IN` and `OR` are never used for access; no index is used
  to satisfy `ORDER BY`.
- Store files are host-endian for reals in rows.

Non-goals: multiple writer processes, a wire protocol, query optimisation
beyond the above, SQL standard conformance.

---

## 22. Rebuilding from scratch: suggested order

Each step is independently testable and roughly matches how the code is
organised. Sizes are the current line counts, as a budget.

1. **Values** (`value.hpp/.cpp`, ~580 lines). `Type`, `Value` as a variant,
   `compare`, `cast` per the table in §4.4, datetime parse/format with
   Hinnant's algorithms, `toText`/`toLiteral`. Test round trips and the
   ordering table exhaustively; everything above depends on `compare` and
   `cast` being right.
2. **Encodings** (~380 lines). Key encoding with the tags, sign-bit flip,
   double transform, escaped bytes and `successor`; row encoding with
   varints and zigzag. Property-test: for random pairs of same-class values,
   `memcmp` order equals `compare`; concatenated tuples order
   lexicographically; every encode/decode round-trips; `skipKeys` lands on
   the right byte.
3. **Lexer** (~310 lines). Token kinds in §8, in the order given. Test
   comments, all quoting forms, numbers incl. `ERANGE`, blob literals, the
   two-character operators.
4. **AST and parser** (~1000 lines). Statement structs, the flat `Expr`,
   precedence climbing exactly as in §9.4 including the `NOT` backtrack and
   `BETWEEN` at additive level, the reserved-word alias rule, parameter
   numbering, `describe()`. Test by parsing and re-describing expressions.
5. **Catalog** (~380 lines). Record formats in §6.2 (write v2, read v1 and
   v2), naming in §6.3, the rowid counter, `Table` helpers in §6.1.
6. **Executor: evaluation** (§10). `evaluate`, `holds`, affinity, LIKE,
   arithmetic, three-valued AND/OR, IN/BETWEEN NULL rules.
7. **Executor: binding, planning, scanning** (§11–12). `SourcePlan`,
   `resolveColumn`, `planAccess` in the exact priority order, `encodeBound`,
   windows with the `0xff`/`successor` rule, key and index access with the
   NULL and fall-through behaviour, `loadRow`.
8. **Executor: constraints and statements** (§13–14). `finalizeRow`, unique
   check, CREATE TABLE with the composite-key and UNIQUE rules, INSERT with
   the rowid protocol, UPDATE/DELETE with materialised match sets, SELECT
   with stars, ORDER BY output resolution, aggregate numbering, nested
   loops, grouping by encoded key in a `std::map`, the empty-group rule,
   stable sort, LIMIT/OFFSET in both paths.
9. **Public layer** (`database.cpp`, ~370 lines). Pimpls, transaction
   selection by `mutates`, read-only checks, prepared statements, `Result`
   ownership, introspection, `env()`.
10. **Tool, tests, soak, docs.** Write the twins tests early; they catch
    planner mistakes nothing else does. Add the soak once the memory model
    is stable.

Two invariants to keep in mind throughout: *every value written to a typed
column has been cast to that type* (closes the key-encoding gap and keeps
indexes correct), and *every plan is re-checked by the full predicate*
(keeps the planner safe).

---

## 23. Design decisions and rejected alternatives

**One sub-database per table and per index, not one big keyspace.**
libnosql sub-databases are cheap and give free namespacing, per-table drop
in O(1), and per-table B+tree statistics. The alternative, prefixing every
key with a table id, would have saved handles but made DROP TABLE a scan.

**Rows stored as one encoded value under the encoded key, not one cell per
key.** Cell-per-key would allow partial reads but multiply store operations
per row by the column count; rows here are small metadata, so one payload
per row wins.

**Key columns duplicated in the payload.** Costs bytes; buys a row format
independent of the key and a `loadRow` with no merging. The rowid is the
exception because it is not a declared column.

**Catalog decoded once per schema version, shared as an immutable
snapshot.** It was re-read per transaction at first, which was simple and
correct but cost every statement the whole catalog. The store is
single-process, so a version record bumped by DDL plus "valid as of commit
N" bookkeeping makes cache validation free for the common case and one point
lookup otherwise, and copying the snapshot on the first DDL of a transaction
keeps rollback correct without touching what other transactions use.

**No keyword token kind.** Lets any word be a quoted identifier and keeps
the lexer trivial; the price is a short reserved-word list for alias
disambiguation, which is a well-understood SQL wart.

**Flat `Expr` node instead of a class hierarchy.** A dozen shapes, one
evaluator switch, no virtual dispatch, trivially deep-copyable if ever
needed. Slightly wasteful in memory per node; irrelevant at this scale.

**Planner annotations written into the AST, plan cached on the statement.**
Avoids a parallel plan tree and, since the plan is pinned to the schema
snapshot it was made against, re-planning happens only after DDL. The cost is
that a `Statement` is not shareable between threads, which the API documents.

**Materialise-then-write for UPDATE/DELETE.** The simplest way to be safe
against a scan seeing its own writes (Halloween problem). Memory is
proportional to the match set, which is acceptable for the intended
workloads and is exactly what the soak measures.

**Planner picks by leading-run length only.** No statistics to maintain, no
ANALYZE, deterministic plans a user can predict by reading the schema. The
"too wide never too narrow" rule makes it safe.

**Composite keys as concatenated order-preserving encodings, not a separate
index.** The store's ordering does the work: prefix predicates become
windows over the rows, and there is no second structure to keep in sync.
The one cost was making every place that touched "the key slot" iterate a
list of slots, and a catalog record format bump with backward-compatible
reading.

**Table-level `UNIQUE (...)` as a unique index rather than a key
constraint.** It reuses the existing machinery end to end, shows up in
introspection like any index, and can be dropped by name.

**`InsertUnique` from the store for primary keys, prefix scan for unique
indexes.** The store cannot know an index is unique, so the check has to be
above it; for the primary key the store's own atomic check is both cheaper
and race-free within the writer.

**Storage errors not wrapped.** A `nosql::Error` means the file or the
machine, a `sql::Error` means the statement; keeping the types distinct
tells the caller which.

**Rejected: LIMIT as a parameter.** Would be easy; left out to keep LIMIT
resolvable at parse time so the early-stop path needs no evaluation. Add if
needed.

**Rejected: using `BETWEEN` for planning.** The parser keeps it as one node
for correct NULL semantics; the planner could lower it to two bounds in a
few lines. Not done yet.

---

## 24. Glossary

- **affinity** — the declared type of a column, applied to the other side
  of a comparison before comparing.
- **autoKey** — a key the engine may assign: the rowid, or a single INTEGER
  primary key column.
- **base** — the offset of a source table's slots within the shared row
  buffer of a statement.
- **conjunct** — one operand of the top-level `AND` chain of a WHERE or ON
  clause.
- **key slots** — the row-buffer positions whose encoded values form the
  stored key.
- **pinned** — a column constrained by an equality whose other side is
  ready.
- **ready** — an expression whose column references all belong to sources
  earlier in the join order (or are literals/parameters).
- **row buffer** — `std::vector<Value>` holding one candidate row across
  every source of a statement.
- **rowid** — the implicit integer key of a table without a declared
  primary key; addressable in SQL, occupies the slot after the declared
  columns.
- **slot** — an index into the row buffer.
- **twins** — the testing technique of running every query against an
  indexed and an unindexed copy of the same data.
- **window** — a `[lo, hi)` byte range over a sub-database derived from a
  prefix and optional bounds.
